/*
 * Copyright 2024 Google LLC
 * Modifications Copyright 2025 sekigon-gonnoc
 *
 * Original source code:
 * https://github.com/zephyrproject-rtos/zephyr/blob/19c6240b6865bcb28e1d786d4dcadfb3a02067a0/drivers/input/input_paw32xx.c
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_SOC_SERIES_NRF52X)
#include <hal/nrf_gpio.h>
#include <hal/nrf_spim.h>
#endif

#include "../include/paw3222.h"
#include <cormoran/paw3222/paw3222_api.h>
#include <cormoran/paw3222/paw3222_settings_apply.h>

LOG_MODULE_REGISTER(paw32xx, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT cormoran_paw3222

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define PAW32XX_PRODUCT_ID1 0x00
#define PAW32XX_PRODUCT_ID2 0x01
#define PAW32XX_MOTION 0x02
#define PAW32XX_DELTA_X 0x03
#define PAW32XX_DELTA_Y 0x04
#define PAW32XX_OPERATION_MODE 0x05
#define PAW32XX_CONFIGURATION 0x06
#define PAW32XX_WRITE_PROTECT 0x09
#define PAW32XX_SLEEP1 0x0a
#define PAW32XX_SLEEP2 0x0b
#define PAW32XX_SLEEP3 0x0c
#define PAW32XX_CPI_X 0x0d
#define PAW32XX_CPI_Y 0x0e
#define PAW32XX_DELTA_XY_HI 0x12
#define PAW32XX_MOUSE_OPTION 0x19

#define PRODUCT_ID_PAW32XX 0x30
#define SPI_WRITE BIT(7)

#define MOTION_STATUS_MOTION BIT(7)
#define OPERATION_MODE_SLP_ENH BIT(4)
#define OPERATION_MODE_SLP2_ENH BIT(3)
#define OPERATION_MODE_SLP_MASK (OPERATION_MODE_SLP_ENH | OPERATION_MODE_SLP2_ENH)
#define CONFIGURATION_PD_ENH BIT(3)
#define CONFIGURATION_RESET BIT(7)
#define WRITE_PROTECT_ENABLE 0x00
#define WRITE_PROTECT_DISABLE 0x5a
#define MOUSE_OPTION_MOVX_INV_BIT 3
#define MOUSE_OPTION_MOVY_INV_BIT 4

#define PAW32XX_DATA_SIZE_BITS 8

#define RESET_DELAY_MS 2

#define RES_STEP 38
#define RES_MIN (16 * RES_STEP)
#define RES_MAX (127 * RES_STEP)

struct paw32xx_config {
    struct spi_dt_spec spi;
    struct gpio_dt_spec irq_gpio;
    struct gpio_dt_spec power_gpio;
    int16_t cpi;
    bool force_awake;
    bool disable_burst_read;
    uint16_t evt_type;
    uint16_t x_input_code;
    uint16_t y_input_code;
    const char *settings_id;
    const char *dt_node_path;
};

struct paw32xx_data {
    const struct device *dev;
    struct k_work motion_work;
    struct gpio_callback motion_cb;
    struct k_timer motion_timer; // Add timer for delayed motion checking
    struct k_work_delayable init_work; // 延迟初始化任务
    int async_init_retry_count; // 初始化重试计数
    bool ready; // 初始化完成标志
    int err; // 初始化错误码
    struct k_mutex lock;
    char device_id[PAW3222_SETTINGS_ID_BUF_SIZE];
    struct paw3222_runtime_config runtime;

#if defined(CONFIG_SOC_SERIES_NRF52X)
    NRF_SPIM_Type *spim;
    uint32_t spim_mosi_psel;
    uint32_t spim_miso_psel;
    uint32_t spim_sclk_psel;
    bool spim_mosi_psel_saved;
    bool spim_miso_psel_saved;
#endif
};
// nRF52 SPI/SDIO 低功耗辅助函数
#if defined(CONFIG_SOC_SERIES_NRF52X)
#define PAW32XX_NRF_PSEL_CONNECT_BIT BIT(31)
#define PAW32XX_NRF_PSEL_PIN_MASK GENMASK(4, 0)
#define PAW32XX_NRF_PSEL_PORT_BIT BIT(5)

static NRF_SPIM_Type *paw32xx_nrf52_spim_from_bus(const struct device *dev) {
    const struct paw32xx_config *cfg = dev->config;
#if DT_NODE_HAS_STATUS(DT_NODELABEL(spi0), okay) && defined(NRF_SPIM0)
    if (cfg->spi.bus == DEVICE_DT_GET(DT_NODELABEL(spi0))) {
        return NRF_SPIM0;
    }
#endif
#if DT_NODE_HAS_STATUS(DT_NODELABEL(spi1), okay) && defined(NRF_SPIM1)
    if (cfg->spi.bus == DEVICE_DT_GET(DT_NODELABEL(spi1))) {
        return NRF_SPIM1;
    }
#endif
    return NULL;
}

static uint32_t paw32xx_nrf52_psel_to_pin(uint32_t psel) {
    uint32_t pin = psel & PAW32XX_NRF_PSEL_PIN_MASK;
    if ((psel & PAW32XX_NRF_PSEL_PORT_BIT) != 0U) {
        pin += 32U;
    }
    return pin;
}

static void paw32xx_nrf52_spim_deactivate(struct paw32xx_data *data) {
    if (data->spim != NULL) {
        nrf_spim_disable(data->spim);
    }
}
static void paw32xx_nrf52_spim_activate(struct paw32xx_data *data) {
    if (data->spim != NULL) {
        nrf_spim_enable(data->spim);
    }
}
static void paw32xx_sdio_init(struct paw32xx_data *data) {
    if (data->spim == NULL) {
        return;
    }
    data->spim_mosi_psel = data->spim->PSEL.MOSI;
    data->spim_miso_psel = data->spim->PSEL.MISO;
    data->spim_sclk_psel = data->spim->PSEL.SCK;
    data->spim_mosi_psel_saved = ((data->spim_mosi_psel & PAW32XX_NRF_PSEL_CONNECT_BIT) == 0U);
    data->spim_miso_psel_saved = ((data->spim_miso_psel & PAW32XX_NRF_PSEL_CONNECT_BIT) == 0U);
    if (data->spim_mosi_psel_saved) {
        nrf_gpio_cfg_default(paw32xx_nrf52_psel_to_pin(data->spim_mosi_psel));
    }
    if (data->spim_miso_psel_saved) {
        nrf_gpio_cfg_default(paw32xx_nrf52_psel_to_pin(data->spim_miso_psel));
        nrf_gpio_cfg_input(paw32xx_nrf52_psel_to_pin(data->spim_miso_psel), NRF_GPIO_PIN_PULLUP);
    }
}
static void paw32xx_sdio_disconnect(struct paw32xx_data *data) {
    if (data->spim == NULL) {
        return;
    }
    if (!data->spim_mosi_psel_saved) {
        data->spim_mosi_psel = data->spim->PSEL.MOSI;
        data->spim_mosi_psel_saved = ((data->spim_mosi_psel & PAW32XX_NRF_PSEL_CONNECT_BIT) == 0U);
    }
    if (!data->spim_miso_psel_saved) {
        data->spim_miso_psel = data->spim->PSEL.MISO;
        data->spim_miso_psel_saved = ((data->spim_miso_psel & PAW32XX_NRF_PSEL_CONNECT_BIT) == 0U);
    }
    if (data->spim_mosi_psel_saved) {
        nrf_gpio_cfg_default(paw32xx_nrf52_psel_to_pin(data->spim_mosi_psel));
        data->spim->PSEL.MOSI = data->spim_mosi_psel | PAW32XX_NRF_PSEL_CONNECT_BIT;
    }
    if (data->spim_miso_psel_saved) {
        nrf_gpio_cfg_input(paw32xx_nrf52_psel_to_pin(data->spim_miso_psel), NRF_GPIO_PIN_PULLUP);
        data->spim->PSEL.MISO = data->spim_miso_psel | PAW32XX_NRF_PSEL_CONNECT_BIT;
    }
}
static void paw32xx_sdio_connect(struct paw32xx_data *data) {
    if (data->spim == NULL) {
        return;
    }
    if (data->spim_mosi_psel_saved) {
        nrf_gpio_cfg_output(paw32xx_nrf52_psel_to_pin(data->spim_mosi_psel));
        data->spim->PSEL.MOSI = data->spim_mosi_psel & ~PAW32XX_NRF_PSEL_CONNECT_BIT;
    }
    if (data->spim_miso_psel_saved) {
        nrf_gpio_cfg_input(paw32xx_nrf52_psel_to_pin(data->spim_miso_psel), NRF_GPIO_PIN_NOPULL);
        data->spim->PSEL.MISO = data->spim_miso_psel & ~PAW32XX_NRF_PSEL_CONNECT_BIT;
    }
}
static void paw32xx_spi_transaction_begin(const struct device *dev) {
    struct paw32xx_data *data = dev->data;
    paw32xx_sdio_connect(data);
    paw32xx_nrf52_spim_activate(data);
}
static void paw32xx_spi_transaction_end(const struct device *dev) {
    struct paw32xx_data *data = dev->data;
    paw32xx_sdio_disconnect(data);
    paw32xx_nrf52_spim_deactivate(data);
}
#else
static inline void paw32xx_spi_transaction_begin(const struct device *dev) { ARG_UNUSED(dev); }
static inline void paw32xx_spi_transaction_end(const struct device *dev) { ARG_UNUSED(dev); }
#endif

static int paw32xx_force_cs(const struct device *dev, bool force_low) {
    const struct paw32xx_config *cfg = dev->config;
    const struct gpio_dt_spec *cs = NULL;
    int ret;

    if (cfg->spi.config.cs.gpio.port != NULL) {
        cs = &cfg->spi.config.cs.gpio;
    }

    if (cs == NULL || cs->port == NULL || !device_is_ready(cs->port)) {
        LOG_ERR("CS GPIO not defined or not ready");
        return ENODEV;
    }

    ret = gpio_pin_set_dt(cs, force_low ? 1 : 0);
    if (ret < 0) {
        LOG_ERR("Failed to drive CS pin: %d", ret);
        return ret;
    }

    return 0;
}

// Define a custom sign_extend function to avoid conflict with Zephyr's implementation
static inline int32_t _sign_extend(uint32_t value, uint8_t index) {
    __ASSERT_NO_MSG(index <= 31);

    uint8_t shift = 31 - index;

    return (int32_t)(value << shift) >> shift;
}

static int paw32xx_read_reg(const struct device *dev, uint8_t addr, uint8_t *value) {
    const struct paw32xx_config *cfg = dev->config;
    int ret;

    const struct spi_buf tx_buf = {
        .buf = &addr,
        .len = sizeof(addr),
    };
    const struct spi_buf_set tx = {
        .buffers = &tx_buf,
        .count = 1,
    };

    struct spi_buf rx_buf[] = {
        {
            .buf = NULL,
            .len = sizeof(addr),
        },
        {
            .buf = value,
            .len = 1,
        },
    };
    const struct spi_buf_set rx = {
        .buffers = rx_buf,
        .count = ARRAY_SIZE(rx_buf),
    };
    paw32xx_spi_transaction_begin(dev);
    ret = spi_transceive_dt(&cfg->spi, &tx, &rx);
    paw32xx_spi_transaction_end(dev);
    return ret;
}

static int paw32xx_write_reg(const struct device *dev, uint8_t addr, uint8_t value) {
    const struct paw32xx_config *cfg = dev->config;
    int ret;
    uint8_t write_buf[] = {addr | SPI_WRITE, value};
    const struct spi_buf tx_buf = {
        .buf = write_buf,
        .len = sizeof(write_buf),
    };
    const struct spi_buf_set tx = {
        .buffers = &tx_buf,
        .count = 1,
    };
    paw32xx_spi_transaction_begin(dev);
    ret = spi_write_dt(&cfg->spi, &tx);
    paw32xx_spi_transaction_end(dev);
    return ret;
}

static int paw32xx_update_reg(const struct device *dev, uint8_t addr, uint8_t mask, uint8_t value) {
    uint8_t val;
    int ret;

    ret = paw32xx_read_reg(dev, addr, &val);
    if (ret < 0) {
        return ret;
    }

    val = (val & ~mask) | (value & mask);

    ret = paw32xx_write_reg(dev, addr, val);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

// If disable_burst_read为true，单字节循环读取X/Y，否则burst读取
static int paw32xx_read_xy(const struct device *dev, int16_t *x, int16_t *y) {
    const struct paw32xx_config *cfg = dev->config;
    int ret;
    uint8_t x_val = 0, y_val = 0;

    if (!cfg->disable_burst_read) {
        // burst read实现（原有方式）
        uint8_t tx_data[] = {
            PAW32XX_DELTA_X,
            0xff,
            PAW32XX_DELTA_Y,
            0xff,
        };
        uint8_t rx_data[sizeof(tx_data)];

        const struct spi_buf tx_buf = {
            .buf = tx_data,
            .len = sizeof(tx_data),
        };
        const struct spi_buf_set tx = {
            .buffers = &tx_buf,
            .count = 1,
        };

        struct spi_buf rx_buf = {
            .buf = rx_data,
            .len = sizeof(rx_data),
        };
        const struct spi_buf_set rx = {
            .buffers = &rx_buf,
            .count = 1,
        };
        paw32xx_spi_transaction_begin(dev);
        ret = spi_transceive_dt(&cfg->spi, &tx, &rx);
        paw32xx_spi_transaction_end(dev);
        if (ret < 0) {
            return ret;
        }
        x_val = rx_data[1];
        y_val = rx_data[3];
    } else {
        // 单字节循环读取X/Y
        ret = paw32xx_read_reg(dev, PAW32XX_DELTA_X, &x_val);
        if (ret < 0) {
            return ret;
        }
        ret = paw32xx_read_reg(dev, PAW32XX_DELTA_Y, &y_val);
        if (ret < 0) {
            return ret;
        }
    }

    *x = _sign_extend(x_val, PAW32XX_DATA_SIZE_BITS - 1);
    *y = _sign_extend(y_val, PAW32XX_DATA_SIZE_BITS - 1);

    return 0;
}

static int paw32xx_interrupt_configure(const struct device *dev, gpio_flags_t flags) {
    const struct paw32xx_config *cfg = dev->config;

    if (!gpio_is_ready_dt(&cfg->irq_gpio)) {
        return -ENODEV;
    }

    return gpio_pin_interrupt_configure_dt(&cfg->irq_gpio, flags);
}

static int paw32xx_interrupt_enable(const struct device *dev) {
    return paw32xx_interrupt_configure(dev, GPIO_INT_LEVEL_ACTIVE);
}

static int paw32xx_interrupt_disable(const struct device *dev) {
    return paw32xx_interrupt_configure(dev, GPIO_INT_DISABLE);
}

static void paw32xx_motion_timer_handler(struct k_timer *timer) {
    struct paw32xx_data *data = CONTAINER_OF(timer, struct paw32xx_data, motion_timer);
    k_work_submit(&data->motion_work);
}

static void paw32xx_motion_work_handler(struct k_work *work) {
    struct paw32xx_data *data = CONTAINER_OF(work, struct paw32xx_data, motion_work);
    const struct device *dev = data->dev;
    const struct paw32xx_config *cfg = dev->config;
    uint8_t val;
    int16_t x, y;
    int ret;

    ret = paw32xx_read_reg(dev, PAW32XX_MOTION, &val);
    if (ret < 0) {
        return;
    }

    if ((val & MOTION_STATUS_MOTION) == 0x00) {
        // No motion detected, re-enable interrupts and wait for next interrupt
        paw32xx_interrupt_enable(dev);

        if (gpio_pin_get_dt(&cfg->irq_gpio) == 0) {
            return;
        }
    }

    ret = paw32xx_read_xy(dev, &x, &y);
    if (ret < 0) {
        return;
    }

    LOG_DBG("x=%4d y=%4d", x, y);

    input_report(data->dev, cfg->evt_type, cfg->x_input_code, x, false, K_FOREVER);
    input_report(data->dev, cfg->evt_type, cfg->y_input_code, y, true, K_FOREVER);

    // Schedule next check after 15ms without using interrupts
    k_timer_start(&data->motion_timer, K_MSEC(15), K_NO_WAIT);
}

static void paw32xx_motion_handler(const struct device *gpio_dev, struct gpio_callback *cb,
                                   uint32_t pins) {
    struct paw32xx_data *data = CONTAINER_OF(cb, struct paw32xx_data, motion_cb);
    const struct device *dev = data->dev;

    ARG_UNUSED(gpio_dev);
    ARG_UNUSED(pins);

    // Disable interrupts while timer is active
    paw32xx_interrupt_disable(dev);

    // Cancel any pending timer
    k_timer_stop(&data->motion_timer);

    // Process motion
    k_work_submit(&data->motion_work);
}

int paw32xx_set_resolution(const struct device *dev, uint16_t res_cpi) {
    uint8_t val;
    int ret;

    if (!IN_RANGE(res_cpi, RES_MIN, RES_MAX)) {
        LOG_ERR("cpi out of range: %d", res_cpi);
        return -EINVAL;
    }

    val = res_cpi / RES_STEP;

    ret = paw32xx_write_reg(dev, PAW32XX_WRITE_PROTECT, WRITE_PROTECT_DISABLE);
    if (ret < 0) {
        return ret;
    }

    ret = paw32xx_write_reg(dev, PAW32XX_CPI_X, val);
    if (ret < 0) {
        return ret;
    }

    ret = paw32xx_write_reg(dev, PAW32XX_CPI_Y, val);
    if (ret < 0) {
        return ret;
    }

    ret = paw32xx_write_reg(dev, PAW32XX_WRITE_PROTECT, WRITE_PROTECT_ENABLE);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

int paw32xx_force_awake(const struct device *dev, bool enable) {
    uint8_t val = enable ? 0 : OPERATION_MODE_SLP_MASK;
    int ret;

    ret = paw32xx_write_reg(dev, PAW32XX_WRITE_PROTECT, WRITE_PROTECT_DISABLE);
    if (ret < 0) {
        return ret;
    }

    ret = paw32xx_update_reg(dev, PAW32XX_OPERATION_MODE, OPERATION_MODE_SLP_MASK, val);
    if (ret < 0) {
        return ret;
    }

    ret = paw32xx_write_reg(dev, PAW32XX_WRITE_PROTECT, WRITE_PROTECT_ENABLE);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

// 异步初始化流程，带重试
#define PAW32XX_ASYNC_INIT_MAX_RETRY 10
static void paw32xx_async_init(struct k_work *work) {
    struct k_work_delayable *work2 = (struct k_work_delayable *)work;
    struct paw32xx_data *data = CONTAINER_OF(work2, struct paw32xx_data, init_work);
    const struct device *dev = data->dev;
    const struct paw32xx_config *cfg = dev->config;
    uint8_t val;
    int ret;

    // 检查设备ID，失败重试
    ret = paw32xx_read_reg(dev, PAW32XX_PRODUCT_ID1, &val);
    if (ret < 0 || val != PRODUCT_ID_PAW32XX) {
        if (data->async_init_retry_count < PAW32XX_ASYNC_INIT_MAX_RETRY) {
            data->async_init_retry_count++;
#if DT_INST_NODE_HAS_PROP(0, power_gpios)
            // reboot
            paw32xx_force_cs(dev, true);
            gpio_pin_set_dt(&cfg->power_gpio, 0);
            k_sleep(K_MSEC(50));
            gpio_pin_set_dt(&cfg->power_gpio, 1);
            paw32xx_force_cs(dev, false);
#endif
            k_work_schedule(&data->init_work, K_MSEC(100));
            return;
        } else {
            data->err = -ENODEV;
            data->ready = false;
            LOG_ERR("paw32xx: failed to init after retries");
            return;
        }
    }

    // 配置寄存器
    ret = paw32xx_update_reg(dev, PAW32XX_CONFIGURATION, CONFIGURATION_RESET, CONFIGURATION_RESET);
    if (ret < 0) {
        data->err = ret;
        data->ready = false;
        return;
    }
    k_sleep(K_MSEC(RESET_DELAY_MS));

    if (cfg->cpi > 0) {
        paw32xx_set_resolution(dev, cfg->cpi);
    }
    paw32xx_force_awake(dev, cfg->force_awake);

    // Dummy reads to clear any residual data
    paw32xx_read_reg(dev, PAW32XX_MOTION, &val);
    paw32xx_read_reg(dev, PAW32XX_DELTA_X, &val);
    paw32xx_read_reg(dev, PAW32XX_DELTA_Y, &val);
    paw32xx_read_reg(dev, PAW32XX_DELTA_XY_HI, &val);

    data->ready = true;
    data->err = 0;
    LOG_INF("paw32xx: initialized");
}

static int paw32xx_init(const struct device *dev) {
    const struct paw32xx_config *cfg = dev->config;
    struct paw32xx_data *data = dev->data;
    int ret;

    if (!spi_is_ready_dt(&cfg->spi)) {
        LOG_ERR("%s is not ready", cfg->spi.bus->name);
        return -ENODEV;
    }

#if defined(CONFIG_SOC_SERIES_NRF52X)
    data->spim = paw32xx_nrf52_spim_from_bus(dev);
    paw32xx_sdio_init(data);
    paw32xx_sdio_disconnect(data);
#endif

    data->dev = dev;
    data->ready = false;
    data->async_init_retry_count = 0;
    data->err = 0;

    k_mutex_init(&data->lock);
    data->runtime.cpi = cfg->cpi > 0 ? (uint32_t)cfg->cpi : 1600;
    data->runtime.force_awake = cfg->force_awake;
    data->runtime.disable_burst_read = cfg->disable_burst_read;
    paw3222_settings_id_resolve(cfg->settings_id, cfg->dt_node_path, data->device_id);

    k_work_init(&data->motion_work, paw32xx_motion_work_handler);
    k_timer_init(&data->motion_timer, paw32xx_motion_timer_handler, NULL);
    k_work_init_delayable(&data->init_work, paw32xx_async_init);

#if DT_INST_NODE_HAS_PROP(0, power_gpios)
    if (gpio_is_ready_dt(&cfg->power_gpio)) {
        ret = paw32xx_force_cs(dev, true);
        if (ret != 0) {
            return ret;
        }
        ret = gpio_pin_configure_dt(&cfg->power_gpio, GPIO_OUTPUT_INACTIVE);
        if (ret != 0) {
            LOG_ERR("Power pin configuration failed: %d", ret);
            return ret;
        }
        k_sleep(K_MSEC(10));
        ret = gpio_pin_set_dt(&cfg->power_gpio, 1);
        if (ret != 0) {
            LOG_ERR("Power pin set failed: %d", ret);
            return ret;
        }
        k_sleep(K_MSEC(500));
        ret = paw32xx_force_cs(dev, false);
        if (ret != 0) {
            return ret;
        }
        k_sleep(K_MSEC(50));
    }
#endif

    if (!gpio_is_ready_dt(&cfg->irq_gpio)) {
        LOG_ERR("%s is not ready", cfg->irq_gpio.port->name);
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&cfg->irq_gpio, GPIO_INPUT);
    if (ret != 0) {
        LOG_ERR("Motion pin configuration failed: %d", ret);
        return ret;
    }

    gpio_init_callback(&data->motion_cb, paw32xx_motion_handler, BIT(cfg->irq_gpio.pin));
    ret = gpio_add_callback_dt(&cfg->irq_gpio, &data->motion_cb);
    if (ret < 0) {
        LOG_ERR("Could not set motion callback: %d", ret);
        return ret;
    }

    // 启动异步初始化
    k_work_schedule(&data->init_work, K_NO_WAIT);

    ret = paw32xx_interrupt_enable(dev);
    if (ret != 0) {
        LOG_ERR("Motion interrupt configuration failed: %d", ret);
        return ret;
    }

    ret = pm_device_runtime_enable(dev);
    if (ret < 0) {
        LOG_ERR("Failed to enable runtime power management: %d", ret);
        return ret;
    }

    return 0;
}

#ifdef CONFIG_PM_DEVICE
static int paw32xx_pm_action(const struct device *dev, enum pm_device_action action) {
    const struct paw32xx_config *cfg = dev->config;
    struct paw32xx_data *data = dev->data;
    int ret;
    uint8_t val;

    switch (action) {
    case PM_DEVICE_ACTION_SUSPEND:
        // 取消初始化任务，断开IRQ，标记未就绪
        k_work_cancel_delayable(&data->init_work);
        data->ready = false;
        ret = paw32xx_interrupt_disable(dev);
        if (ret < 0) {
            LOG_ERR("Failed to disable IRQ interrupt: %d", ret);
            return ret;
        }
        ret = gpio_pin_configure_dt(&cfg->irq_gpio, GPIO_DISCONNECTED);
        if (ret < 0) {
            LOG_ERR("Failed to disconnect IRQ GPIO: %d", ret);
            return ret;
        }
        val = CONFIGURATION_PD_ENH;
        ret = paw32xx_update_reg(dev, PAW32XX_CONFIGURATION, CONFIGURATION_PD_ENH, val);
        if (ret < 0) {
            return ret;
        }
        break;
    case PM_DEVICE_ACTION_RESUME:
        // 恢复IRQ，重新初始化
        val = 0;
        ret = paw32xx_update_reg(dev, PAW32XX_CONFIGURATION, CONFIGURATION_PD_ENH, val);
        if (ret < 0) {
            return ret;
        }
        ret = gpio_pin_configure_dt(&cfg->irq_gpio, GPIO_INPUT);
        if (ret < 0) {
            LOG_ERR("Failed to configure IRQ GPIO: %d", ret);
            return ret;
        }
        ret = paw32xx_interrupt_enable(dev);
        if (ret < 0) {
            LOG_ERR("Failed to enable IRQ interrupt: %d", ret);
            return ret;
        }
        // 重新启动异步初始化
        data->async_init_retry_count = 0;
        k_work_schedule(&data->init_work, K_NO_WAIT);
        break;
    default:
        return -ENOTSUP;
    }
    return 0;
}
#endif

#define PAW32XX_SPI_MODE                                                                           \
    (SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_MODE_CPOL | SPI_MODE_CPHA | SPI_TRANSFER_MSB)

#define PAW32XX_INIT(n)                                                                            \
    BUILD_ASSERT(IN_RANGE(DT_INST_PROP_OR(n, cpi, RES_MIN), RES_MIN, RES_MAX),                 \
                 "invalid cpi");                                                               \
                                                                                                   \
    static const struct paw32xx_config paw32xx_cfg_##n = {                                         \
        .spi = SPI_DT_SPEC_INST_GET(n, PAW32XX_SPI_MODE, 0),                                       \
        .irq_gpio = GPIO_DT_SPEC_INST_GET(n, irq_gpios),                                           \
        .power_gpio = GPIO_DT_SPEC_INST_GET_OR(n, power_gpios, {0}),                               \
        .cpi = DT_INST_PROP_OR(n, cpi, -1),                                                        \
        .force_awake = DT_INST_PROP(n, force_awake),                                               \
        .disable_burst_read = DT_INST_PROP_OR(n, disable_burst_read, 0),                           \
        .evt_type = DT_PROP(DT_DRV_INST(n), evt_type),                                             \
        .x_input_code = DT_PROP(DT_DRV_INST(n), x_input_code),                                     \
        .y_input_code = DT_PROP(DT_DRV_INST(n), y_input_code),                                     \
        .settings_id = DT_PROP_OR(DT_DRV_INST(n), settings_id, NULL),                              \
        .dt_node_path = DT_NODE_PATH(DT_DRV_INST(n)),                                              \
    };                                                                                             \
                                                                                                   \
    static struct paw32xx_data paw32xx_data_##n;                                                   \
                                                                                                   \
    PM_DEVICE_DT_INST_DEFINE(n, paw32xx_pm_action);                                                \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(n, paw32xx_init, PM_DEVICE_DT_INST_GET(n), &paw32xx_data_##n,            \
                          &paw32xx_cfg_##n, POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(PAW32XX_INIT)

////// Public API (see include/cormoran/paw3222/paw3222_api.h) //////////

#include <string.h>

#define PAW3222_DEV(n) DEVICE_DT_GET(DT_DRV_INST(n)),

static const struct device *paw3222_devs[] = {DT_INST_FOREACH_STATUS_OKAY(PAW3222_DEV)};

size_t paw3222_device_count(void) { return ARRAY_SIZE(paw3222_devs); }

const struct device *paw3222_get_device(size_t index) {
    if (index >= ARRAY_SIZE(paw3222_devs)) {
        return NULL;
    }
    return paw3222_devs[index];
}

bool paw3222_is_ready(const struct device *dev) {
    if (!dev) {
        return false;
    }
    return ((struct paw32xx_data *)dev->data)->ready;
}

int paw3222_get_init_error(const struct device *dev) {
    if (!dev) {
        return -ENODEV;
    }
    return ((struct paw32xx_data *)dev->data)->err;
}

int paw3222_get_device_id(const struct device *dev, char *buf, size_t buf_len) {
    if (!dev || !buf || buf_len == 0) {
        return -EINVAL;
    }
    struct paw32xx_data *data = dev->data;
    strncpy(buf, data->device_id, buf_len - 1);
    buf[buf_len - 1] = '\0';
    return 0;
}

int paw3222_read_register(const struct device *dev, uint8_t addr, uint8_t *value) {
    if (!dev) {
        return -ENODEV;
    }
    return paw32xx_read_reg(dev, addr, value);
}

int paw3222_write_register(const struct device *dev, uint8_t addr, uint8_t value) {
    if (!dev) {
        return -ENODEV;
    }
    struct paw32xx_data *data = dev->data;
    k_mutex_lock(&data->lock, K_FOREVER);
    int err = paw32xx_write_reg(dev, addr, value);
    k_mutex_unlock(&data->lock);
    return err;
}

int paw3222_read_diagnostics(const struct device *dev, struct paw3222_diagnostics *out) {
    if (!dev || !out) {
        return -ENODEV;
    }
    struct paw32xx_data *data = dev->data;
    if (!data->ready) {
        return -EBUSY;
    }

    uint8_t id1 = 0, id2 = 0, motion = 0;
    uint32_t cpi;
    int err;

    k_mutex_lock(&data->lock, K_FOREVER);
    do {
        if ((err = paw32xx_read_reg(dev, PAW32XX_PRODUCT_ID1, &id1)) != 0) {
            break;
        }
        if ((err = paw32xx_read_reg(dev, PAW32XX_PRODUCT_ID2, &id2)) != 0) {
            break;
        }
        err = paw32xx_read_reg(dev, PAW32XX_MOTION, &motion);
    } while (0);
    cpi = data->runtime.cpi;
    k_mutex_unlock(&data->lock);

    if (err) {
        return err;
    }

    out->product_id1 = id1;
    out->product_id2 = id2;
    out->motion = motion;
    out->cpi = cpi;
    return 0;
}

int paw3222_get_runtime_config(const struct device *dev, struct paw3222_runtime_config *out) {
    if (!dev || !out) {
        return -ENODEV;
    }
    struct paw32xx_data *data = dev->data;
    k_mutex_lock(&data->lock, K_FOREVER);
    *out = data->runtime;
    k_mutex_unlock(&data->lock);
    return 0;
}

int paw3222_set_cpi_runtime(const struct device *dev, uint32_t cpi) {
    if (!dev) {
        return -ENODEV;
    }
    if (!IN_RANGE(cpi, RES_MIN, RES_MAX)) {
        return -EINVAL;
    }
    struct paw32xx_data *data = dev->data;
    k_mutex_lock(&data->lock, K_FOREVER);
    data->runtime.cpi = cpi;
    int err = 0;
    if (data->ready) {
        err = paw32xx_set_resolution(dev, (uint16_t)cpi);
    }
    k_mutex_unlock(&data->lock);
    return err;
}

int paw3222_set_force_awake(const struct device *dev, bool enabled) {
    if (!dev) {
        return -ENODEV;
    }
    struct paw32xx_data *data = dev->data;
    k_mutex_lock(&data->lock, K_FOREVER);
    data->runtime.force_awake = enabled;
    int err = 0;
    if (data->ready) {
        err = paw32xx_force_awake(dev, enabled);
    }
    k_mutex_unlock(&data->lock);
    return err;
}

#endif // DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
