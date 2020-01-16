#include "util.h"
#include <gpio.h>

/*
	Inverse state of pin
*/
void gpio_pin_toggle(struct device *port, u32_t pin)
{
	u32_t value = 0;
	u8_t ret = gpio_pin_read(port, pin, &value);

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
