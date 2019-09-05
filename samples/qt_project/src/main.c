#include <zephyr.h>
#include <device.h>
#include <gpio.h>
#include <uart.h>
#include <misc/printk.h>
#include <misc/__assert.h>
#include <string.h>
#include "util.h"

#define LED_PORT DT_ALIAS_LED0_GPIOS_CONTROLLER
#define LED0 DT_ALIAS_LED0_GPIOS_PIN
#define LED1 DT_ALIAS_LED1_GPIOS_PIN
#define LED2 DT_ALIAS_LED2_GPIOS_PIN
#define LED3 DT_ALIAS_LED3_GPIOS_PIN
#define UART_PORT "UART_2"
#define SW_PORT DT_ALIAS_SW0_GPIOS_CONTROLLER
#define SW_PIN DT_ALIAS_SW0_GPIOS_PIN
#define EDGE (GPIO_INT_EDGE | GPIO_INT_ACTIVE_LOW)

#define PULL_UP 0

/* size of stack area used by each thread */
#define STACKSIZE 1024

/* scheduling priority used by each thread */
#define PRIORITY 7

/* Callback function*/
typedef void (*callback_t)(struct device *port,
				   struct gpio_callback *cb, u32_t pins);

static struct bridge_t bridge;

static struct gpio_callback gpio_cb;

/* Initialiase Fifo*/
K_FIFO_DEFINE(uart_fifo);

/* Uart rx callback*/
static void uart_irq_callback(struct device *uart)
{
	int rx;
	char buffer[255];

	while (uart_irq_update(uart) &&
		   uart_irq_is_pending(uart))
	{

		if (!uart_irq_rx_ready(uart))
		{
			if (uart_irq_tx_ready(uart))
			{
				/* Nothing to do*/
			}
			else
			{
				/* Nothing to do*/
			}
			/* Only the UART RX path is interrupt-enabled */
			break;
		}

		/* Read character and send to mainthead with fifo */
		rx = uart_fifo_read(uart, buffer, 1);
		struct fifo_data_t data = {.data = buffer};
		size_t size = sizeof(struct fifo_data_t);
		char *mem_ptr = k_malloc(size);
		__ASSERT_NO_MSG(mem_ptr != 0);

		memcpy(mem_ptr, &data, size);

		k_fifo_put(&uart_fifo, mem_ptr);
	}
}

/* Gpio interrupt callback */
void gpio_callback(struct device *port,
				   struct gpio_callback *cb, u32_t pins)
{
	struct bridge_t *dev = &bridge;

	gpio_pin_toggle(dev->gpio.handle, LED0);
	gpio_pin_toggle(dev->gpio.handle, LED1);
	gpio_pin_toggle(dev->gpio.handle, LED2);
	gpio_pin_toggle(dev->gpio.handle, LED3);
}

/* Thread */
static void MainThread(void)
{

	struct bridge_t *dev = &bridge;
	struct device *sw_dev;
	callback_t gpioCallback = gpio_callback;

	/* Enable gpio interrupt */
	sw_dev = device_get_binding(SW_PORT);
	__ASSERT_NO_MSG(sw_dev != NULL);

	gpio_pin_configure(sw_dev, SW_PIN, GPIO_DIR_IN | GPIO_INT | PULL_UP | EDGE);
	gpio_init_callback(&gpio_cb, gpioCallback, BIT(SW_PIN));
	gpio_add_callback(sw_dev, &gpio_cb);
	gpio_pin_enable_callback(sw_dev, SW_PIN);

	/* Set LED pin as output */
	dev->gpio.handle = device_get_binding(LED_PORT);
	__ASSERT_NO_MSG(dev->gpio.handle != NULL);

	gpio_pin_configure(dev->gpio.handle, LED0, GPIO_DIR_OUT);
	gpio_pin_configure(dev->gpio.handle, LED1, GPIO_DIR_OUT);
	gpio_pin_configure(dev->gpio.handle, LED2, GPIO_DIR_OUT);
	gpio_pin_configure(dev->gpio.handle, LED3, GPIO_DIR_OUT);

	/* uart setup */
	dev->uart.handle = device_get_binding(UART_PORT);
	__ASSERT_NO_MSG(dev->gpio.handle != NULL);

	uart_irq_rx_disable(dev->uart.handle);
	uart_irq_tx_disable(dev->uart.handle);
	uart_irq_callback_set(dev->uart.handle, uart_irq_callback);
	uart_irq_rx_enable(dev->uart.handle);

	while (1)
	{
		struct fifo_data_t *data = k_fifo_get(&uart_fifo, K_FOREVER);
		switch (*data->data)
		{
		case 0x30:
			gpio_pin_toggle(dev->gpio.handle, LED3);
			break;
		case 0x31:
			gpio_pin_toggle(dev->gpio.handle, LED0);
			break;
		case 0x32:
			gpio_pin_toggle(dev->gpio.handle, LED1);
			break;
		case 0x33:
			gpio_pin_toggle(dev->gpio.handle, LED2);
			break;
		default:
			break;
		}
		k_free(data);
		k_sleep(500);
	}
}

/* Initialiase Thread */
K_THREAD_DEFINE(MainThread_id, STACKSIZE, MainThread, NULL, NULL, NULL, PRIORITY, 0, K_NO_WAIT);