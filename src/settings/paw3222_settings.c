/*
 * Copyright (c) 2026 xinta
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <xinta/zmk/custom_settings.h>
#include <xinta/paw3222/paw3222_api.h>
#include <xinta/paw3222/paw3222_settings_apply.h>
#include <xinta/paw3222/paw3222_settings_id.h>

#include <zmk/event_manager.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT xinta_paw3222

#define PAW3222_SETTINGS_SUBSYSTEM_ID "xinta__paw3222"

#define PAW3222_SETTINGS_KEY_BUF_SIZE 40

#define PAW3222_SETTING_INT32(_name, _key, _default, _constraint)                                  \
    ZMK_CUSTOM_SETTING_DEFINE(                                                                     \
        _name, PAW3222_SETTINGS_SUBSYSTEM_ID, _key, ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,           \
        ZMK_CUSTOM_SETTING_VALUE_INT32(_default), ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,   \
        ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_PERMISSION_SECURE, _constraint)

#define PAW3222_SETTING_BOOL(_name, _key, _default)                                                \
    ZMK_CUSTOM_SETTING_DEFINE(                                                                     \
        _name, PAW3222_SETTINGS_SUBSYSTEM_ID, _key, ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL,            \
        ZMK_CUSTOM_SETTING_VALUE_BOOL(_default), ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,    \
        ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_PERMISSION_SECURE,              \
        ZMK_CUSTOM_SETTING_NO_CONSTRAINT)

#define PAW3222_INST_ID_VAR(n) paw3222_settings_id_##n
#define PAW3222_INST_KEY_VAR(n, field) paw3222_settings_key_##n##_##field

#define PAW3222_DECLARE_INST_STORAGE(n)                                                            \
    static char PAW3222_INST_ID_VAR(n)[PAW3222_SETTINGS_ID_BUF_SIZE];                              \
    static char PAW3222_INST_KEY_VAR(n, cpi)[PAW3222_SETTINGS_KEY_BUF_SIZE];                       \
    static char PAW3222_INST_KEY_VAR(n, force_awake)[PAW3222_SETTINGS_KEY_BUF_SIZE];

DT_INST_FOREACH_STATUS_OKAY(PAW3222_DECLARE_INST_STORAGE)

#define PAW3222_DEFINE_INST_SETTINGS(n)                                                            \
    PAW3222_SETTING_INT32(paw3222_setting_##n##_cpi, PAW3222_INST_KEY_VAR(n, cpi),                 \
                          DT_INST_PROP_OR(n, cpi, 1600),                                           \
                          ZMK_CUSTOM_SETTING_RANGE_INT32(608, 4826));                              \
    PAW3222_SETTING_BOOL(paw3222_setting_##n##_force_awake, PAW3222_INST_KEY_VAR(n, force_awake),  \
                         DT_INST_PROP(n, force_awake));

DT_INST_FOREACH_STATUS_OKAY(PAW3222_DEFINE_INST_SETTINGS)

#define PAW3222_INIT_INST_KEYS_FN(n)                                                               \
    static int paw3222_settings_keys_init_##n(void) {                                              \
        paw3222_settings_id_resolve(DT_INST_PROP_OR(n, settings_id, NULL),                         \
                                    DT_NODE_PATH(DT_DRV_INST(n)), PAW3222_INST_ID_VAR(n));         \
        snprintf(PAW3222_INST_KEY_VAR(n, cpi), PAW3222_SETTINGS_KEY_BUF_SIZE, "cpi@%s",            \
                 PAW3222_INST_ID_VAR(n));                                                          \
        snprintf(PAW3222_INST_KEY_VAR(n, force_awake), PAW3222_SETTINGS_KEY_BUF_SIZE,              \
                 "force_awake@%s", PAW3222_INST_ID_VAR(n));                                        \
        return 0;                                                                                  \
    }                                                                                              \
    SYS_INIT(paw3222_settings_keys_init_##n, POST_KERNEL, 0);

DT_INST_FOREACH_STATUS_OKAY(PAW3222_INIT_INST_KEYS_FN)

static int32_t read_int32_by_field(const char *id, const char *field, int32_t fallback) {
    char key[PAW3222_SETTINGS_KEY_BUF_SIZE];
    snprintf(key, sizeof(key), "%s@%s", field, id);

    struct zmk_custom_setting_value value;
    if (zmk_custom_setting_read_by_key(PAW3222_SETTINGS_SUBSYSTEM_ID, key, &value) != 0 ||
        value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32) {
        return fallback;
    }
    return value.int32_value;
}

static bool read_bool_by_field(const char *id, const char *field, bool fallback) {
    char key[PAW3222_SETTINGS_KEY_BUF_SIZE];
    snprintf(key, sizeof(key), "%s@%s", field, id);

    struct zmk_custom_setting_value value;
    if (zmk_custom_setting_read_by_key(PAW3222_SETTINGS_SUBSYSTEM_ID, key, &value) != 0 ||
        value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL) {
        return fallback;
    }
    return value.bool_value;
}

void paw3222_settings_apply_to_device(const struct device *dev) {
    if (!dev) {
        return;
    }

    char id[PAW3222_SETTINGS_ID_BUF_SIZE];
    if (paw3222_get_device_id(dev, id, sizeof(id)) != 0) {
        return;
    }

    paw3222_set_cpi_runtime(dev, (uint32_t)read_int32_by_field(id, "cpi", 1600));
    paw3222_set_force_awake(dev, read_bool_by_field(id, "force_awake", false));
}

static void paw3222_settings_apply_to_all_devices(void) {
    size_t count = paw3222_device_count();
    for (size_t i = 0; i < count; i++) {
        paw3222_settings_apply_to_device(paw3222_get_device(i));
    }
}

static int paw3222_settings_event_listener(const zmk_event_t *eh) {
    if (as_zmk_custom_settings_initialized(eh) != NULL) {
        paw3222_settings_apply_to_all_devices();
        return 0;
    }

    const struct zmk_custom_setting_changed *ev = as_zmk_custom_setting_changed(eh);
    if (!ev || !ev->setting) {
        return 0;
    }

    if (strncmp(ev->setting->custom_subsystem_id, PAW3222_SETTINGS_SUBSYSTEM_ID,
                CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN) != 0) {
        return 0;
    }

    paw3222_settings_apply_to_all_devices();
    return 0;
}

ZMK_LISTENER(paw3222_settings, paw3222_settings_event_listener);
ZMK_SUBSCRIPTION(paw3222_settings, zmk_custom_setting_changed);
ZMK_SUBSCRIPTION(paw3222_settings, zmk_custom_settings_initialized);
