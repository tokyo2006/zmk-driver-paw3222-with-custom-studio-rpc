#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <cormoran/paw3222/paw3222.pb.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAW3222_SOURCE_ALL UINT32_MAX

bool paw3222_request_exec_handle(const cormoran_paw3222_Request *req,
                                 cormoran_paw3222_Response *resp);

uint32_t paw3222_request_get_source(const cormoran_paw3222_Request *req);

#ifdef __cplusplus
}
#endif
