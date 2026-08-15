#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <xinta/paw3222/paw3222.pb.h>

#ifdef __cplusplus
extern "C" {
#endif

bool paw3222_request_exec_handle(const xinta_paw3222_Request *req,
                                 xinta_paw3222_Response *resp);

uint32_t paw3222_request_get_source(const xinta_paw3222_Request *req);

#ifdef __cplusplus
}
#endif
