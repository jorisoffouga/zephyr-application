/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LED patterns, see include/pattern.h.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "pattern.h"

LOG_MODULE_REGISTER(pattern, CONFIG_APP_LOG_LEVEL);

static const char *const pattern_names[PATTERN_COUNT] = {
	[PATTERN_OFF] = "off",
	[PATTERN_K2000] = "k2000",
	[PATTERN_CHASE] = "chase",
	[PATTERN_FILL] = "fill",
	[PATTERN_ALTERNATE] = "alternate",
	[PATTERN_COUNTER] = "counter",
};

static const char *const bank_names[LED_BANK_COUNT] = {
	[LED_BANK_BOARD] = "board",
	[LED_BANK_EXPANDER] = "expander",
	[LED_BANK_BARGRAPH] = "bargraph",
};

struct bank_run {
	struct k_work_delayable work;
	enum pattern_id id;
	uint32_t step_ms;
	uint32_t width;
	uint32_t step;
	uint32_t shown;		/* mask on the LEDs */
	bool refresh;		/* write every LED at the next step */
};

static struct bank_run runs[LED_BANK_COUNT];
static K_MUTEX_DEFINE(lock);

/* LEDs lit at a step, bit i = LED i of the bank (n LEDs) */
static uint32_t pattern_mask(enum pattern_id id, uint32_t n, uint32_t step, uint32_t width)
{
	uint32_t mask = 0;

	if (n == 0) {
		return 0;
	}

	switch (id) {
	case PATTERN_K2000: {
		/* 0 .. n-1 .. 1, tail behind the head */
		uint32_t len = (n > 1) ? 2 * (n - 1) : 1;
		uint32_t p = step % len;
		int head = (p < n) ? (int)p : (int)(len - p);
		int dir = (p < n - 1) ? 1 : -1;

		for (uint32_t k = 0; k < width; k++) {
			int i = head - dir * (int)k;

			if (i >= 0 && i < (int)n) {
				mask |= BIT(i);
			}
		}
		break;
	}
	case PATTERN_CHASE: {
		uint32_t head = step % n;

		for (uint32_t k = 0; k < width && k < n; k++) {
			mask |= BIT((head + n - k) % n);
		}
		break;
	}
	case PATTERN_FILL: {
		/* level 0 .. n .. 1 */
		uint32_t p = step % (2 * n);
		uint32_t level = (p <= n) ? p : 2 * n - p;

		mask = BIT_MASK(level);
		break;
	}
	case PATTERN_ALTERNATE:
		mask = ((step & 1) ? 0xAAAAAAAAU : 0x55555555U) & BIT_MASK(n);
		break;
	case PATTERN_COUNTER:
		mask = step & BIT_MASK(n);
		break;
	default:
		break;
	}

	return mask;
}

static void pattern_step(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct bank_run *run = CONTAINER_OF(dwork, struct bank_run, work);
	enum led_bank bank = (enum led_bank)(run - runs);
	size_t first, n;
	uint32_t mask, changed;

	k_mutex_lock(&lock, K_FOREVER);

	if (run->id == PATTERN_OFF) {
		k_mutex_unlock(&lock);
		return;
	}

	led_ctrl_bank(bank, &first, &n);
	mask = pattern_mask(run->id, n, run->step, run->width);
	changed = run->refresh ? BIT_MASK(n) : (mask ^ run->shown);

	/* only the LEDs that change: one SPI transfer each on the click boards */
	for (size_t i = 0; i < n; i++) {
		if (changed & BIT(i)) {
			(void)led_ctrl_set_quiet(first + i,
						 (mask & BIT(i)) ? LED_MODE_ON : LED_MODE_OFF, 0);
		}
	}

	run->shown = mask;
	run->refresh = false;
	run->step++;
	k_work_reschedule_for_queue(led_ctrl_workq(), dwork, K_MSEC(run->step_ms));

	k_mutex_unlock(&lock);
}

