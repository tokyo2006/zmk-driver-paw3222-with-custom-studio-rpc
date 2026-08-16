/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file paw3222_relay.c
 *
 * @brief Split relay bridge for the `cormoran.paw3222` Studio RPC subsystem
 * (CONFIG_ZMK_PAW3222_SPLIT_RPC_RELAY, see DESIGN.md Phase F) -- enabled on
 * BOTH halves of a split keyboard:
 *
 *  - Peripheral role: receives a relayed `RelayRequest` (ZMK split relay
 *    event, identifier "pmq"), executes it against this half's own local
 *    PAW3222 devices via paw3222_request_exec_handle(), and relays a
 *    `RelayResponse` back (identifier "pmp").
 *  - Central role: paw3222_relay_dispatch_request() (called from
 *    src/studio/paw3222_handler.c for any request whose `source` is
 *    nonzero) relays the request out and immediately returns a
 *    DeferredResponse; when the matching `RelayResponse` relays back in, it
 *    is re-raised as a `PeripheralResponse` Studio notification (the same
 *    "custom notification" mechanism used by SetFrameStream's
 *    FrameStreamChunk). paw3222_relay_broadcast_request() is the
 *    fire-and-forget sibling used for GetInfo's `source = PAW3222_SOURCE_ALL`
 *    ("list every PAW3222 across the whole keyboard") -- every connected
 *    peripheral answers independently, sharing one request_id.
 *
 * Pattern (event struct shape, relay macros, subsystem-index lookup,
 * static-buffer notification encoding) copied from
 * zmk-feature-custom-settings' own custom_settings_handler.c, the reference
 * implementation of this exact bridge for its own RPC surface.
 *
 * Caveat: CONFIG_ZMK_SPLIT_RELAY_EVENT broadcasts a central-to-peripheral
 * relay event to every connected peripheral, not to one addressed
 * peripheral -- so with more than one peripheral, every peripheral executes
 * every relayed request (each correctly tagged with its own source on the
 * way back, via ZMK_RELAY_EVENT_HANDLE's `source_field_name` rewrite). This
 * is fine for the common single-peripheral split; see
 * CONFIG_ZMK_PAW3222_SPLIT_RPC_RELAY's Kconfig help.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zmk/event_manager.h>
#include <zmk/workqueue.h>

/* Every listener below is invoked synchronously on whichever thread ZMK core
 * happens to dispatch relay/split events from -- the system workqueue on
 * both the peripheral (split_svc_relay_event_from_central_work,
 * app/src/split/bluetooth/service.c) and the central
 * (peripheral_event_work, app/src/split/bluetooth/central.c). protobuf
 * decode/exec/encode for this subsystem's largest messages (~230 bytes) adds
 * enough stack depth through nanopb + paw3222_request_exec_handle() +
 * (peripheral only) ZMK core's own relay-out path to overflow the system
 * workqueue's default stack -- confirmed on real hardware (MPU fault / stack
 * overflow) with only CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN=240 in play, no
 * larger devices_count and no other system workqueue user competing for that
 * stack at the same moment. Growing CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE
 * "fixes" that, but bloats every other system workqueue consumer's headroom
 * requirement and lets a slow protobuf pass (or, worse, frame streaming's
 * ~2s-per-frame capture loop) block unrelated system workqueue work
 * (BLE housekeeping, HID reports, keyscan-deferred work, ...) system-wide.
 * Instead, every entry point below re-dispatches its own work onto ZMK
 * core's dedicated low-priority workqueue (zmk_workqueue_lowprio_work_q(),
 * app/src/workqueue.c -- already used by ZMK core itself for exactly this
 * reason, e.g. gatt_rpc_transport.c's notify_tx_work) before doing any
 * decode/exec/encode, so only CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE (its
 * own dedicated stack, shared only with other deliberately-deferred,
 * tolerant-of-latency ZMK work) needs to accommodate it. */

/* ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL() (invoked below) expands to a call
 * to zmk_split_central_send_relay_event() but -- unlike the peripheral
 * side's ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(), which pulls in
 * <zmk/split/peripheral.h> itself -- event_manager.h does not include
 * <zmk/split/central.h> for its declaration, so callers must. */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#endif

#include <cormoran/paw3222/paw3222.pb.h>
#include <cormoran/paw3222/paw3222_relay.h>
#include <cormoran/paw3222/paw3222_request_exec.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/studio/custom.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Matches the identifier stringified by ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran__paw3222, ...)
 * in paw3222_handler.c -- kept as a separate copy here (not shared via a
 * header) since it is only ever needed as a literal string for the
 * subsystem-index lookup below. */
#define PAW3222_SUBSYSTEM_IDENTIFIER_STRING "cormoran__paw3222"

/* nanopb generates a static worst-case encoded size for every message here
 * since every bytes/string/repeated field in paw3222.proto has an explicit
 * max_size/max_count (see paw3222.options) -- used to size the relay event
 * payload buffers below exactly, instead of guessing a constant. */
#define PAW3222_RELAY_REQUEST_PAYLOAD_MAX_SIZE cormoran_paw3222_RelayRequest_size
#define PAW3222_RELAY_RESPONSE_PAYLOAD_MAX_SIZE cormoran_paw3222_RelayResponse_size

struct zmk_paw3222_relay_request {
    uint8_t source;
    uint16_t size;
    uint8_t payload[PAW3222_RELAY_REQUEST_PAYLOAD_MAX_SIZE];
};

struct zmk_paw3222_relay_response {
    uint8_t source;
    uint16_t size;
    uint8_t payload[PAW3222_RELAY_RESPONSE_PAYLOAD_MAX_SIZE];
};

/* The *_SERIALIZE relay variants below put only each event's actually-encoded
 * `size` bytes on the wire, not the whole fixed-size struct -- so a 3-byte
 * GetInfo request no longer costs a full CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN
 * transfer, and frame streaming's many small FrameStreamChunk notifications each
 * cost only their real size (the BLE relay transport chunks by event_data_size).
 * The framework still serializes into a CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN
 * buffer, so the largest encoded payload must still fit it -- the serialize fn
 * bounds-checks at runtime and these assert it at build time (on the encoded
 * payload max, not sizeof(struct), since the struct itself is never relayed). */
BUILD_ASSERT(PAW3222_RELAY_REQUEST_PAYLOAD_MAX_SIZE <= CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN is too small for the paw3222 relay request "
             "payload -- raise it (see DESIGN.md Phase F / Kconfig help for "
             "ZMK_PAW3222_SPLIT_RPC_RELAY)");
BUILD_ASSERT(PAW3222_RELAY_RESPONSE_PAYLOAD_MAX_SIZE <= CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN is too small for the paw3222 relay response "
             "payload -- raise it (see DESIGN.md Phase F / Kconfig help for "
             "ZMK_PAW3222_SPLIT_RPC_RELAY)");
/* Hard transport ceiling, independent of CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN:
 * struct relay_event_header (zmk/split/transport/types.h) encodes a relayed
 * event's data size in a single `uint8_t event_data_size` wire field, so no
 * relayed event can ever exceed 255 bytes. If a paw3222 encoded payload could,
 * paw3222.options' GetInfoResponse.devices max_count must come down instead. */
BUILD_ASSERT(PAW3222_RELAY_REQUEST_PAYLOAD_MAX_SIZE <= 255,
             "the paw3222 relay request payload exceeds the split relay transport's 255-byte "
             "hard ceiling (relay_event_header.event_data_size is a uint8_t) -- reduce "
             "paw3222.options' GetInfoResponse.devices max_count");
BUILD_ASSERT(PAW3222_RELAY_RESPONSE_PAYLOAD_MAX_SIZE <= 255,
             "the paw3222 relay response payload exceeds the split relay transport's 255-byte "
             "hard ceiling (relay_event_header.event_data_size is a uint8_t) -- reduce "
             "paw3222.options' GetInfoResponse.devices max_count");
BUILD_ASSERT(PAW3222_RELAY_NOTIFICATION_PAYLOAD_MAX_SIZE <= 255,
             "the paw3222 relay notification payload exceeds the split relay transport's "
             "255-byte hard ceiling (relay_event_header.event_data_size is a uint8_t)");

ZMK_EVENT_DECLARE(zmk_paw3222_relay_request);
ZMK_EVENT_DECLARE(zmk_paw3222_relay_response);
ZMK_EVENT_IMPL(zmk_paw3222_relay_request);
ZMK_EVENT_IMPL(zmk_paw3222_relay_response);

/* Serialize/deserialize the pre-encoded protobuf in `payload` (its actual
 * `size` bytes), used by the *_SERIALIZE relay macros below. All three structs
 * share {source, size, payload[]}, so one macro defines both halves. Each
 * serialize fn is referenced only by the sending side for its direction (the
 * central sends requests, the peripheral sends responses/notifications), so
 * __maybe_unused keeps the unused half quiet on the opposite role. `source` is
 * NOT serialized -- the relay framework carries it and rewrites it on receive
 * (ev->source + 1) per ZMK_RELAY_EVENT_HANDLE_DESERIALIZE's source_field_name. */
#define PAW3222_RELAY_DEFINE_SERDES(type)                                                          \
    static int __maybe_unused type##_serialize(const struct type *ev, uint8_t *event_data,         \
                                               size_t max_size) {                                  \
        if (ev->size > max_size) {                                                                 \
            return -EMSGSIZE;                                                                      \
        }                                                                                          \
        memcpy(event_data, ev->payload, ev->size);                                                 \
        return (int)ev->size;                                                                      \
    }                                                                                              \
    static int __maybe_unused type##_deserialize(struct type *ev, const uint8_t *event_data,       \
                                                 size_t size) {                                    \
        if (size > sizeof(ev->payload)) {                                                          \
            return -EMSGSIZE;                                                                      \
        }                                                                                          \
        memcpy(ev->payload, event_data, size);                                                     \
        ev->size = (uint16_t)size;                                                                 \
        return 0;                                                                                  \
    }

