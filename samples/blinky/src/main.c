#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>


/*
 * A build error on this line means your board is unsupported.
 * See the sample documentation for information on how to fix this.
 */
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led2 = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

/* 1000 msec = 1 sec */
#define SLEEP_TIME 1000

int main(void)
{
	int ret;

	if (!gpio_is_ready_dt(&led0)) {
		return 0;
	}
	if (!gpio_is_ready_dt(&led1)) {
		return 0;
	}
	if (!gpio_is_ready_dt(&led2)) {
		return 0;
	}

	/* Set LED pin as output */
	ret = gpio_pin_configure_dt(&led0, GPIO_OUTPUT_ACTIVE);
	// ret = gpio_pin_configure_dt(&led1, GPIO_OUTPUT_ACTIVE);
	// ret = gpio_pin_configure_dt(&led2, GPIO_OUTPUT_ACTIVE);

	while (1)
	{
		/* Set pin to HIGH/LOW every 1 second */
		gpio_pin_toggle_dt(&led0);
		k_sleep(K_MSEC(SLEEP_TIME));
	}
}
