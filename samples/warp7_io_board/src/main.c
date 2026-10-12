/*
 * Copyright (c) 2026, Joris Offouga
 * SPDX-License-Identifier: Apache-2.0
 *
 * WaRP7 IO board devices on I2C3, driven by the Cortex-M4 (snippet
 * warp7-io-board): LM75A temperature, MCP7940N time, 24C08 EEPROM, PCF8591
 * inputs (ADC) and output ramp (DAC), and the MCP23008 output 0, the "boot"
 * LED (active low).
 */

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define LED_PIN		0
#define ADC_CHANNELS	4
#define ADC_RESOLUTION	8
#define DAC_STEP	32

static const struct device *const io_gpio = DEVICE_DT_GET(DT_ALIAS(io_gpio));
static const struct device *const io_temp = DEVICE_DT_GET(DT_ALIAS(io_temp));
static const struct device *const io_eeprom =
	DEVICE_DT_GET(DT_ALIAS(io_eeprom));
static const struct device *const io_rtc = DEVICE_DT_GET(DT_ALIAS(io_rtc));
static const struct device *const io_adc = DEVICE_DT_GET(DT_ALIAS(io_adc));
static const struct device *const io_dac = DEVICE_DT_GET(DT_ALIAS(io_dac));

static void print_temp(void)
{
	struct sensor_value val;

	if (sensor_sample_fetch(io_temp) ||
	    sensor_channel_get(io_temp, SENSOR_CHAN_AMBIENT_TEMP, &val)) {
		printk("lm75a: read failed\n");
		return;
	}

	printk("lm75a: %d.%06d C\n", val.val1, val.val2);
}

static void print_time(void)
{
	uint32_t seconds;

	if (counter_get_value(io_rtc, &seconds)) {
		printk("mcp7940n: read failed\n");
		return;
	}

	printk("mcp7940n: %u s\n", seconds);
}

static int setup_adc(void)
{
	struct adc_channel_cfg cfg = {
		.gain = ADC_GAIN_1,
		.reference = ADC_REF_INTERNAL,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
	};
	int ret;

	for (uint8_t ch = 0; ch < ADC_CHANNELS; ch++) {
		cfg.channel_id = ch;
		ret = adc_channel_setup(io_adc, &cfg);
		if (ret) {
			return ret;
		}
	}

	return 0;
}

static void print_adc(void)
{
	uint16_t samples[ADC_CHANNELS];
	const struct adc_sequence seq = {
		.channels = BIT_MASK(ADC_CHANNELS),
		.buffer = samples,
		.buffer_size = sizeof(samples),
		.resolution = ADC_RESOLUTION,
	};

	if (adc_read(io_adc, &seq)) {
		printk("pcf8591: read failed\n");
		return;
	}

	printk("pcf8591:");
	for (uint8_t ch = 0; ch < ADC_CHANNELS; ch++) {
		int32_t mv = samples[ch];

		adc_raw_to_millivolts(adc_ref_internal(io_adc), ADC_GAIN_1,
				      ADC_RESOLUTION, &mv);
		printk(" AIN%u %u (%d mV)", ch, samples[ch], mv);
	}
	printk("\n");
}

static int setup_dac(void)
{
	const struct dac_channel_cfg cfg = {
		.channel_id = 0,
		.resolution = ADC_RESOLUTION,
	};

	return dac_channel_setup(io_dac, &cfg);
}

static void print_eeprom(void)
{
	uint8_t data[16];

	if (eeprom_read(io_eeprom, 0, data, sizeof(data))) {
		printk("eeprom: read failed\n");
		return;
	}

	printk("eeprom:");
	for (size_t i = 0; i < sizeof(data); i++) {
		printk(" %02x", data[i]);
	}
	printk("\n");
}

int main(void)
{
	const struct device *const devs[] = {
		io_gpio, io_temp, io_eeprom, io_rtc, io_adc, io_dac,
	};
	bool led_ok = false;
	bool adc_ok = false;
	bool dac_ok = false;
	bool led = false;
	uint8_t aout = 0;

	for (size_t i = 0; i < ARRAY_SIZE(devs); i++) {
		if (!device_is_ready(devs[i])) {
			printk("%s not ready\n", devs[i]->name);
		}
	}

	if (device_is_ready(io_gpio)) {
		led_ok = !gpio_pin_configure(io_gpio, LED_PIN,
					     GPIO_OUTPUT_INACTIVE |
					     GPIO_ACTIVE_LOW);
	}

	if (device_is_ready(io_eeprom)) {
		print_eeprom();
	}

	if (device_is_ready(io_adc)) {
		adc_ok = !setup_adc();
	}

	if (device_is_ready(io_dac)) {
		dac_ok = !setup_dac();
	}

	while (1) {
		if (device_is_ready(io_temp)) {
			print_temp();
		}
		if (device_is_ready(io_rtc)) {
			print_time();
		}
		if (dac_ok) {
			/* AOUT ramp, AGND to VREF */
			if (!dac_write_value(io_dac, 0, aout)) {
				printk("pcf8591: AOUT %u\n", aout);
			}
			aout += DAC_STEP;
		}
		if (adc_ok) {
			print_adc();
		}
		if (led_ok) {
			led = !led;
			gpio_pin_set(io_gpio, LED_PIN, led);
		}

		k_sleep(K_SECONDS(1));
	}

	return 0;
}
