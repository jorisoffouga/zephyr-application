/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LED settings in the EEPROM click. Record layout in include/app_store.h.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include "app_store.h"
#include "led_ctrl.h"
#include "pattern.h"

LOG_MODULE_REGISTER(app_store, CONFIG_APP_LOG_LEVEL);

#define STORE_MAGIC	0x7473434dU	/* "MCst" */
#define STORE_VERSION	1
#define HDR_SIZE	12
#define LED_REC_SIZE	3
#define BANK_REC_SIZE	4
/* board 4 + expander 8 + bargraph 10 LEDs fit in a few pages */
#define MAX_LEDS	32
#define RECORD_MAX	(HDR_SIZE + MAX_LEDS * LED_REC_SIZE + LED_BANK_COUNT * BANK_REC_SIZE)

BUILD_ASSERT(RECORD_MAX <= CONFIG_APP_STORE_SIZE, "APP_STORE_SIZE too small");

static const struct device *const eeprom = DEVICE_DT_GET(DT_NODELABEL(eeprom));

static K_MUTEX_DEFINE(lock);
static uint8_t buf[RECORD_MAX];
static uint8_t old[RECORD_MAX];
static bool autosave = IS_ENABLED(CONFIG_APP_STORE_AUTOSAVE);
static bool valid;
static bool restored;
static uint32_t saves;
static int last_rc;

static void save_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(save_work, save_work_fn);

/* the record of the current state, in buf; returns its length */
static size_t build(void)
{
	size_t n = MIN(led_ctrl_count(), MAX_LEDS);
	size_t pos = HDR_SIZE;
	uint16_t len;

	sys_put_le32(STORE_MAGIC, &buf[0]);
	buf[4] = STORE_VERSION;
	for (int b = 0; b < LED_BANK_COUNT; b++) {
		size_t first, count;

		led_ctrl_bank(b, &first, &count);
		buf[5 + b] = count;
	}

	for (size_t i = 0; i < n; i++) {
		enum led_mode mode;
		uint32_t period;

		(void)led_ctrl_get(i, &mode, &period);
		buf[pos] = mode;
		sys_put_le16(MIN(period, UINT16_MAX), &buf[pos + 1]);
		pos += LED_REC_SIZE;
	}

	for (int b = 0; b < LED_BANK_COUNT; b++) {
		struct pattern_state st;

		pattern_get(b, &st);
		buf[pos] = st.id;
		sys_put_le16(MIN(st.step_ms, UINT16_MAX), &buf[pos + 1]);
		buf[pos + 3] = st.width;
		pos += BANK_REC_SIZE;
	}

	len = pos - HDR_SIZE;
	sys_put_le16(len, &buf[8]);
	sys_put_le16(crc16_ccitt(0, &buf[HDR_SIZE], len), &buf[10]);
	return pos;
}

/* reads and checks the stored record into old; its length, or -errno */
static int load(void)
{
	uint16_t len;
	int rc;

	rc = eeprom_read(eeprom, CONFIG_APP_STORE_OFFSET, old, HDR_SIZE);
	if (rc != 0) {
		return rc;
	}
	len = sys_get_le16(&old[8]);
	if (sys_get_le32(&old[0]) != STORE_MAGIC || old[4] != STORE_VERSION ||
	    len > sizeof(old) - HDR_SIZE) {
		return -ENOENT;
	}
	rc = eeprom_read(eeprom, CONFIG_APP_STORE_OFFSET + HDR_SIZE, &old[HDR_SIZE], len);
	if (rc != 0) {
		return rc;
	}
	if (crc16_ccitt(0, &old[HDR_SIZE], len) != sys_get_le16(&old[10])) {
		return -EBADMSG;
	}
	return HDR_SIZE + len;
}

