/*
 * Copyright (c) 2019 Joris Offouga
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT nxp_lm75a

#include <device.h>
#include <drivers/i2c.h>
#include <drivers/sensor.h>
#include <sys/byteorder.h>
#include <sys/util.h>
#include <sys/__assert.h>
#include <logging/log.h>

LOG_MODULE_REGISTER(LM75A, CONFIG_SENSOR_LOG_LEVEL);

#define LM75A_REG_TEMP 0x00

struct lm75a_config {
	char *i2c_name;
	uint8_t i2c_address;
};

struct lm75a_data {
	struct device *i2c;
	uint16_t temp;
};

static int lm75a_sample_fetch(struct device *dev, enum sensor_channel chan)
{

	__ASSERT_NO_MSG(chan == SENSOR_CHAN_ALL || chan == SENSOR_CHAN_AMBIENT_TEMP);

	int ret;
	uint8_t temp[2];
	struct lm75a_data *data = dev->data;
	const struct lm75a_config *config = dev->config;

	ret = i2c_burst_read(data->i2c,  config->i2c_address,
		LM75A_REG_TEMP, temp, sizeof(temp));

	if (ret == 0) {
		data->temp = (temp[0] << 8) | temp[1];
	} else {
		LOG_ERR("read register err");
	}

	return ret;
}

static int lm75a_channel_get(struct device *dev,
			       enum sensor_channel chan,
			       struct sensor_value *val)
{
	struct lm75a_data *data = dev->data;
	int32_t uval;

	if (chan != SENSOR_CHAN_AMBIENT_TEMP) {
		return -ENOTSUP;
	}

	uval = (int32_t) (((data->temp >> 5) / 8) * 1000);
	val->val1 = uval;
	val->val2 = uval;

	return 0;
}

int lm75a_init(struct device *dev)
{
	const struct lm75a_config *config = dev->config;
	struct lm75a_data *data = dev->data;

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
	.i2c_name = DT_INST_BUS_LABEL(0),
	.i2c_address = DT_INST_REG_ADDR(0),
};

static struct lm75a_data lm75a_driver;

DEVICE_AND_API_INIT(lm75a, DT_INST_LABEL(0), lm75a_init,
			&lm75a_driver, &lm75a_config, 
			POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,
			&lm75a_driver_api);
