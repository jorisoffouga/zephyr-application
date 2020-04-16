/*
 * Copyright (c) 2019, Joris Offouga
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <device.h>
#include <drivers/sensor.h>

void main(void)
{
	struct sensor_value temp;
	struct device *dev = device_get_binding("LM75A");

	if (!dev) {
		printk("Could not get lm75a device\n");
		return;
	}

	while (1) {
		sensor_sample_fetch(dev);
		sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);

		printk("Temp is %d (%d micro)\n", temp.val1,
                temp.val2);
		k_sleep(Z_TIMEOUT_TICKS(1000));
	}
}
