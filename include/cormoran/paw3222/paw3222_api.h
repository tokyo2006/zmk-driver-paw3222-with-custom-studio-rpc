#pragma once

/**
 * @file paw3222_api.h
 *
 * @brief Public API exposed by the PAW3222 driver for use by the custom
 * Studio RPC handler and the optional settings integration.
 *
 * This header intentionally does not depend on any nanopb/proto generated
 * types so it stays usable outside the studio RPC subsystem.
 *
 * The PAW3222 is a low-power optical *motion* sensor (reports delta X/Y);
 * unlike the PMW3610 it has no pixel array, so there is no frame-capture or
 * surface-quality (SQUAL/shutter) API here.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <cormoran/paw3222/paw3222_settings_id.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Number of PAW3222 device instances found in the devicetree. */
size_t paw3222_device_count(void);

/** @brief Get the PAW3222 device instance at the given index (0-based). */
const struct device *paw3222_get_device(size_t index);

/** @brief Whether the given PAW3222 device has finished async init. */
bool paw3222_is_ready(const struct device *dev);

/** @brief Last error code observed during async init (0 = no error). */
int paw3222_get_init_error(const struct device *dev);

/** @brief Get this device's stable per-device settings id (NUL-terminated). */
int paw3222_get_device_id(const struct device *dev, char *buf, size_t buf_len);

/** @brief Read a single register over SPI. */
int paw3222_read_register(const struct device *dev, uint8_t addr, uint8_t *value);

/** @brief Write a single register over SPI (debug/tuning facility, no
 * validation -- callers may brick sensor behavior until next power-up). */
int paw3222_write_register(const struct device *dev, uint8_t addr, uint8_t value);

/** @brief Sensor diagnostics snapshot (product id + motion + cpi). */
struct paw3222_diagnostics {
    uint8_t product_id1; /**< PRODUCT_ID1 (0x00), expect 0x30. */
    uint8_t product_id2; /**< PRODUCT_ID2 (0x01). */
    uint8_t motion;      /**< MOTION (0x02) raw status. */
    uint32_t cpi;        /**< Current effective CPI. */
};

/** @brief Read product id / motion / cpi diagnostics registers. */
int paw3222_read_diagnostics(const struct device *dev, struct paw3222_diagnostics *out);

/** @brief Snapshot of the current runtime configuration. */
struct paw3222_runtime_config {
    uint32_t cpi;
    bool force_awake;
    bool disable_burst_read;
};

/** @brief Read the current runtime configuration snapshot. */
int paw3222_get_runtime_config(const struct device *dev, struct paw3222_runtime_config *out);

/**
 * Runtime setters. Each validates, stores into the per-device runtime config,
 * and (if the device is ready) pushes to the sensor immediately. Return 0 on
 * success, -EINVAL for out-of-range values, -ENODEV for an unknown device.
 */
int paw3222_set_cpi_runtime(const struct device *dev, uint32_t cpi);
int paw3222_set_force_awake(const struct device *dev, bool enabled);

#ifdef __cplusplus
}
#endif
