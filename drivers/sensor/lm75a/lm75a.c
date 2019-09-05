/*
 * Copyright (c) 2019 Joris Offouga
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <device.h>
#include <i2c.h>
#include <gpio.h>
#include <misc/byteorder.h>
#include <misc/util.h>
#include <kernel.h>
#include <sensor.h>
#include <misc/__assert.h>
#include <logging/log.h>

#include "lm75a.h"

LOG_MODULE_REGISTER(LM75A, CONFIG_SENSOR_LOG_LEVEL);

#define LM75A_REG_TEMP 0x00

int lm75a_reg_read(struct device *dev, u8_t reg)
{
	struct lm75a_data *data = dev->driver_data;
	const struct lm75a_config *config = dev->config->config_info;
	u16_t *val = &data->temp;

	struct i2c_msg msgs[2] = {
		{
			.buf = &reg,
			.len = 1,
			.flags = I2C_MSG_WRITE | I2C_MSG_RESTART,
		},
		{
			.buf = (u8_t *)val,
			.len = 2,
			.flags = I2C_MSG_READ | I2C_MSG_STOP,
		},
	};

	if (i2c_transfer(data->i2c, msgs, 2, config->i2c_address) < 0) {
		return -EIO;
	}

	*val = sys_be16_to_cpu(*val);

	return 0;
}

static int lm75a_sample_fetch(struct device *dev, enum sensor_channel chan)
{

	__ASSERT_NO_MSG(chan == SENSOR_CHAN_ALL || chan == SENSOR_CHAN_AMBIENT_TEMP);

	if (lm75a_reg_read(dev, LM75A_REG_TEMP) < 0) {
		return -EIO;
	}

	return 0;
}

static int lm75a_channel_get(struct device *dev,
			       enum sensor_channel chan,
			       struct sensor_value *val)
{
	struct lm75a_data *data = dev->driver_data;
	s32_t uval;

	if (chan != SENSOR_CHAN_AMBIENT_TEMP) {
		return -ENOTSUP;
	}

	uval = (s32_t) ((data->temp >> 5) * 0.125);
	val->val1 = uval;
	val->val2 = uval;

	return 0;
}

int lm75a_init(struct device *dev)
{
	const struct lm75a_config *config = dev->config->config_info;
	struct lm75a_data *data = dev->driver_data;
	u8_t id;

	data->i2c = device_get_binding(config->i2c_name);
	if (data->i2c == NULL) {
		LOG_DBG("Failed to get pointer to %s device!",
			    config->i2c_name);
		return -EINVAL;
	}

	return 0;
}

static const struct sensor_driver_api lm75a_driver_api = {
	.sample_fetch = lm75a_sample_fetch,
	.channel_get = lm75a_channel_get,
};

static const struct lm75a_config lm75a_config = {
	.i2c_name = DT_INST_0_NXP_LM75A_BUS_NAME,
	.i2c_address = DT_INST_0_NXP_LM75A_BASE_ADDRESS,
};

static struct lm75a_data lm75a_driver;

DEVICE_AND_API_INIT(lm75a, DT_INST_0_NXP_LM75A_LABEL, lm75a_init, 
			&lm75a_driver, &lm75a_config, 
			POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,
			&lm75a_driver_api);
