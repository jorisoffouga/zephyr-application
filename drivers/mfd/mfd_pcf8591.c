/*
 * Copyright (c) 2026, Joris Offouga
 * SPDX-License-Identifier: Apache-2.0
 *
 * NXP PCF8591 8-bit A/D and D/A converter. The control byte holds both the
 * selected input and the analog output enable: this driver keeps it, so
 * that input conversions do not turn the output off, and serializes the
 * ADC and DAC children.
 */

#define DT_DRV_COMPAT nxp_pcf8591

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <za/drivers/mfd/pcf8591.h>

LOG_MODULE_REGISTER(mfd_pcf8591, CONFIG_MFD_LOG_LEVEL);

/* Control byte */
#define PCF8591_CTRL_AOE	BIT(6)	/* analog output enable */
#define PCF8591_CTRL_CHAN_MASK	GENMASK(1, 0)
/* Input programming 0 (bits 5:4): four single-ended inputs, no auto-increment */

struct mfd_pcf8591_config {
	struct i2c_dt_spec i2c;
	uint16_t vref_mv;
};

struct mfd_pcf8591_data {
	struct k_mutex lock;
	uint8_t ctrl;
};

int mfd_pcf8591_adc_read(const struct device *dev, uint8_t channel,
			 uint8_t *value)
{
	const struct mfd_pcf8591_config *config = dev->config;
	struct mfd_pcf8591_data *data = dev->data;
	uint8_t buf[2];
	int ret;

	if (channel >= PCF8591_ADC_CHANNELS) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	data->ctrl = (data->ctrl & ~PCF8591_CTRL_CHAN_MASK) | channel;

	/*
	 * A read returns the conversion started by the previous one: the
	 * first byte is the previously selected input, the second the new
	 * channel. Only the control byte is written: the DAC register keeps
	 * its value, and AOE in data->ctrl keeps the output on.
	 */
	ret = i2c_write_read_dt(&config->i2c, &data->ctrl, sizeof(data->ctrl),
				buf, sizeof(buf));
	if (ret == 0) {
		*value = buf[1];
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

int mfd_pcf8591_dac_write(const struct device *dev, uint8_t value)
{
	const struct mfd_pcf8591_config *config = dev->config;
	struct mfd_pcf8591_data *data = dev->data;
	uint8_t buf[2];
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	buf[0] = data->ctrl | PCF8591_CTRL_AOE;
	buf[1] = value;

	ret = i2c_write_dt(&config->i2c, buf, sizeof(buf));
	if (ret == 0) {
		data->ctrl = buf[0];
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

uint16_t mfd_pcf8591_vref_mv(const struct device *dev)
{
	const struct mfd_pcf8591_config *config = dev->config;

	return config->vref_mv;
}

static int mfd_pcf8591_init(const struct device *dev)
{
	const struct mfd_pcf8591_config *config = dev->config;
	struct mfd_pcf8591_data *data = dev->data;
	uint8_t ctrl = 0;
	int ret;

	if (!i2c_is_ready_dt(&config->i2c)) {
		LOG_ERR("I2C bus %s not ready", config->i2c.bus->name);
		return -ENODEV;
	}

	k_mutex_init(&data->lock);

	/* Output off, input AIN0: also checks the chip answers */
	ret = i2c_write_dt(&config->i2c, &ctrl, sizeof(ctrl));
	if (ret < 0) {
		LOG_ERR("no PCF8591 at 0x%02x: %d", config->i2c.addr, ret);
		return ret;
	}

	data->ctrl = ctrl;

	return 0;
}

#define MFD_PCF8591_DEFINE(inst)                                               \
	static const struct mfd_pcf8591_config mfd_pcf8591_config_##inst = {   \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                             \
		.vref_mv = DT_INST_PROP(inst, vref_mv),                        \
	};                                                                     \
	static struct mfd_pcf8591_data mfd_pcf8591_data_##inst;                \
	DEVICE_DT_INST_DEFINE(inst, mfd_pcf8591_init, NULL,                    \
			      &mfd_pcf8591_data_##inst,                        \
			      &mfd_pcf8591_config_##inst, POST_KERNEL,         \
			      CONFIG_MFD_PCF8591_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(MFD_PCF8591_DEFINE)