PAW3222_RELAY_DEFINE_SERDES(zmk_paw3222_relay_request)
PAW3222_RELAY_DEFINE_SERDES(zmk_paw3222_relay_response)

ZMK_RELAY_EVENT_HANDLE_DESERIALIZE(zmk_paw3222_relay_request, pmq, source,
                                   zmk_paw3222_relay_request_deserialize);
ZMK_RELAY_EVENT_HANDLE_DESERIALIZE(zmk_paw3222_relay_response, pmp, source,
                                   zmk_paw3222_relay_response_deserialize);
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL_SERIALIZE(zmk_paw3222_relay_request, pmq, source,
                                                zmk_paw3222_relay_request_serialize);
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL_SERIALIZE(zmk_paw3222_relay_response, pmp, source,
                                                zmk_paw3222_relay_response_serialize);

/* --- Peripheral role: execute a relayed request, relay the response back --- */

/* Decodes `payload`, executes it via paw3222_request_exec_handle() against
 * this half's local devices, and fills `out_resp` (always -- an
 * unsupported/undecodable request produces an ErrorResponse, not a
 * function failure, matching this module's existing "never crash on a bad
 * request" style). Exposed as its own step (rather than inlined into the
 * event listener below) so the split-relay self-test can exercise it
 * directly without needing a real relay event. */
