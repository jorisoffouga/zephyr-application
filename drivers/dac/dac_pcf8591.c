/*
 * Copyright (c) 2026, Joris Offouga
 * SPDX-License-Identifier: Apache-2.0
 *
 * NXP PCF8591 analog output AOUT: channel 0, 8-bit, AGND to VREF. The output
 * is enabled by the first write.
 */

#define DT_DRV_COMPAT nxp_pcf8591_dac

#include <zephyr/device.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <za/drivers/mfd/pcf8591.h>

LOG_MODULE_REGISTER(dac_pcf8591, CONFIG_DAC_LOG_LEVEL);

struct dac_pcf8591_config {
	const struct device *mfd;
};

static int dac_pcf8591_channel_setup(const struct device *dev,
				     const struct dac_channel_cfg *cfg)
{
	if (cfg->channel_id != 0) {
		LOG_ERR("invalid channel %u", cfg->channel_id);
		return -EINVAL;
	}

	if (cfg->resolution != PCF8591_RESOLUTION) {
		LOG_ERR("resolution must be %u bits", PCF8591_RESOLUTION);
		return -ENOTSUP;
	}

	return 0;
}

static int dac_pcf8591_write_value(const struct device *dev, uint8_t channel,
				   uint32_t value)
{
	const struct dac_pcf8591_config *config = dev->config;

	if (channel != 0) {
		return -EINVAL;
	}

	if (value > BIT_MASK(PCF8591_RESOLUTION)) {
		return -EINVAL;
	}

	return mfd_pcf8591_dac_write(config->mfd, value);
}

static int dac_pcf8591_init(const struct device *dev)
{
	const struct dac_pcf8591_config *config = dev->config;

	if (!device_is_ready(config->mfd)) {
		LOG_ERR("PCF8591 %s not ready", config->mfd->name);
		return -ENODEV;
	}

	return 0;
}

static DEVICE_API(dac, dac_pcf8591_api) = {
	.channel_setup = dac_pcf8591_channel_setup,
	.write_value = dac_pcf8591_write_value,
};

#define DAC_PCF8591_DEFINE(inst)                                               \
	static const struct dac_pcf8591_config dac_pcf8591_config_##inst = {   \
		.mfd = DEVICE_DT_GET(DT_INST_PARENT(inst)),                    \
	};                                                                     \
	DEVICE_DT_INST_DEFINE(inst, dac_pcf8591_init, NULL, NULL,              \
			      &dac_pcf8591_config_##inst, POST_KERNEL,         \
			      CONFIG_DAC_PCF8591_INIT_PRIORITY,                \
			      &dac_pcf8591_api);

DT_INST_FOREACH_STATUS_OKAY(DAC_PCF8591_DEFINE)