int app_store_restore(void)
{
	size_t n = led_ctrl_count();
	size_t pos = HDR_SIZE;
	int rc;

	if (!device_is_ready(eeprom)) {
		LOG_WRN("store: EEPROM not ready, settings not restored");
		return -ENODEV;
	}

	k_mutex_lock(&lock, K_FOREVER);
	rc = load();
	valid = rc > 0;
	last_rc = rc < 0 && rc != -ENOENT ? rc : 0;
	if (rc < 0) {
		k_mutex_unlock(&lock);
		if (rc == -ENOENT) {
			LOG_INF("store: no settings in the EEPROM yet");
		} else {
			LOG_WRN("store: settings not readable (%d), not restored", rc);
		}
		return rc;
	}

	for (int b = 0; b < LED_BANK_COUNT; b++) {
		size_t first, count;

		led_ctrl_bank(b, &first, &count);
		if (old[5 + b] != count) {
			k_mutex_unlock(&lock);
			LOG_WRN("store: saved for another LED layout (other click "
				"boards), not restored");
			return -ESTALE;
		}
	}
	if ((size_t)rc < HDR_SIZE + MIN(n, MAX_LEDS) * LED_REC_SIZE +
			 LED_BANK_COUNT * BANK_REC_SIZE) {
		k_mutex_unlock(&lock);
		return -EBADMSG;
	}

	/* LED 0 shows the image identity: not restored */
	for (size_t i = 0; i < MIN(n, MAX_LEDS); i++, pos += LED_REC_SIZE) {
		enum led_mode mode = old[pos];
		uint32_t period = sys_get_le16(&old[pos + 1]);

		if (i > 0 && mode != LED_MODE_OFF) {
			(void)led_ctrl_set_quiet(i, mode, period);
		}
	}
	for (int b = 0; b < LED_BANK_COUNT; b++, pos += BANK_REC_SIZE) {
		enum pattern_id id = old[pos];

		if (id != PATTERN_OFF) {
			(void)pattern_start(b, id, sys_get_le16(&old[pos + 1]), old[pos + 3]);
		}
	}
	restored = true;
	k_mutex_unlock(&lock);

	LOG_INF("store: settings restored from the EEPROM");
	return 0;
}

static int save_locked(void)
{
	size_t len = build();
	int old_len = load();
	int rc;

	if (old_len == (int)len && memcmp(old, buf, len) == 0) {
		valid = true;
		return 0;	/* unchanged: no EEPROM write */
	}

	rc = eeprom_write(eeprom, CONFIG_APP_STORE_OFFSET, buf, len);
	last_rc = rc;
	if (rc == 0) {
		valid = true;
		saves++;
		LOG_INF("store: settings saved (%u bytes)", (unsigned int)len);
	} else {
		LOG_ERR("store: EEPROM write failed (%d): write protect (WP)?", rc);
	}
	return rc;
}

int app_store_save(void)
{
	int rc;

	if (!device_is_ready(eeprom)) {
		return -ENODEV;
	}
	(void)k_work_cancel_delayable(&save_work);
	k_mutex_lock(&lock, K_FOREVER);
	rc = save_locked();
	k_mutex_unlock(&lock);
	return rc;
}

static void save_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)app_store_save();
}

void app_store_changed(void)
{
	if (autosave && device_is_ready(eeprom)) {
		k_work_reschedule(&save_work, K_MSEC(CONFIG_APP_STORE_DELAY_MS));
	}
}

int app_store_clear(void)
{
	static const uint8_t blank[HDR_SIZE] = { 0xff, 0xff, 0xff, 0xff };
	int rc;

	if (!device_is_ready(eeprom)) {
		return -ENODEV;
	}
	(void)k_work_cancel_delayable(&save_work);
	k_mutex_lock(&lock, K_FOREVER);
	rc = eeprom_write(eeprom, CONFIG_APP_STORE_OFFSET, blank, sizeof(blank));
	last_rc = rc;
	if (rc == 0) {
		valid = false;
		LOG_INF("store: settings cleared");
	}
	k_mutex_unlock(&lock);
	return rc;
}

void app_store_set_autosave(bool on)
{
	autosave = on;
	if (!on) {
		(void)k_work_cancel_delayable(&save_work);
	}
	LOG_INF("store: autosave %s", on ? "on" : "off");
}

void app_store_get_status(struct app_store_status *st)
{
	k_mutex_lock(&lock, K_FOREVER);
	*st = (struct app_store_status){
		.ready = device_is_ready(eeprom),
		.valid = valid,
		.restored = restored,
		.autosave = autosave,
		.pending = k_work_delayable_is_pending(&save_work),
		.saves = saves,
		.last_rc = last_rc,
		.offset = CONFIG_APP_STORE_OFFSET,
		.size = CONFIG_APP_STORE_SIZE,
	};
	k_mutex_unlock(&lock);
}
