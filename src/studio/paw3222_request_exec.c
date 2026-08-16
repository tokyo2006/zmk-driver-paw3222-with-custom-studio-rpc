#include <pb_decode.h>
#include <pb_encode.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <cormoran/paw3222/paw3222.pb.h>
#include <cormoran/paw3222/paw3222_api.h>
#include <cormoran/paw3222/paw3222_request_exec.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

uint32_t paw3222_request_get_source(const cormoran_paw3222_Request *req) {
    switch (req->which_request_type) {
    case cormoran_paw3222_Request_get_info_tag:
        return req->request_type.get_info.source;
    case cormoran_paw3222_Request_read_diagnostics_tag:
        return req->request_type.read_diagnostics.source;
    case cormoran_paw3222_Request_read_register_tag:
        return req->request_type.read_register.source;
    case cormoran_paw3222_Request_write_register_tag:
        return req->request_type.write_register.source;
    default:
        return 0;
    }
}

static void set_error(cormoran_paw3222_Response *resp, const char *msg) {
    cormoran_paw3222_ErrorResponse err = cormoran_paw3222_ErrorResponse_init_zero;
    strncpy(err.message, msg, sizeof(err.message) - 1);
    resp->which_response_type = cormoran_paw3222_Response_error_tag;
    resp->response_type.error = err;
}

static void handle_get_info(cormoran_paw3222_Response *resp) {
    cormoran_paw3222_GetInfoResponse *out = &resp->response_type.get_info;
    size_t count = paw3222_device_count();

    for (size_t i = 0; i < count && i < ARRAY_SIZE(out->devices); i++) {
        const struct device *dev = paw3222_get_device(i);
        cormoran_paw3222_DeviceInfo *info = &out->devices[out->devices_count++];

        struct paw3222_runtime_config rt;
        struct paw3222_diagnostics diag;
        char id[PAW3222_SETTINGS_ID_BUF_SIZE];

        paw3222_get_runtime_config(dev, &rt);
        paw3222_get_device_id(dev, id, sizeof(id));

        info->ready = paw3222_is_ready(dev);
        info->init_error = paw3222_get_init_error(dev);
        info->device_index = i;
        info->runtime_config.cpi = rt.cpi;
        info->runtime_config.force_awake = rt.force_awake;
        info->runtime_config.disable_burst_read = rt.disable_burst_read;
        strncpy(info->settings_id, id, sizeof(info->settings_id) - 1);

        if (info->ready && paw3222_read_diagnostics(dev, &diag) == 0) {
            info->product_id = diag.product_id1;
            info->revision_id = diag.product_id2;
        }
    }

    resp->which_response_type = cormoran_paw3222_Response_get_info_tag;
}

static void handle_read_diagnostics(const cormoran_paw3222_ReadDiagnosticsRequest *req,
                                    cormoran_paw3222_Response *resp) {
    const struct device *dev = paw3222_get_device(req->device_index);
    if (!dev) {
        set_error(resp, "invalid device_index");
        return;
    }

    struct paw3222_diagnostics diag;
    if (paw3222_read_diagnostics(dev, &diag) != 0) {
        set_error(resp, "device not ready");
        return;
    }

    cormoran_paw3222_ReadDiagnosticsResponse *out = &resp->response_type.read_diagnostics;
    out->product_id1 = diag.product_id1;
    out->product_id2 = diag.product_id2;
    out->motion = diag.motion;
    out->cpi = diag.cpi;
    resp->which_response_type = cormoran_paw3222_Response_read_diagnostics_tag;
}

static void handle_read_register(const cormoran_paw3222_ReadRegisterRequest *req,
                                 cormoran_paw3222_Response *resp) {
    const struct device *dev = paw3222_get_device(req->device_index);
    if (!dev) {
        set_error(resp, "invalid device_index");
        return;
    }

    uint8_t value;
    if (paw3222_read_register(dev, (uint8_t)req->address, &value) != 0) {
        set_error(resp, "register read failed");
        return;
    }

    resp->response_type.read_register.value = value;
    resp->which_response_type = cormoran_paw3222_Response_read_register_tag;
}

static void handle_write_register(const cormoran_paw3222_WriteRegisterRequest *req,
                                  cormoran_paw3222_Response *resp) {
    const struct device *dev = paw3222_get_device(req->device_index);
    if (!dev) {
        set_error(resp, "invalid device_index");
        return;
    }

    if (paw3222_write_register(dev, (uint8_t)req->address, (uint8_t)req->value) != 0) {
        set_error(resp, "register write failed");
        return;
    }

    resp->which_response_type = cormoran_paw3222_Response_write_register_tag;
}

bool paw3222_request_exec_handle(const cormoran_paw3222_Request *req, cormoran_paw3222_Response *resp) {
    switch (req->which_request_type) {
    case cormoran_paw3222_Request_get_info_tag:
        handle_get_info(resp);
        return true;
    case cormoran_paw3222_Request_read_diagnostics_tag:
        handle_read_diagnostics(&req->request_type.read_diagnostics, resp);
        return true;
    case cormoran_paw3222_Request_read_register_tag:
        handle_read_register(&req->request_type.read_register, resp);
        return true;
    case cormoran_paw3222_Request_write_register_tag:
        handle_write_register(&req->request_type.write_register, resp);
        return true;
    default:
        return false;
    }
}
