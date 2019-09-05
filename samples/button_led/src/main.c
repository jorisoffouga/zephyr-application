#include <zephyr.h>
#include <misc/printk.h>
#include <misc/util.h>
#include <device.h>
#include <gpio.h>

#define SW0_PORT DT_ALIAS_SW0_GPIOS_CONTROLLER
#define SW0_PIN DT_ALIAS_SW0_GPIOS_PIN

#define LED0 DT_ALIAS_LED0_GPIOS_PIN
#define LED_PORT DT_ALIAS_LED0_GPIOS_CONTROLLER

/* change to use another GPIO pin interrupt config */
#ifdef DT_ALIAS_SW0_GPIOS_FLAGS
#define EDGE    (DT_ALIAS_SW0_GPIOS_FLAGS | GPIO_INT_EDGE)
#else
/*
 * If DT_ALIAS_SW0_GPIOS_FLAGS not defined used default EDGE value.
 * Change this to use a different interrupt trigger
 */
#define EDGE    (GPIO_INT_EDGE | GPIO_INT_ACTIVE_LOW)
#endif

/* change this to enable pull-up/pull-down */
#ifndef DT_ALIAS_SW0_GPIOS_FLAGS
#ifdef DT_ALIAS_SW0_GPIOS_PIN_PUD
#define DT_ALIAS_SW0_GPIOS_FLAGS DT_ALIAS_SW0_GPIOS_PIN_PUD
#else
#define DT_ALIAS_SW0_GPIOS_FLAGS 0
#endif
#endif
#define PULL_UP DT_ALIAS_SW0_GPIOS_FLAGS

static struct gpio_callback gpio_cb;
static struct device *led;

static int gpio_pin_toggle(struct device *port, u32_t pin)
{
    u32_t value = 0;
    u8_t ret = gpio_pin_read(port, pin, &value);
    if (ret < 0)
    {
        printk("Error gpio read");
        return -1;
    }

    if (value)
    {
        return gpio_pin_write(port, pin, 0);
    }
    else
    {
        return gpio_pin_write(port, pin, 1);
    }
}

void gpio_callback(struct device *port,
                   struct gpio_callback *cb, u32_t pins)
{
    int ret;

    printk("Button pressed at %d\n", k_cycle_get_32());

    ret = gpio_pin_toggle(led, LED0);

    if (ret)
    {
        printk("Error set " LED_PORT "%d!\n", LED0);
    }
}

void main(void)
{
    int ret;
    struct device *button_dev;

    button_dev = device_get_binding(SW0_PORT);

    if (!button_dev)
    {
        printk("Cannot find %s!\n", SW0_PORT);
        return;
    }

	led = device_get_binding(LED_PORT);

	if(!led){
		printk("Cannot find %s!\n", SW0_PORT);
        return;
	}

    ret = gpio_pin_configure(led, LED0, (GPIO_DIR_OUT));

    if (ret)
    {
        printk("Error configuring " LED_PORT "%d!\n", LED0);
    }

    ret = gpio_pin_configure(button_dev, SW0_PIN, GPIO_DIR_IN | GPIO_INT | PULL_UP | EDGE);
    if (ret)
    {
        printk("Error configuring " SW0_PORT "%d!\n", SW0_PIN);
    }

    gpio_init_callback(&gpio_cb, gpio_callback, BIT(SW0_PIN));

    ret = gpio_add_callback(button_dev, &gpio_cb);

    if (ret)
    {
        printk("Cannot setup callback!\n");
    }

    ret = gpio_pin_enable_callback(button_dev, SW0_PIN);

    if (ret)
    {
        printk("Error enabling callback!\n");
    }

    printk("Started Simple Application Zephyr\n");
    printk("Press The user button for turn on or turn off led\n");

    while (1)
    {
        k_sleep(500);
    }
}
