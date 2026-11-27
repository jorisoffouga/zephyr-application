/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test application for the SWUpdate "mcumgr" handler.
 *
 * - Exposes MCUmgr SMP (image + os groups) on the dedicated UART
 *   transport (uart0). The UART carries only SMP frames: no console,
 *   no shell, no printk.
 * - At boot, if MCUboot started this image in test mode, it runs a
 *   self-test and confirms the image. With CONFIG_APP_SIMULATE_BAD_IMAGE
 *   the self-test fails, the image is never confirmed and MCUboot
 *   reverts it at the next reset.
 * - The LED blink rate identifies the running build; the version is also
 *   reported by SMP (image state "version").
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/dfu/mcuboot.h>

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static int self_test(void)
{
	if (IS_ENABLED(CONFIG_APP_SIMULATE_BAD_IMAGE)) {
		return -EIO;
	}

	if (!gpio_is_ready_dt(&led)) {
		return -ENODEV;
	}

	return 0;
}

int main(void)
{
	/* image started in test mode by MCUboot: confirm it if healthy */
	if (!boot_is_img_confirmed() && self_test() == 0) {
		(void)boot_write_img_confirmed();
	}

	if (!gpio_is_ready_dt(&led) ||
	    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE)) {
		return 0;
	}

	while (1) {
		gpio_pin_toggle_dt(&led);
		k_msleep(CONFIG_APP_BLINK_MS);
	}

	return 0;
}
