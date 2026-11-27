/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "led_ctrl.h"
#include "led_mgmt.h"

LOG_MODULE_REGISTER(led_ctrl, CONFIG_APP_LOG_LEVEL);

struct led {
	struct gpio_dt_spec gpio;
	const char *name;
	enum led_bank bank;
	struct k_work_delayable blink;
	enum led_mode mode;
	uint32_t period_ms;
	bool lit;
	bool ready;
};

#define LED_ENTRY(node, _bank)						\
	{								\
		.gpio = GPIO_DT_SPEC_GET(node, gpios),			\
		.name = DT_PROP_OR(node, label, DT_NODE_FULL_NAME(node)), \
		.bank = _bank,						\
	},

#define BOARD_LED(n)							\
	COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(led##n)),			\
		    (LED_ENTRY(DT_ALIAS(led##n), LED_BANK_BOARD)), ())

#define BANK_LEDS(nodelabel, _bank)					\
	COND_CODE_1(DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(nodelabel)),	\
		    (DT_FOREACH_CHILD_STATUS_OKAY_VARGS(DT_NODELABEL(nodelabel), \
							LED_ENTRY, _bank)), \
		    ())

/* in bank order: led_ctrl_bank() relies on it */
static struct led leds[] = {
	BOARD_LED(0) BOARD_LED(1) BOARD_LED(2) BOARD_LED(3)
	BANK_LEDS(expander_leds, LED_BANK_EXPANDER)
	BANK_LEDS(bargraph_leds, LED_BANK_BARGRAPH)
};

BUILD_ASSERT(ARRAY_SIZE(leds) > 0, "board has no led0 alias");

/* held across GPIO calls, which may be SPI transfers: a mutex, not a spinlock */
static K_MUTEX_DEFINE(lock);
static bool initialized;

static K_THREAD_STACK_DEFINE(led_wq_stack, CONFIG_APP_LED_WQ_STACK_SIZE);
static struct k_work_q led_wq;

static void led_blink(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct led *led = CONTAINER_OF(dwork, struct led, blink);

	k_mutex_lock(&lock, K_FOREVER);
	/* led_ctrl_set may have changed the mode while this item waited */
	if (led->mode == LED_MODE_BLINK) {
		led->lit = !led->lit;
		(void)gpio_pin_set_dt(&led->gpio, led->lit);
		k_work_reschedule_for_queue(&led_wq, dwork, K_MSEC(led->period_ms));
	}
	k_mutex_unlock(&lock);
}

int led_ctrl_init(void)
{
	static const char *const bank_names[] = { "board", "expander", "bargraph" };
	size_t total[ARRAY_SIZE(bank_names)] = { 0 };
	size_t ready[ARRAY_SIZE(bank_names)] = { 0 };
	int rc = 0;

	k_work_queue_start(&led_wq, led_wq_stack, K_THREAD_STACK_SIZEOF(led_wq_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
	k_thread_name_set(&led_wq.thread, "led_wq");

	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		struct led *led = &leds[i];

		k_work_init_delayable(&led->blink, led_blink);
		led->mode = LED_MODE_OFF;
		led->period_ms = 0;
		led->lit = false;
		led->ready = gpio_is_ready_dt(&led->gpio) &&
			     gpio_pin_configure_dt(&led->gpio, GPIO_OUTPUT_INACTIVE) == 0;

		total[led->bank]++;
		if (led->ready) {
			ready[led->bank]++;
		} else if (led->bank == LED_BANK_BOARD) {
			/* only the board LEDs are required (image self-test) */
			rc = -ENODEV;
		}
	}

	for (size_t b = 0; b < ARRAY_SIZE(bank_names); b++) {
		if (total[b] == 0) {
			continue;
		}
		if (ready[b] == total[b]) {
			LOG_INF("%s LEDs: %u ready", bank_names[b], (unsigned int)total[b]);
		} else {
			LOG_WRN("%s LEDs: %u of %u ready (device missing?)",
				bank_names[b], (unsigned int)ready[b],
				(unsigned int)total[b]);
		}
	}

	initialized = true;
	return rc;
}

size_t led_ctrl_count(void)
{
	return ARRAY_SIZE(leds);
}

void led_ctrl_bank(enum led_bank bank, size_t *first, size_t *count)
{
	*first = 0;
	*count = 0;

	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		if (leds[i].bank != bank) {
			continue;
		}
		if (*count == 0) {
			*first = i;
		}
		(*count)++;
	}
}

const char *led_ctrl_name(size_t idx)
{
	return idx < ARRAY_SIZE(leds) ? leds[idx].name : NULL;
}

static int led_set(size_t idx, enum led_mode mode, uint32_t period_ms, bool log)
{
	struct led *led;
	bool lit;
	int rc;

	if (idx >= ARRAY_SIZE(leds)) {
		return -EINVAL;
	}

	led = &leds[idx];
	if (!initialized || !led->ready) {
		return -ENODEV;
	}

	switch (mode) {
	case LED_MODE_OFF:
		lit = false;
		period_ms = 0;
		break;
	case LED_MODE_ON:
		lit = true;
		period_ms = 0;
		break;
	case LED_MODE_BLINK:
		if (period_ms < LED_PERIOD_MIN_MS || period_ms > LED_PERIOD_MAX_MS) {
			return -ERANGE;
		}
		lit = true;
		break;
	default:
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	(void)k_work_cancel_delayable(&led->blink);

	rc = gpio_pin_set_dt(&led->gpio, lit);
	if (rc == 0) {
		led->mode = mode;
		led->period_ms = period_ms;
		led->lit = lit;
		if (mode == LED_MODE_BLINK) {
			k_work_reschedule_for_queue(&led_wq, &led->blink, K_MSEC(period_ms));
		}
	} else {
		rc = -EIO;
	}

	k_mutex_unlock(&lock);

	if (rc != 0) {
		LOG_ERR("led %u (%s): GPIO write failed", (unsigned int)idx, led->name);
	} else if (log) {
		LOG_INF("led %u (%s): %s %u ms", (unsigned int)idx, led->name,
			led_mode_name(mode), period_ms);
	}
	return rc;
}

int led_ctrl_set(size_t idx, enum led_mode mode, uint32_t period_ms)
{
	return led_set(idx, mode, period_ms, true);
}

int led_ctrl_set_quiet(size_t idx, enum led_mode mode, uint32_t period_ms)
{
	return led_set(idx, mode, period_ms, false);
}

int led_ctrl_led_bank(size_t idx, enum led_bank *bank)
{
	if (idx >= ARRAY_SIZE(leds)) {
		return -EINVAL;
	}
	*bank = leds[idx].bank;
	return 0;
}

struct k_work_q *led_ctrl_workq(void)
{
	return &led_wq;
}

int led_ctrl_get(size_t idx, enum led_mode *mode, uint32_t *period_ms)
{
	if (idx >= ARRAY_SIZE(leds)) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);
	*mode = leds[idx].mode;
	*period_ms = leds[idx].period_ms;
	k_mutex_unlock(&lock);

	return 0;
}

const char *led_mode_name(enum led_mode mode)
{
	switch (mode) {
	case LED_MODE_ON:
		return "on";
	case LED_MODE_BLINK:
		return "blink";
	default:
		return "off";
	}
}
