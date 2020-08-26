#include "util.h"
#include <drivers/gpio.h>

/*
	Inverse state of pin
*/
void gpio_pin_toggle(struct device *port, uint32_t pin)
{
	uint32_t value = 0;
	uint8_t ret = gpio_pin_read(port, pin, &value);

	if (ret < 0)
	{
		printk("Error gpio read");
		return;
	}

	if (value)
	{
		gpio_pin_write(port, pin, 0);
		printk("Turn Off gpio %d\n", pin);
	}
	else
	{
		gpio_pin_write(port, pin, 1);
		printk("Turn on gpio %d\n", pin);
	}
}
