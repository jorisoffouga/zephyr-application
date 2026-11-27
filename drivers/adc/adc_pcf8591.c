/*
 * Copyright (c) 2026, Joris Offouga
 * SPDX-License-Identifier: Apache-2.0
 *
 * NXP PCF8591 analog inputs AIN0-AIN3: 8-bit, single-ended, full scale VREF.
 * Synchronous reads, one conversion per channel of the sequence; one
 * uint16_t sample per channel in the buffer.
 */

#define DT_DRV_COMPAT nxp_pcf8591_adc

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <za/drivers/mfd/pcf8591.h>

LOG_MODULE_REGISTER(adc_pcf8591, CONFIG_ADC_LOG_LEVEL);

struct adc_pcf8591_config {
	const struct device *mfd;
};

static int adc_pcf8591_channel_setup(const struct device *dev,
				     const struct adc_channel_cfg *cfg)
{
	if (cfg->channel_id >= PCF8591_ADC_CHANNELS) {
		LOG_ERR("invalid channel %u", cfg->channel_id);
		return -EINVAL;
	}

	if (cfg->differential) {
		LOG_ERR("differential inputs not supported");
		return -ENOTSUP;
	}

	if (cfg->gain != ADC_GAIN_1) {
		LOG_ERR("gain must be 1");
		return -ENOTSUP;
	}

	/* VREF is the only reference, reported as ref_internal */
	if (cfg->reference != ADC_REF_INTERNAL &&
	    cfg->reference != ADC_REF_EXTERNAL0) {
		LOG_ERR("reference must be internal or external0 (VREF)");
		return -ENOTSUP;
	}

	if (cfg->acquisition_time != ADC_ACQ_TIME_DEFAULT) {
		LOG_ERR("acquisition time must be the default");
		return -ENOTSUP;
	}

	return 0;
}

static int adc_pcf8591_read(const struct device *dev,
			    const struct adc_sequence *seq)
{
	const struct adc_pcf8591_config *config = dev->config;
	uint32_t channels = seq->channels;
	uint16_t *buf = seq->buffer;
	uint8_t value;
	int ret;

	if (seq->options) {
		LOG_ERR("sequence options not supported");
		return -ENOTSUP;
	}

	if (seq->resolution != PCF8591_RESOLUTION || seq->oversampling ||
	    seq->calibrate) {
		LOG_ERR("8-bit resolution, no oversampling nor calibration");
		return -EINVAL;
	}

	if (!channels || channels & ~BIT_MASK(PCF8591_ADC_CHANNELS)) {
		LOG_ERR("invalid channels 0x%08x", channels);
		return -EINVAL;
	}

	if (seq->buffer_size < POPCOUNT(channels) * sizeof(*buf)) {
		return -ENOMEM;
	}

	while (channels) {
		uint8_t channel = find_lsb_set(channels) - 1;

		ret = mfd_pcf8591_adc_read(config->mfd, channel, &value);
		if (ret < 0) {
			return ret;
		}

		*buf++ = value;
		channels &= ~BIT(channel);
	}

	return 0;
}

#ifdef CONFIG_ADC_ASYNC
static int adc_pcf8591_read_async(const struct device *dev,
				  const struct adc_sequence *seq,
				  struct k_poll_signal *async)
{
	return -ENOTSUP;
}
#endif

static int adc_pcf8591_init(const struct device *dev)
{
	const struct adc_pcf8591_config *config = dev->config;

	if (!device_is_ready(config->mfd)) {
		LOG_ERR("PCF8591 %s not ready", config->mfd->name);
		return -ENODEV;
	}

	return 0;
}

#define ADC_PCF8591_DEFINE(inst)                                               \
	static const struct adc_pcf8591_config adc_pcf8591_config_##inst = {   \
		.mfd = DEVICE_DT_GET(DT_INST_PARENT(inst)),                    \
	};                                                                     \
	static DEVICE_API(adc, adc_pcf8591_api_##inst) = {                     \
		.channel_setup = adc_pcf8591_channel_setup,                    \
		.read = adc_pcf8591_read,                                      \
		IF_ENABLED(CONFIG_ADC_ASYNC,                                   \
			   (.read_async = adc_pcf8591_read_async,))            \
		.ref_internal = DT_PROP(DT_INST_PARENT(inst), vref_mv),        \
	};                                                                     \
	DEVICE_DT_INST_DEFINE(inst, adc_pcf8591_init, NULL, NULL,              \
			      &adc_pcf8591_config_##inst, POST_KERNEL,         \
			      CONFIG_ADC_PCF8591_INIT_PRIORITY,                \
			      &adc_pcf8591_api_##inst);

DT_INST_FOREACH_STATUS_OKAY(ADC_PCF8591_DEFINE)
