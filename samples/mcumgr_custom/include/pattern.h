/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LED patterns, run by the firmware on one bank of LEDs (led_ctrl.h):
 *
 *   k2000      scanner going back and forth, "width" LEDs (head + tail)
 *   chase      running light in one direction, "width" LEDs
 *   fill       bar filling up then emptying
 *   alternate  even / odd LEDs in turn
 *   counter    binary counter
 *
 * One step every step_ms, on the LED work queue: the pattern keeps
 * running without any host. Each bank has its own pattern. A manual
 * change of an LED of the bank (led / bargraph groups) stops the pattern
 * of that bank.
 */

#ifndef PATTERN_H_
#define PATTERN_H_

#include <stdbool.h>
#include <stdint.h>

#include "led_ctrl.h"

enum pattern_id {
	PATTERN_OFF,
	PATTERN_K2000,
	PATTERN_CHASE,
	PATTERN_FILL,
	PATTERN_ALTERNATE,
	PATTERN_COUNTER,
	PATTERN_COUNT,
};

#define PATTERN_STEP_MIN_MS	20
#define PATTERN_STEP_MAX_MS	5000
#define PATTERN_WIDTH_MAX	4

struct pattern_state {
	enum pattern_id id;
	uint32_t step_ms;
	uint32_t width;
};

/**
 * Starts (or replaces) the pattern of a bank. PATTERN_OFF stops it and
 * switches the LEDs of the bank off.
 *
 * @return 0, -ENODEV (no LED in the bank), -EINVAL (pattern),
 *         -ERANGE (step_ms or width)
 */
int pattern_start(enum led_bank bank, enum pattern_id id, uint32_t step_ms, uint32_t width);

/** Stops the pattern of a bank, the LEDs keep their current state. */
void pattern_stop(enum led_bank bank);

/** Stops the pattern of the bank of this LED (manual change of the LED). */
void pattern_stop_led(size_t led);

void pattern_get(enum led_bank bank, struct pattern_state *st);

const char *pattern_name(enum pattern_id id);
int pattern_from_name(const char *name, size_t len, enum pattern_id *id);

const char *led_bank_name(enum led_bank bank);
int led_bank_from_name(const char *name, size_t len, enum led_bank *bank);

#endif /* PATTERN_H_ */
