#pragma once

/**
 * @file paw3222_relay.h
 *
 * @brief Split relay bridge entry points for the `xinta.paw3222` Studio
 * RPC subsystem, when CONFIG_ZMK_PAW3222_SPLIT_RPC_RELAY is enabled -- see
 * DESIGN.md Phase F and src/split/paw3222_relay.c.
 *
 * Three entry points:
 *  - paw3222_relay_dispatch_request() (central only): relays a request to a
 *    split peripheral's own PAW3222 devices. Relaying is inherently
 *    asynchronous (the split link round-trip does not fit the Studio RPC
 *    call/response model), so this always returns immediately with a
 *    DeferredResponse; the real Response for the assigned request_id
 *    arrives later as a PeripheralResponse Studio notification.
 *  - paw3222_relay_broadcast_request() (central only): fire-and-forget
 *    variant used for GetInfo's `source = PAW3222_SOURCE_ALL` sentinel --
 *    every connected peripheral answers independently as its own
 *    PeripheralResponse notification, all sharing the returned request_id.
 */

#include <xinta/paw3222/paw3222.pb.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Relay `req` to the split peripheral(s) and fill `resp` with a
 * DeferredResponse.
 *
 * @param source The request's `source` field (nonzero; a zero source is
 *   handled locally by the caller and never reaches this function).
 * @param req The decoded request to relay (one of get_info/
 *   read_diagnostics/read_register/write_register).
 * @param resp Always filled with a DeferredResponse (or an ErrorResponse if
 *   encoding/relaying failed outright).
 */
void paw3222_relay_dispatch_request(uint32_t source, const xinta_paw3222_Request *req,
                                    xinta_paw3222_Response *resp);

/** @brief Broadcast `req` to every connected peripheral without waiting for
 * (or producing) a DeferredResponse -- used for GetInfo's
 * `source = PAW3222_SOURCE_ALL` sentinel, where the caller already has an
 * immediate local answer to return synchronously and just needs any
 * peripheral answers correlated separately.
 *
 * @param req The request to broadcast (its `source` field is ignored by
 *   the peripheral executor regardless of value).
 * @return The request_id every responding peripheral's PeripheralResponse
 *   notification will carry (nonzero), or 0 if encoding failed (logged).
 */
uint32_t paw3222_relay_broadcast_request(const xinta_paw3222_Request *req);

#ifdef __cplusplus
}
#endif
