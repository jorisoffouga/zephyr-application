#include <zephyr.h>
#include <stdio.h>
#include <device.h>
#include <drivers/gpio.h>
#include <drivers/uart.h>
#include <sys/printk.h>
#include <sys/__assert.h>
#include <string.h>

#define LED0_PORT DT_ALIAS_LED0_GPIOS_CONTROLLER
#define LED0 DT_ALIAS_LED0_GPIOS_PIN

#define LED1 DT_ALIAS_LED1_GPIOS_PIN
#define LED1_PORT DT_ALIAS_LED1_GPIOS_CONTROLLER

#define LED2 DT_ALIAS_LED2_GPIOS_PIN
#define LED2_PORT DT_ALIAS_LED2_GPIOS_CONTROLLER

#define UART_PORT "UART_4"
#define SW_PORT DT_ALIAS_SW0_GPIOS_CONTROLLER
#define SW_PIN DT_ALIAS_SW0_GPIOS_PIN
#define EDGE (GPIO_INT_EDGE | GPIO_INT_ACTIVE_LOW)

/* size of stack area used by each thread */
#define STACKSIZE 1024

/* scheduling priority used by each thread */
#define PRIORITY 7

struct fifo_data_t
{
	void *fifo_reserved;
	u8_t *data;
};

struct bridge_t
{
	struct
	{
		struct device *handle;
	} cdc;

	struct
	{
		struct device *handle;
	} uart;
};

K_FIFO_DEFINE(uart_fifo);
K_FIFO_DEFINE(cdc_fifo);

static struct bridge_t bridge;

static void gpio_pin_toggle(struct device *port, u32_t pin)
{
	u32_t value = 0;

	u8_t ret = gpio_pin_read(port, pin, &value);

	if (ret < 0)
	{
		printk("Error gpio read\n");
		return;
	}

	if (value)
	{
		gpio_pin_write(port, pin, 0);
	}
	else
	{
		gpio_pin_write(port, pin, 1);
	}
}

static void cdc_irq_callback(struct device *cdc)
{
	u8_t byte = 0;

	while (uart_irq_update(cdc) && uart_irq_is_pending(cdc))
	{
		if (!uart_irq_rx_ready(cdc))
		{
			/* Only the UART RX path is interrupt-enabled */
			break;
		}

		uart_fifo_read(cdc, &byte, sizeof(byte));
		struct fifo_data_t data = {.data = &byte};
		size_t size = sizeof(struct fifo_data_t);
		char *mem_ptr = k_malloc(size);

		if(mem_ptr != 0){
			return;
		}

		memcpy(mem_ptr, &data, size);

		k_fifo_put(&cdc_fifo, mem_ptr);
	}
}

static void uart_irq_callback(struct device *uart)
{
	u8_t byte;
	while (uart_irq_update(uart) && uart_irq_is_pending(uart)){

		if (!uart_irq_rx_ready(uart))
		{
			/* Only the UART RX path is interrupt-enabled */
			break;
		}

		uart_fifo_read(uart, &byte, sizeof(byte));
		uart_poll_out(uart, byte);
	}
}

K_THREAD_STACK_DEFINE(cdc_stack_size, STACKSIZE);
K_THREAD_STACK_DEFINE(uart_stack_size, STACKSIZE);

static void uartThread(void *dummy1, void *dummy2, void *dummy3)
{
	ARG_UNUSED(dummy1);
	ARG_UNUSED(dummy2);
	ARG_UNUSED(dummy3);

	struct bridge_t *dev = &bridge;
	struct device *dev0;
	struct device *dev1;
	struct device *dev2;

	/* uart setup */
	dev->uart.handle = device_get_binding(UART_PORT);
	__ASSERT_NO_MSG(dev->uart.handle != NULL);

	uart_irq_rx_disable(dev->uart.handle);
	uart_irq_tx_disable(dev->uart.handle);
	uart_irq_callback_set(dev->uart.handle, uart_irq_callback);


	dev0 = device_get_binding(LED0_PORT);
	__ASSERT_NO_MSG(dev0 != NULL);
	dev1 = device_get_binding(LED1_PORT);
	__ASSERT_NO_MSG(dev1 != NULL);
	dev2 = device_get_binding(LED2_PORT);
	__ASSERT_NO_MSG(dev2 != NULL);
	/* Set LED pin as output */
	gpio_pin_configure(dev0, LED0, GPIO_DIR_OUT);
	gpio_pin_configure(dev1, LED1, GPIO_DIR_OUT);
	gpio_pin_configure(dev1, LED2, GPIO_DIR_OUT);


	printf("Thread Uart Ready\n");

	while (1)
	{
		struct fifo_data_t *data = k_fifo_get(&uart_fifo, K_FOREVER);
		switch (*data->data)
		{
		case 0x30:
			gpio_pin_toggle(dev0, LED0);
			break;
		case 0x31:
			gpio_pin_toggle(dev1, LED1);
			break;
		case 0x32:
			gpio_pin_toggle(dev2, LED2);
			break;
		default:
			break;
		}
		k_free(data);
		k_sleep(500);
	}
}

static void cdcThread(void *dummy1, void *dummy2, void *dummy3)
{
	ARG_UNUSED(dummy1);
	ARG_UNUSED(dummy2);
	ARG_UNUSED(dummy3);

	struct bridge_t *dev = &bridge;
	u32_t baudrate, dtr = 0U;
	int ret;

	dev->cdc.handle = device_get_binding("CDC_ACM_0");
	if (!dev->cdc.handle)
	{
		printf("CDC ACM device not found\n");
		return;
	}

	printf("Wait for DTR\n");
	while (1)
	{
		uart_line_ctrl_get(dev->cdc.handle, LINE_CTRL_DTR, &dtr);
		if (dtr)
			break;
	}
	printf("DTR set, start test\n");

	/* They are optional, we use them to test the interrupt endpoint */
	ret = uart_line_ctrl_set(dev->cdc.handle, LINE_CTRL_DCD, 1);
	if (ret)
		printf("Failed to set DCD, ret code %d\n", ret);

	ret = uart_line_ctrl_set(dev->cdc.handle, LINE_CTRL_DSR, 1);
	if (ret)
		printf("Failed to set DSR, ret code %d\n", ret);

	/* Wait 1 sec for the host to do all settings */
	k_busy_wait(1000000);

	ret = uart_line_ctrl_get(dev->cdc.handle, LINE_CTRL_BAUD_RATE, &baudrate);
	if (ret)
		printf("Failed to get baudrate, ret code %d\n", ret);
	else
		printf("Baudrate detected: %d\n", baudrate);

	uart_irq_callback_set(dev->cdc.handle, cdc_irq_callback);

	/* Enable rx interrupts */
	uart_irq_rx_enable(dev->cdc.handle);

	printf("Thread Cdc Ready\n");

	while (1)
	{
		struct fifo_data_t *data = k_fifo_get(&cdc_fifo, K_FOREVER);
		uart_poll_out(dev->cdc.handle, *data->data);
		k_sleep(500);
	}
}

K_THREAD_DEFINE(uartThread_id, STACKSIZE, uartThread, NULL, NULL, NULL,
PRIORITY, 0, K_NO_WAIT);

K_THREAD_DEFINE(cdcThread_id, STACKSIZE, cdcThread, NULL, NULL, NULL,
PRIORITY, 0, K_NO_WAIT);
