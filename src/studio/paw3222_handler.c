#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>
#include <xinta/paw3222/paw3222.pb.h>
#include <xinta/paw3222/paw3222_request_exec.h>
#if IS_ENABLED(CONFIG_ZMK_PAW3222_SPLIT_RPC_RELAY)
#include <xinta/paw3222/paw3222_relay.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta paw3222_feature_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(
        "http://xinta.github.io/zmk-driver-paw3222-with-custom-studio-rpc/"),
    /* WriteRegister is a raw, unvalidated sensor register write, so the whole
     * subsystem (including GetInfo/ReadDiagnostics) is secured behind ZMK
     * Studio's unlock (physical &studio_unlock keypress). */
    .security = ZMK_STUDIO_RPC_HANDLER_SECURED,
};

static bool paw3222_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                       pb_callback_t *encode_response);

ZMK_RPC_CUSTOM_SUBSYSTEM(xinta__paw3222, &paw3222_feature_meta, paw3222_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(xinta__paw3222, xinta_paw3222_Response);

static void set_error(xinta_paw3222_Response *resp, const char *fmt, ...) {
    xinta_paw3222_ErrorResponse err = xinta_paw3222_ErrorResponse_init_zero;

    va_list args;
    va_start(args, fmt);
    vsnprintf(err.message, sizeof(err.message), fmt, args);
    va_end(args);

    resp->which_response_type = xinta_paw3222_Response_error_tag;
    resp->response_type.error = err;
}

static bool paw3222_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                       pb_callback_t *encode_response) {
    xinta_paw3222_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(xinta__paw3222, encode_response);

    xinta_paw3222_Request req = xinta_paw3222_Request_init_zero;

    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, xinta_paw3222_Request_fields, &req)) {
        LOG_WRN("Failed to decode paw3222 request: %s", PB_GET_ERROR(&req_stream));
        set_error(resp, "Failed to decode request");
        return true;
    }

    uint32_t source = paw3222_request_get_source(&req);
    bool is_broadcast_get_info = req.which_request_type == xinta_paw3222_Request_get_info_tag &&
                                 source == PAW3222_SOURCE_ALL;

    if (source == 0 || is_broadcast_get_info) {
        if (!paw3222_request_exec_handle(&req, resp)) {
            LOG_WRN("Unsupported paw3222 request type: %d", req.which_request_type);
            set_error(resp, "Unsupported request type");
        }
#if IS_ENABLED(CONFIG_ZMK_PAW3222_SPLIT_RPC_RELAY)
        if (is_broadcast_get_info &&
            resp->which_response_type == xinta_paw3222_Response_get_info_tag) {
            resp->response_type.get_info.relay_request_id = paw3222_relay_broadcast_request(&req);
        }
#endif
    } else {
#if IS_ENABLED(CONFIG_ZMK_PAW3222_SPLIT_RPC_RELAY)
        paw3222_relay_dispatch_request(source, &req, resp);
#else
        set_error(resp, "source %u requested but CONFIG_ZMK_PAW3222_SPLIT_RPC_RELAY is not enabled",
                  source);
#endif
    }

    return true;
}