static int paw3222_relay_exec_request(const uint8_t *payload, size_t size,
                                      cormoran_paw3222_RelayResponse *out_resp) {
    cormoran_paw3222_RelayRequest relay_req = cormoran_paw3222_RelayRequest_init_zero;
    pb_istream_t istream = pb_istream_from_buffer(payload, size);
    if (!pb_decode(&istream, cormoran_paw3222_RelayRequest_fields, &relay_req)) {
        LOG_WRN("Failed to decode paw3222 relay request: %s", PB_GET_ERROR(&istream));
        return -EINVAL;
    }

    *out_resp = (cormoran_paw3222_RelayResponse)cormoran_paw3222_RelayResponse_init_zero;
    out_resp->request_id = relay_req.request_id;
    out_resp->has_response = true;

    if (!relay_req.has_request ||
        !paw3222_request_exec_handle(&relay_req.request, &out_resp->response)) {
        cormoran_paw3222_ErrorResponse err = cormoran_paw3222_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message),
                 "unsupported or missing relayed paw3222 request");
        out_resp->response.which_response_type = cormoran_paw3222_Response_error_tag;
        out_resp->response.response_type.error = err;
    }

    return 0;
}

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* Single static buffer/work item: the relay bridge already assumes one
 * in-flight relayed request at a time (matches the single-buffer/mutex
 * pattern used for notifications elsewhere in this file) -- central awaits
 * a DeferredResponse before a Studio client can issue another relayed call. */
