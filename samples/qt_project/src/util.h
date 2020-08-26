#ifndef UTIL_H
#define UTIL_H

#include <zephyr.h>
#include <device.h>

void gpio_pin_toggle(struct device *port, uint32_t pin);

struct fifo_data_t
{
	void *fifo_reserved;
	uint8_t *data;
};

struct bridge_t
{
	struct
	{
		struct device *handle;
	} uart;

	struct
	{
		struct device *handle;
	} gpio;
};


#endif //UTIL_H
