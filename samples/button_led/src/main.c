#include <zephyr.h>
#include <misc/printk.h>
#include <misc/util.h>
#include <device.h>
#include <gpio.h>

#define GPIO_DRV_NAME SW0_GPIO_CONTROLLER
#define LED0 LED0_GPIO_PIN
#define LED1 LED1_GPIO_PIN
#define GPIO_KEY_PIN SW0_GPIO_PIN
#define EDGE (GPIO_INT_EDGE | GPIO_INT_ACTIVE_LOW)
#define PULL_UP 0

static struct gpio_callback gpio_cb;

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

    ret = gpio_pin_toggle(port, LED0);

    if (ret)
    {
        printk("Error set " GPIO_DRV_NAME "%d!\n", LED0);
    }

    ret = gpio_pin_toggle(port, LED1);

    if (ret)
    {
        printk("Error set " GPIO_DRV_NAME "%d!\n", LED1);
    }
}

void main(void)
{
    struct device *gpio_dev;
    int ret;

    gpio_dev = device_get_binding(GPIO_DRV_NAME);

    if (!gpio_dev)
    {
        printk("Cannot find %s!\n", GPIO_DRV_NAME);
        return;
    }

    ret = gpio_pin_configure(gpio_dev, LED0, (GPIO_DIR_OUT));

    if (ret)
    {
        printk("Error configuring " GPIO_DRV_NAME "%d!\n", LED0);
    }

    ret = gpio_pin_configure(gpio_dev, LED1, (GPIO_DIR_OUT));

    if (ret)
    {
        printk("Error configuring " GPIO_DRV_NAME "%d!\n", LED1);
    }

    ret = gpio_pin_configure(gpio_dev, GPIO_KEY_PIN, GPIO_DIR_IN | GPIO_INT | PULL_UP | EDGE);
    if (ret)
    {
        printk("Error configuring " GPIO_DRV_NAME "%d!\n", GPIO_KEY_PIN);
    }

    gpio_init_callback(&gpio_cb, gpio_callback, BIT(GPIO_KEY_PIN));

    ret = gpio_add_callback(gpio_dev, &gpio_cb);

    if (ret)
    {
        printk("Cannot setup callback!\n");
    }

    ret = gpio_pin_enable_callback(gpio_dev, GPIO_KEY_PIN);

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