static uint8_t relay_request_work_payload[PAW3222_RELAY_REQUEST_PAYLOAD_MAX_SIZE];
static size_t relay_request_work_payload_size;

static void relay_request_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    cormoran_paw3222_RelayResponse relay_resp;
    if (paw3222_relay_exec_request(relay_request_work_payload, relay_request_work_payload_size,
                                   &relay_resp) < 0) {
        return;
    }

    struct zmk_paw3222_relay_response resp_event = {.source = ZMK_RELAY_EVENT_SOURCE_SELF};
    pb_ostream_t ostream = pb_ostream_from_buffer(resp_event.payload, sizeof(resp_event.payload));
    if (!pb_encode(&ostream, cormoran_paw3222_RelayResponse_fields, &relay_resp)) {
        LOG_WRN("Failed to encode paw3222 relay response: %s", PB_GET_ERROR(&ostream));
        return;
    }
    resp_event.size = (uint16_t)ostream.bytes_written;

    raise_zmk_paw3222_relay_response(resp_event);
}

static K_WORK_DEFINE(relay_request_work, relay_request_work_handler);

static int on_paw3222_relay_request(const zmk_event_t *eh) {
    const struct zmk_paw3222_relay_request *ev = as_zmk_paw3222_relay_request(eh);
    if (!ev) {
        return 0;
    }

    size_t size = MIN(ev->size, sizeof(relay_request_work_payload));
    memcpy(relay_request_work_payload, ev->payload, size);
    relay_request_work_payload_size = size;
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &relay_request_work);
    return 0;
}

ZMK_LISTENER(paw3222_relay_request_exec, on_paw3222_relay_request);
ZMK_SUBSCRIPTION(paw3222_relay_request_exec, zmk_paw3222_relay_request);

#endif // !CONFIG_ZMK_SPLIT_ROLE_CENTRAL

/* --- Central role: dispatch a request out, turn a relayed response into a notification --- */

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* Resolve this subsystem's runtime index, needed to raise a custom Studio
 * notification tagged with the right subsystem_index. Pattern copied from
 * paw3222_handler.c's custom_subsystem_index()/_for_identifier() (itself
 * copied from custom_settings_handler.c) -- kept as its own copy here since
 * this file may be linked without paw3222_handler.c's internals being
 * exposed. */
static int custom_subsystem_index_for_identifier(const char *identifier, uint32_t *index) {
    if (!identifier) {
        return -ENOENT;
    }

    size_t subsystem_count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &subsystem_count);

    for (size_t i = 0; i < subsystem_count; i++) {
        struct zmk_rpc_custom_subsystem *custom_subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &custom_subsys);
        if (strcmp(custom_subsys->identifier, identifier) == 0) {
            *index = i;
            return 0;
        }
    }

    return -ENOENT;
}

