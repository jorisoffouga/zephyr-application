#include <zephyr.h>
#include <device.h>
#include <drivers/sensor.h>
#include <uart.h>

#define UART_PORT 	"UART_2"
#define BME280 			"BME280"
#define STACKSIZE 	1024			/* size of stack area used by each thread */
#define PRIORITY 		7					/* scheduling priority used by each thread */
#define TEMPERATURE	0
#define PRESSION		1
#define HUMIDITE 		2

u8_t data[6];
struct sensor_value temp, press, humidity;


void concatene(u8_t *array, struct sensor_value* temp, struct sensor_value* press, struct sensor_value* humidity){
	array[0] = (s8_t)temp->val1;
	array[1] = (s8_t)temp->val2;
	array[2] = (s8_t)press->val1;
	array[3] = (s8_t)press->val2;
	array[4] = (s8_t)humidity->val1;
	array[5] = (s8_t)humidity->val2;
}

void send_data( struct device *uart, s8_t* data, u8_t type){
	/* 	0 --> temp
			1 --> press
			2 --> humidity */
	uart_poll_out(uart, 0x55);
	uart_poll_out(uart, 0x02);
	uart_poll_out(uart, type);
	uart_poll_out(uart, data[(type*2)]);
	uart_poll_out(uart, data[(type*2)+1]);
}

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
		sensor_sample_fetch(bme280);
		sensor_channel_get(bme280, SENSOR_CHAN_AMBIENT_TEMP, &temp);
		sensor_channel_get(bme280, SENSOR_CHAN_PRESS, &press);
		sensor_channel_get(bme280, SENSOR_CHAN_HUMIDITY, &humidity);

		concatene(data, &temp, &press, &humidity);

		send_data(uart_handle, data, 0);
		k_sleep(333);
		send_data(uart_handle, data, 1);
		k_sleep(333);
		send_data(uart_handle, data, 2);
		k_sleep(333);
	}
}

