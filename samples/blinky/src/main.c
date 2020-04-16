#include <device.h>
#include <drivers/gpio.h>

#define LED_PORT DT_ALIAS_LED0_GPIOS_CONTROLLER
#define LED0 DT_ALIAS_LED0_GPIOS_PIN
#define LED1 DT_ALIAS_LED1_GPIOS_PIN
#define LED2 DT_ALIAS_LED2_GPIOS_PIN
#define LED3 DT_ALIAS_LED3_GPIOS_PIN

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
		k_sleep(Z_TIMEOUT_TICKS(SLEEP_TIME));
	}
}