static int custom_subsystem_index(void) {
    static int cached_index = -1;
    if (cached_index >= 0) {
        return cached_index;
    }

    uint32_t index;
    int ret = custom_subsystem_index_for_identifier(PAW3222_SUBSYSTEM_IDENTIFIER_STRING, &index);
    if (ret < 0) {
        return ret;
    }

    cached_index = (int)index;
    return cached_index;
}

static uint32_t next_relay_request_id = 1;

/* Encodes `req` into a RelayRequest and raises it for CENTRAL_TO_PERIPHERAL
 * relay (see the macro invocations above) -- shared by
 * paw3222_relay_dispatch_request() (single-target, produces a
 * DeferredResponse) and paw3222_relay_broadcast_request() (fire-and-forget,
 * every connected peripheral answers independently). Returns the assigned
 * request_id (nonzero) on success, or 0 on encode failure (logged). */
static uint32_t send_relay_request(const cormoran_paw3222_Request *req) {
    uint32_t request_id = next_relay_request_id++;

    cormoran_paw3222_RelayRequest relay_req = cormoran_paw3222_RelayRequest_init_zero;
    relay_req.request_id = request_id;
    relay_req.has_request = true;
    relay_req.request = *req;

    struct zmk_paw3222_relay_request event = {.source = ZMK_RELAY_EVENT_SOURCE_SELF};
    pb_ostream_t ostream = pb_ostream_from_buffer(event.payload, sizeof(event.payload));
    if (!pb_encode(&ostream, cormoran_paw3222_RelayRequest_fields, &relay_req)) {
        LOG_WRN("Failed to encode paw3222 relay request: %s", PB_GET_ERROR(&ostream));
        return 0;
    }
    event.size = (uint16_t)ostream.bytes_written;

    raise_zmk_paw3222_relay_request(event);
    return request_id;
}

void paw3222_relay_dispatch_request(uint32_t source, const cormoran_paw3222_Request *req,
                                    cormoran_paw3222_Response *resp) {
    ARG_UNUSED(source); /* Transport broadcasts to every peripheral -- see file doc comment. */

    uint32_t request_id = send_relay_request(req);
    if (request_id == 0) {
        cormoran_paw3222_ErrorResponse err = cormoran_paw3222_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "failed to encode relay request");
        resp->which_response_type = cormoran_paw3222_Response_error_tag;
        resp->response_type.error = err;
        return;
    }

    cormoran_paw3222_DeferredResponse deferred = cormoran_paw3222_DeferredResponse_init_zero;
    deferred.request_id = request_id;
    resp->which_response_type = cormoran_paw3222_Response_deferred_tag;
    resp->response_type.deferred = deferred;
}

uint32_t paw3222_relay_broadcast_request(const cormoran_paw3222_Request *req) {
    return send_relay_request(req);
}

static K_MUTEX_DEFINE(peripheral_response_notification_lock);
static cormoran_paw3222_Notification peripheral_response_notification;

static bool encode_paw3222_notification_payload(pb_ostream_t *stream, const pb_field_t *field,
                                                void *const *arg) {
    const cormoran_paw3222_Notification *notification = (const cormoran_paw3222_Notification *)*arg;
    return zmk_rpc_custom_subsystem_encode_response_payload(
        stream, field, cormoran_paw3222_Notification_fields, notification);
}

static int raise_paw3222_notification(cormoran_paw3222_Notification *notification) {
    int index = custom_subsystem_index();
    if (index < 0) {
        return index;
    }

    pb_callback_t payload = {
        .funcs.encode = encode_paw3222_notification_payload,
        .arg = (void *)notification,
    };

    return raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = (uint8_t)index,
        .encode_payload = payload,
    });
}

