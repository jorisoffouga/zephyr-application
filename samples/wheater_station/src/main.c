#include <zephyr.h>
#include <device.h>
#include <drivers/sensor.h>
#include <uart.h>

#define UART_PORT "UART_2"
#define BME280 "BME280"
/* size of stack area used by each thread */
#define STACKSIZE 1024

/* scheduling priority used by each thread */
#define PRIORITY 7

// static struct bridge_t bridge;

void main(void)
{
	struct device *bme280 = device_get_binding(BME280);
	struct device *uart_handle = device_get_binding(UART_PORT);

	if (uart_handle == NULL) {
		printk("Could not get UART_2 device\n");
		return;
	}

	if (bme280 == NULL) {
		printk("Could not get BME280 device\n");
		return;
	}

	printk("dev %p name %s\n", bme280, bme280->config->name);

	while (1) {
		struct sensor_value temp, press, humidity;

		sensor_sample_fetch(bme280);
		sensor_channel_get(bme280, SENSOR_CHAN_AMBIENT_TEMP, &temp);
		sensor_channel_get(bme280, SENSOR_CHAN_PRESS, &press);
		sensor_channel_get(bme280, SENSOR_CHAN_HUMIDITY, &humidity);

		printk("temp: %d.%06d; press: %d.%06d; humidity: %d.%06d\n",
		      temp.val1, temp.val2, press.val1, press.val2,
		      humidity.val1, humidity.val2);
		k_sleep(K_MSEC(1000));
	}
}

