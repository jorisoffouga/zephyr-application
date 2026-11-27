/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LED controller: on, off or blinking, for every LED of the devicetree:
 *
 *   bank      devicetree                          indexes (stm32f4_disco)
 *   BOARD     led0..led3 aliases                  0..3
 *   EXPANDER  children of expander_leds           4..11
 *   BARGRAPH  children of bargraph_leds           12..21
 *
 * Absent banks have no LEDs, the following indexes move down.
 *
 * Callable from threads only (the expander LEDs are on SPI), not from ISRs.
 * The blinking runs from a dedicated work queue.
 */

#ifndef LED_CTRL_H_
#define LED_CTRL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum led_mode {
	LED_MODE_OFF,
	LED_MODE_ON,
	LED_MODE_BLINK,
};

enum led_bank {
	LED_BANK_BOARD,
	LED_BANK_EXPANDER,
	LED_BANK_BARGRAPH,
	LED_BANK_COUNT,
};

/**
 * Configures the GPIOs, all LEDs off, starts the blink work queue.
 *
 * @return 0, or -ENODEV if a board LED is not usable. LEDs of the click
 *         boards that are not usable are skipped (led_ctrl_set returns
 *         -ENODEV for them): a missing click board does not fail the
 *         image self-test.
 */
int led_ctrl_init(void);

/** Number of LEDs, all banks. */
size_t led_ctrl_count(void);

/** First index and number of LEDs of a bank (count 0: no such LED). */
void led_ctrl_bank(enum led_bank bank, size_t *first, size_t *count);

/** Devicetree label of the LED ("User LD4", "exp0", "bar3"), or NULL. */
const char *led_ctrl_name(size_t led);

/**
 * Sets one LED. period_ms is the blink toggle interval, ignored unless
 * mode is LED_MODE_BLINK.
 *
 * @return 0, -EINVAL (index), -ERANGE (period), -ENODEV / -EIO (GPIO)
 */
int led_ctrl_set(size_t led, enum led_mode mode, uint32_t period_ms);

/** Reads one LED state. period_ms is 0 unless blinking. 0 or -EINVAL. */
int led_ctrl_get(size_t led, enum led_mode *mode, uint32_t *period_ms);

const char *led_mode_name(enum led_mode mode);

#endif /* LED_CTRL_H_ */