static uint8_t relay_response_work_source;
static uint8_t relay_response_work_payload[PAW3222_RELAY_RESPONSE_PAYLOAD_MAX_SIZE];
static size_t relay_response_work_payload_size;

static void relay_response_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    cormoran_paw3222_RelayResponse relay_resp = cormoran_paw3222_RelayResponse_init_zero;
    pb_istream_t istream =
        pb_istream_from_buffer(relay_response_work_payload, relay_response_work_payload_size);
    if (!pb_decode(&istream, cormoran_paw3222_RelayResponse_fields, &relay_resp)) {
        LOG_WRN("Failed to decode paw3222 relay response: %s", PB_GET_ERROR(&istream));
        return;
    }

    k_mutex_lock(&peripheral_response_notification_lock, K_FOREVER);

    peripheral_response_notification =
        (cormoran_paw3222_Notification)cormoran_paw3222_Notification_init_zero;
    peripheral_response_notification.which_notification_type =
        cormoran_paw3222_Notification_peripheral_response_tag;
    cormoran_paw3222_PeripheralResponse *pr =
        &peripheral_response_notification.notification_type.peripheral_response;
    /* Stashed source was rewritten by ZMK_RELAY_EVENT_HANDLE's receive-side
     * `source_field_name = ev->source + 1` to the relaying peripheral's
     * slot + 1 -- exactly the addressing convention this module documents
     * for `source` elsewhere (0 = local/central, N = peripheral slot N). */
    pr->source = relay_response_work_source;
    pr->request_id = relay_resp.request_id;
    pr->has_response = relay_resp.has_response;
    if (relay_resp.has_response) {
        pr->response = relay_resp.response;
    }

    int ret = raise_paw3222_notification(&peripheral_response_notification);
    if (ret) {
        LOG_WRN("Failed to raise paw3222 PeripheralResponse notification: %d", ret);
    }

    k_mutex_unlock(&peripheral_response_notification_lock);
}

static K_WORK_DEFINE(relay_response_work, relay_response_work_handler);

static int on_paw3222_relay_response(const zmk_event_t *eh) {
    const struct zmk_paw3222_relay_response *ev = as_zmk_paw3222_relay_response(eh);
    if (!ev) {
        return 0;
    }

    size_t size = MIN(ev->size, sizeof(relay_response_work_payload));
    relay_response_work_source = ev->source;
    memcpy(relay_response_work_payload, ev->payload, size);
    relay_response_work_payload_size = size;
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &relay_response_work);
    return 0;
}

ZMK_LISTENER(paw3222_relay_response_notify, on_paw3222_relay_response);
ZMK_SUBSCRIPTION(paw3222_relay_response_notify, zmk_paw3222_relay_response);

#endif // CONFIG_ZMK_SPLIT_ROLE_CENTRAL

/* --- native_sim-only self-tests: exercise the relay logic without a --- */
/* --- real transport (native_sim cannot simulate one) --- */

