/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <device.h>
#include <gpio.h>

#define LED_PORT LED0_GPIO_CONTROLLER
#define LED0 LED0_GPIO_PIN
#define LED1 LED1_GPIO_PIN
#define LED2 LED2_GPIO_PIN
#define LED3 LED3_GPIO_PIN

/* 1000 msec = 1 sec */
#define SLEEP_TIME 1000

void main(void)
{
	int cnt = 0;
	struct device *dev;

	dev = device_get_binding(LED_PORT);
	/* Set LED pin as output */
	gpio_pin_configure(dev, LED0, GPIO_DIR_OUT);
	gpio_pin_configure(dev, LED1, GPIO_DIR_OUT);
	gpio_pin_configure(dev, LED2, GPIO_DIR_OUT);
	gpio_pin_configure(dev, LED3, GPIO_DIR_OUT);

	while (1)
	{
		/* Set pin to HIGH/LOW every 1 second */
		gpio_pin_write(dev, LED0, cnt % 2);
		gpio_pin_write(dev, LED1, cnt % 2);
		gpio_pin_write(dev, LED2, cnt % 2);
		gpio_pin_write(dev, LED3, cnt % 2);
		cnt++;
		k_sleep(SLEEP_TIME);
	}
}