static int pattern_init_once(void)
{
	static bool done;

	if (!done) {
		for (size_t b = 0; b < LED_BANK_COUNT; b++) {
			k_work_init_delayable(&runs[b].work, pattern_step);
		}
		done = true;
	}
	return 0;
}

int pattern_start(enum led_bank bank, enum pattern_id id, uint32_t step_ms, uint32_t width)
{
	struct bank_run *run;
	size_t first, n;

	if (bank >= LED_BANK_COUNT) {
		return -EINVAL;
	}
	if (id >= PATTERN_COUNT) {
		return -EINVAL;
	}

	led_ctrl_bank(bank, &first, &n);
	if (n == 0) {
		return -ENODEV;
	}

	if (id != PATTERN_OFF) {
		if (step_ms < PATTERN_STEP_MIN_MS || step_ms > PATTERN_STEP_MAX_MS ||
		    width < 1 || width > PATTERN_WIDTH_MAX) {
			return -ERANGE;
		}
	}

	k_mutex_lock(&lock, K_FOREVER);
	pattern_init_once();
	run = &runs[bank];

	(void)k_work_cancel_delayable(&run->work);
	run->id = id;
	run->step_ms = step_ms;
	run->width = width;
	run->step = 0;
	run->shown = 0;
	run->refresh = true;

	if (id == PATTERN_OFF) {
		for (size_t i = 0; i < n; i++) {
			(void)led_ctrl_set_quiet(first + i, LED_MODE_OFF, 0);
		}
	} else {
		k_work_reschedule_for_queue(led_ctrl_workq(), &run->work, K_NO_WAIT);
	}

	k_mutex_unlock(&lock);

	if (id == PATTERN_OFF) {
		LOG_INF("%s: pattern off", bank_names[bank]);
	} else {
		LOG_INF("%s: pattern %s, step %u ms, width %u", bank_names[bank],
			pattern_names[id], step_ms, width);
	}
	return 0;
}

void pattern_stop(enum led_bank bank)
{
	bool was_running;

	if (bank >= LED_BANK_COUNT) {
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);
	pattern_init_once();
	was_running = runs[bank].id != PATTERN_OFF;
	runs[bank].id = PATTERN_OFF;
	(void)k_work_cancel_delayable(&runs[bank].work);
	k_mutex_unlock(&lock);

	if (was_running) {
		LOG_INF("%s: pattern stopped (manual change)", bank_names[bank]);
	}
}

void pattern_stop_led(size_t led)
{
	enum led_bank bank;

	if (led_ctrl_led_bank(led, &bank) == 0) {
		pattern_stop(bank);
	}
}

void pattern_get(enum led_bank bank, struct pattern_state *st)
{
	*st = (struct pattern_state){ 0 };
	if (bank >= LED_BANK_COUNT) {
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);
	st->id = runs[bank].id;
	st->step_ms = runs[bank].step_ms;
	st->width = runs[bank].width;
	k_mutex_unlock(&lock);
}

const char *pattern_name(enum pattern_id id)
{
	return (id < PATTERN_COUNT) ? pattern_names[id] : "?";
}

static int index_of(const char *const *names, size_t count, const char *name, size_t len)
{
	for (size_t i = 0; i < count; i++) {
		if (strlen(names[i]) == len && memcmp(names[i], name, len) == 0) {
			return (int)i;
		}
	}
	return -1;
}

int pattern_from_name(const char *name, size_t len, enum pattern_id *id)
{
	int i = index_of(pattern_names, PATTERN_COUNT, name, len);

	if (i < 0) {
		return -EINVAL;
	}
	*id = (enum pattern_id)i;
	return 0;
}

const char *led_bank_name(enum led_bank bank)
{
	return (bank < LED_BANK_COUNT) ? bank_names[bank] : "?";
}

int led_bank_from_name(const char *name, size_t len, enum led_bank *bank)
{
	int i = index_of(bank_names, LED_BANK_COUNT, name, len);

	if (i < 0) {
		return -EINVAL;
	}
	*bank = (enum led_bank)i;
	return 0;
}
