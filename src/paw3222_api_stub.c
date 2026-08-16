/*
 * Fallback implementation of the PAW3222 public API used when the driver
 * core (CONFIG_PAW3222) is not compiled in, e.g. native_sim which has no
 * devicetree node for the sensor (DT_HAS_CORMORAN_PAW3222_ENABLED=n).
 */

#include <cormoran/paw3222/paw3222_api.h>
#include <zephyr/sys/util.h>
#include <errno.h>

size_t paw3222_device_count(void) { return 0; }

const struct device *paw3222_get_device(size_t index) {
    ARG_UNUSED(index);
    return NULL;
}

bool paw3222_is_ready(const struct device *dev) {
    ARG_UNUSED(dev);
    return false;
}

int paw3222_get_init_error(const struct device *dev) {
    ARG_UNUSED(dev);
    return -ENODEV;
}

int paw3222_get_device_id(const struct device *dev, char *buf, size_t buf_len) {
    ARG_UNUSED(dev);
    ARG_UNUSED(buf);
    ARG_UNUSED(buf_len);
    return -ENODEV;
}

int paw3222_read_register(const struct device *dev, uint8_t addr, uint8_t *value) {
    ARG_UNUSED(dev);
    ARG_UNUSED(addr);
    ARG_UNUSED(value);
    return -ENODEV;
}

int paw3222_write_register(const struct device *dev, uint8_t addr, uint8_t value) {
    ARG_UNUSED(dev);
    ARG_UNUSED(addr);
    ARG_UNUSED(value);
    return -ENODEV;
}

int paw3222_read_diagnostics(const struct device *dev, struct paw3222_diagnostics *out) {
    ARG_UNUSED(dev);
    ARG_UNUSED(out);
    return -ENODEV;
}

int paw3222_get_runtime_config(const struct device *dev, struct paw3222_runtime_config *out) {
    ARG_UNUSED(dev);
    ARG_UNUSED(out);
    return -ENODEV;
}

int paw3222_set_cpi_runtime(const struct device *dev, uint32_t cpi) {
    ARG_UNUSED(dev);
    ARG_UNUSED(cpi);
    return -ENODEV;
}

int paw3222_set_force_awake(const struct device *dev, bool enabled) {
    ARG_UNUSED(dev);
    ARG_UNUSED(enabled);
    return -ENODEV;
}
