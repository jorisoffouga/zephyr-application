#include <zephyr.h>
#include <device.h>
#include <drivers/sensor.h>
#include <drivers/uart.h>

#define UART_PORT 	"UART_2"
#define BME280 		"BME280"
#define SOF 0x55

static void message_compose(u8_t *array, struct sensor_value* value, u8_t len, u8_t data_type){
	array[0] = SOF;
	array[1] = len;
	array[2] = data_type;
	array[3] = (s8_t)value->val1;
	array[4] = (s8_t)value->val2;
}

static void uart_send(struct device *uart, s8_t* data, u8_t len){

	for (u8_t i = 0; i < len; i++)
	{
		uart_poll_out(uart, data[i]);
	}
}

static void send_message(struct device *uart, struct sensor_value *value, u8_t data_type)
{
	u8_t data[6];

	message_compose(data, value, 2, data_type);
	uart_send(uart, data, sizeof(data));
}

void main(void)
{
	struct device *bme280 = device_get_binding(BME280);
	struct device *uart_handle = device_get_binding(UART_PORT);
	struct sensor_value bme280_data[3];

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
		sensor_channel_get(bme280, SENSOR_CHAN_AMBIENT_TEMP, &bme280_data[0]);
		sensor_channel_get(bme280, SENSOR_CHAN_PRESS, &bme280_data[1]);
		sensor_channel_get(bme280, SENSOR_CHAN_HUMIDITY, &bme280_data[2]);

		for (u8_t i = 0; i < 3; i++)
		{
			send_message(uart_handle, &bme280_data[i], i);
			k_sleep(1000);
		}

	}
}
