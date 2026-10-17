/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT raspberrypi_dsi_panel_mcu

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/mipi_dsi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(rpi_dsi_panel_mcu, CONFIG_DISPLAY_LOG_LEVEL);

#define RPI_PANEL_MCU_REG_ID      0x80U
#define RPI_PANEL_MCU_REG_PORTA   0x81U
#define RPI_PANEL_MCU_REG_POWERON 0x85U
#define RPI_PANEL_MCU_REG_PWM     0x86U

#define RPI_PANEL_MCU_ID_1 0xC3U
#define RPI_PANEL_MCU_ID_2 0xDEU

/* PORTA bit signalling that the panel power-up sequence is complete */
#define RPI_PANEL_MCU_PORTA_POWERON_DONE BIT(2)

/* Settling time the MCU needs after power-up and after each command */
#define RPI_PANEL_MCU_DELAY_MS 100

struct rpi_dsi_panel_mcu_config {
	struct i2c_dt_spec i2c;
	const struct device *mipi_dsi;
	uint8_t data_lanes;
	uint8_t pixel_format;
	uint8_t brightness;
};

static int rpi_dsi_panel_mcu_write(const struct device *dev, uint8_t reg, uint8_t val)
{
	const struct rpi_dsi_panel_mcu_config *config = dev->config;
	int ret;

	ret = i2c_reg_write_byte_dt(&config->i2c, reg, val);
	if (ret < 0) {
		LOG_ERR("Writing 0x%02x to register 0x%02x failed (%d)", val, reg, ret);
		return ret;
	}

	k_msleep(RPI_PANEL_MCU_DELAY_MS);

	return 0;
}

static int rpi_dsi_panel_mcu_init(const struct device *dev)
{
	const struct rpi_dsi_panel_mcu_config *config = dev->config;
	struct mipi_dsi_device mdev = {0};
	uint8_t id;
	int ret;

	if (!i2c_is_ready_dt(&config->i2c)) {
		LOG_ERR("I2C bus %s not ready", config->i2c.bus->name);
		return -ENODEV;
	}

	k_msleep(RPI_PANEL_MCU_DELAY_MS);

	ret = i2c_reg_read_byte_dt(&config->i2c, RPI_PANEL_MCU_REG_ID, &id);
	if (ret < 0) {
		LOG_ERR("Reading the MCU ID failed (%d)", ret);
		return ret;
	}

	if ((id != RPI_PANEL_MCU_ID_1) && (id != RPI_PANEL_MCU_ID_2)) {
		LOG_ERR("Unknown MCU ID 0x%02x", id);
		return -ENODEV;
	}

	/* Power cycle the bridge and the panel */
	ret = rpi_dsi_panel_mcu_write(dev, RPI_PANEL_MCU_REG_POWERON, 0x00U);
	if (ret < 0) {
		return ret;
	}

	ret = rpi_dsi_panel_mcu_write(dev, RPI_PANEL_MCU_REG_POWERON, 0x01U);
	if (ret < 0) {
		return ret;
	}

	ret = rpi_dsi_panel_mcu_write(dev, RPI_PANEL_MCU_REG_PORTA,
				      RPI_PANEL_MCU_PORTA_POWERON_DONE);
	if (ret < 0) {
		return ret;
	}

	ret = rpi_dsi_panel_mcu_write(dev, RPI_PANEL_MCU_REG_PWM, config->brightness);
	if (ret < 0) {
		return ret;
	}

	mdev.data_lanes = config->data_lanes;
	mdev.pixfmt = config->pixel_format;
	mdev.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST | MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_attach(config->mipi_dsi, 0, &mdev);
	if (ret < 0) {
		LOG_ERR("Could not attach to MIPI DSI host (%d)", ret);
		return ret;
	}

	LOG_DBG("Panel MCU 0x%02x initialized", id);

	return 0;
}

#define RPI_DSI_PANEL_MCU_DEFINE(n)                                                                \
	static const struct rpi_dsi_panel_mcu_config rpi_dsi_panel_mcu_config_##n = {              \
		.i2c = I2C_DT_SPEC_INST_GET(n),                                                    \
		.mipi_dsi = DEVICE_DT_GET(DT_INST_PHANDLE(n, mipi_dsi)),                           \
		.data_lanes = DT_INST_PROP_BY_IDX(n, data_lanes, 0),                               \
		.pixel_format = DT_INST_PROP(n, pixel_format),                                     \
		.brightness = DT_INST_PROP(n, brightness),                                         \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, rpi_dsi_panel_mcu_init, NULL, NULL,                               \
			      &rpi_dsi_panel_mcu_config_##n, POST_KERNEL,                          \
			      CONFIG_DISPLAY_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(RPI_DSI_PANEL_MCU_DEFINE)