#if IS_ENABLED(CONFIG_ZMK_PAW3222_SPLIT_RPC_RELAY_TEST) &&                                         \
    !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static int paw3222_split_relay_test_init(void) {
    cormoran_paw3222_RelayRequest relay_req = cormoran_paw3222_RelayRequest_init_zero;
    relay_req.request_id = 42;
    relay_req.has_request = true;
    relay_req.request.which_request_type = cormoran_paw3222_Request_get_info_tag;
    relay_req.request.request_type.get_info.source = 1;

    uint8_t payload[PAW3222_RELAY_REQUEST_PAYLOAD_MAX_SIZE];
    pb_ostream_t ostream = pb_ostream_from_buffer(payload, sizeof(payload));
    if (!pb_encode(&ostream, cormoran_paw3222_RelayRequest_fields, &relay_req)) {
        LOG_ERR("Split relay test: failed to encode synthetic request: %s", PB_GET_ERROR(&ostream));
        return -EIO;
    }

    cormoran_paw3222_RelayResponse relay_resp;
    int ret = paw3222_relay_exec_request(payload, ostream.bytes_written, &relay_resp);
    if (ret < 0) {
        LOG_ERR("Split relay test: exec failed: %d", ret);
        return ret;
    }

    if (relay_resp.request_id != 42) {
        LOG_ERR("Split relay test: request_id mismatch: got %u", relay_resp.request_id);
        return -EINVAL;
    }
    if (relay_resp.response.which_response_type != cormoran_paw3222_Response_get_info_tag) {
        LOG_ERR("Split relay test: expected a GetInfoResponse, got response type %d",
                relay_resp.response.which_response_type);
        return -EINVAL;
    }

    printk("PASS: paw3222_split_relay device_count=%u\n",
           (unsigned int)relay_resp.response.response_type.get_info.devices_count);

    /* Genuinely unsupported/malformed relayed request (no request_type set
     * at all): paw3222_request_exec_handle() returns false, which must
     * still produce an ErrorResponse, not a crash or an unfilled response. */
    cormoran_paw3222_RelayRequest empty_relay_req = cormoran_paw3222_RelayRequest_init_zero;
    empty_relay_req.request_id = 43;
    empty_relay_req.has_request = true;
    ostream = pb_ostream_from_buffer(payload, sizeof(payload));
    if (!pb_encode(&ostream, cormoran_paw3222_RelayRequest_fields, &empty_relay_req)) {
        LOG_ERR("Split relay test: failed to encode empty request: %s", PB_GET_ERROR(&ostream));
        return -EIO;
    }
    ret = paw3222_relay_exec_request(payload, ostream.bytes_written, &relay_resp);
    if (ret < 0) {
        LOG_ERR("Split relay test: empty-request exec failed: %d", ret);
        return ret;
    }
    if (relay_resp.response.which_response_type != cormoran_paw3222_Response_error_tag) {
        LOG_ERR("Split relay test: expected an ErrorResponse for an unset request kind, got "
                "response type %d",
                relay_resp.response.which_response_type);
        return -EINVAL;
    }

    printk("PASS: paw3222_split_relay_unsupported_kind\n");
    return 0;
}

SYS_INIT(paw3222_split_relay_test_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif // CONFIG_ZMK_PAW3222_SPLIT_RPC_RELAY_TEST && !CONFIG_ZMK_SPLIT_ROLE_CENTRAL

#if IS_ENABLED(CONFIG_ZMK_PAW3222_SPLIT_RPC_RELAY_TEST) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* Central-side counterpart: asserts paw3222_relay_broadcast_request()
 * (the GetInfo{source: PAW3222_SOURCE_ALL} broadcast path) assigns
 * distinct, nonzero request_ids -- see tests/split_central. Does not (and
 * cannot, without a real peripheral) assert that a broadcast actually
 * reaches anyone; that is exactly what native_sim cannot simulate. */
static int paw3222_split_relay_central_test_init(void) {
    cormoran_paw3222_Request req = cormoran_paw3222_Request_init_zero;
    req.which_request_type = cormoran_paw3222_Request_get_info_tag;
    req.request_type.get_info.source = PAW3222_SOURCE_ALL;

    uint32_t id1 = paw3222_relay_broadcast_request(&req);
    if (id1 == 0) {
        LOG_ERR("Split relay central test: broadcast returned request_id 0");
        return -EINVAL;
    }

    uint32_t id2 = paw3222_relay_broadcast_request(&req);
    if (id2 == 0 || id2 == id1) {
        LOG_ERR("Split relay central test: broadcast request_ids did not increment (%u, %u)", id1,
                id2);
        return -EINVAL;
    }

    printk("PASS: paw3222_split_relay_central_broadcast\n");
    return 0;
}

SYS_INIT(paw3222_split_relay_central_test_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif // CONFIG_ZMK_PAW3222_SPLIT_RPC_RELAY_TEST && CONFIG_ZMK_SPLIT_ROLE_CENTRAL
