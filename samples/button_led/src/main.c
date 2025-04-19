#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>


static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec sw0 = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback gpio_cb;

void gpio_callback(const struct device *port,
                   struct gpio_callback *cb, uint32_t pins)
{
    int ret;

    printk("Button pressed at %d\n", k_cycle_get_32());

    ret = gpio_pin_toggle_dt(&led0);

    if (ret)
    {
        printk("Error set %d!\n", led0.pin);
    }
}

int main(void)
{
    int ret;

    if(!gpio_is_ready_dt(&led0))
    {
        return 0;
    }
    if(!gpio_is_ready_dt(&sw0))
    {
        return 0;
    }

    gpio_pin_configure_dt(&led0, GPIO_OUTPUT_ACTIVE);
    gpio_pin_interrupt_configure_dt(&sw0, GPIO_INT_EDGE_TO_ACTIVE);

    gpio_init_callback(&gpio_cb, gpio_callback, BIT(sw0.pin));

    ret = gpio_add_callback(sw0.port, &gpio_cb);

    if (ret)
    {
        printk("Cannot setup callback!\n");
        return 0;
    }

    printk("Started Simple Application Zephyr\n");
    printk("Press The user button for turn on or turn off led\n");

    while (1)
    {
        k_sleep(Z_TIMEOUT_TICKS(500));
    }
}
