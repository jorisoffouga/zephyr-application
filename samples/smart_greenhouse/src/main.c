#include <zephyr.h>
#include <device.h>
#include <drivers/sensor.h>
#include <drivers/uart.h>

#include "message.h"

#define UART_PORT DT_LABEL(DT_NODELABEL(usart2))
#define BME280	"BME280"
#define SOF 0x55
#define SIZE(a) sizeof (a) / sizeof *(a)

static void uart_send(struct device *uart, int8_t* data, uint8_t len){

	for (uint8_t i = 0; i < len; i++)
	{
		uart_poll_out(uart, data[i]);
	}
}

static void send_message(struct device *uart, struct sensor_value *value, uint8_t id)
{
	uint8_t *rawMsg;
	size_t len;
	//message_t *msg = NULL;
	message_t msg;

	memset(&msg, 0, sizeof(struct sensor_value) + sizeof(message_t));
	uint16_t rawValue = (value->val1 * 10^(6)) + (uint16_t)(value->val2);

	//msg.dataPtr = &msg.data.datagen;

	//msg = message_alloc(sizeof(struct sensor_value));

	// if (msg =! NULL)
	// {
	// 	message_init(msg);
	// }else{
	// 	printk("Memory Alloc fail");
	// 	return;
	// }

	msg.cmd = CMD_EVENT;
	msg.id = (id_t) (id);
	msg.status = MSG_STATE_SUCCESS;

	switch (msg.id)
	{
	case ID_TEMP:
		msg.data.temp.degrees = rawValue;
		msg.dataLen = sizeof(msg.data.temp);
		printk("Temperature reel %d, Temperature  %d.%d\n ", rawValue, value->val1, value->val2);
		break;
	case ID_HUMIDITY:
		msg.data.humidity.percents = rawValue;
		msg.dataLen = sizeof(msg.data.humidity);
		printk("Humidity reel %d, Humidity  %d.%d\n ", rawValue, value->val1, value->val2);
		break;
	case ID_PRESSURE:
		msg.data.pressure.bar = rawValue;
		msg.dataLen = sizeof(msg.data.pressure);
		printk("Pressure reel %d, Pressure  %d.%d\n ", rawValue, value->val1, value->val2);
		break;
	default:
		break;
	}

	// len = message_compose(&rawMsg, &msg);

	// if (len != 0 )
	// {
	// 	k_free(rawMsg);
	// 	//message_free(msg);
	// 	//uart_send(uart, rawMsg, len);
	// }
}

void main(void)
{
	struct device *bme280 = device_get_binding(BME280);
	struct device *uart_handle = device_get_binding(UART_PORT);
	struct sensor_value bme280_data[3];

	//printk("")
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

		for (uint8_t i = 0; i < 3; i++)
		{
			send_message(uart_handle, &bme280_data[i], i);
			k_sleep(K_MSEC(1000));
		}
	}
}
