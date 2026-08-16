#pragma once

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

void paw3222_settings_apply_to_device(const struct device *dev);

#ifdef __cplusplus
}
#endif
