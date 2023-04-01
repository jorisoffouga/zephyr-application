#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/uart.h>

#define UART_PORT "UART_2"

#define BME280 DT_INST(0, bosch_bme280)

#if DT_NODE_HAS_STATUS(BME280, okay)
#define BME280_LABEL DT_LABEL(BME280)
#else
#error Your devicetree has no enabled nodes with compatible "bosch,bme280"
#define BME280_LABEL "<none>"
#endif

#define SOF 0x55

static void message_compose(uint8_t *msg, struct sensor_value* value, uint8_t len, uint8_t data_type){
	msg[0] = SOF;
	msg[1] = len;
	msg[2] = data_type;
	msg[3] = (int8_t)value->val1;
	msg[4] = (int8_t)value->val2;
}

static void uart_send(struct device *uart, int8_t* data, uint8_t len){

	for (uint8_t i = 0; i < len; i++)
	{
		uart_poll_out(uart, data[i]);
	}
}

static void send_message(struct device *uart, struct sensor_value *value, uint8_t data_type)
{
	uint8_t data[6];

	message_compose(data, value, 2, data_type);
	uart_send(uart, data, sizeof(data));
}

void main(void)
{
	struct device *bme280 = device_get_binding(BME280_LABEL);
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

	printk("dev %p name %s\n", bme280, bme280->name);

	while (1) {
		sensor_sample_fetch(bme280);
		sensor_channel_get(bme280, SENSOR_CHAN_AMBIENT_TEMP, &bme280_data[0]);
		sensor_channel_get(bme280, SENSOR_CHAN_PRESS, &bme280_data[1]);
		sensor_channel_get(bme280, SENSOR_CHAN_HUMIDITY, &bme280_data[2]);

		for (uint8_t i = 0; i < 3; i++)
		{
			send_message(uart_handle, &bme280_data[i], i);
			k_sleep(Z_TIMEOUT_TICKS(1000));
		}
	}
}
