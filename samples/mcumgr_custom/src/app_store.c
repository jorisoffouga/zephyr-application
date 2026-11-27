/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LED settings in the EEPROM click. Record layout in include/app_store.h.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt_callbacks.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include "app_store.h"
#include "led_ctrl.h"
#include "pattern.h"

LOG_MODULE_REGISTER(app_store, CONFIG_APP_LOG_LEVEL);

#define STORE_MAGIC	0x7473434dU	/* "MCst" */
#define STORE_VERSION	2
#define HDR_SIZE	16
#define CRC_FROM	12		/* the CRC covers the sequence number and the payload */
#define LED_REC_SIZE	3
#define BANK_REC_SIZE	4
/* board 4 + expander 8 + bargraph 10 LEDs */
#define MAX_LEDS	32
#define RECORD_MAX	(HDR_SIZE + MAX_LEDS * LED_REC_SIZE + LED_BANK_COUNT * BANK_REC_SIZE)
#define SLOT_SIZE	(CONFIG_APP_STORE_SIZE / 2)

BUILD_ASSERT(RECORD_MAX <= SLOT_SIZE, "APP_STORE_SIZE too small for two copies");

static const struct device *const eeprom = DEVICE_DT_GET(DT_NODELABEL(eeprom));

static K_MUTEX_DEFINE(lock);
static uint8_t buf[RECORD_MAX];		/* the record to write */
static uint8_t old[RECORD_MAX];		/* the stored record in use */
static uint8_t check[RECORD_MAX];	/* read back */
static int old_len;			/* length of old, or -errno: none valid */
static uint8_t cur_slot;
static uint32_t cur_seq;
static bool autosave = IS_ENABLED(CONFIG_APP_STORE_AUTOSAVE);
static bool restored;
static uint32_t saves;
static int last_rc;

static void save_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(save_work, save_work_fn);

static off_t slot_off(int slot)
{
	return CONFIG_APP_STORE_OFFSET + slot * SLOT_SIZE;
}

static uint16_t record_crc(const uint8_t *rec, uint16_t len)
{
	return crc16_ccitt(0, &rec[CRC_FROM], HDR_SIZE - CRC_FROM + len);
}

/* the record of the current state in buf, with sequence number seq; its length */
static size_t build(uint32_t seq)
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
	sys_put_le32(seq, &buf[12]);

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
	sys_put_le16(record_crc(buf, len), &buf[10]);
	return pos;
}

/* reads and checks one copy into rec; its length, or -errno */
static int load_slot(int slot, uint8_t *rec)
{
	uint16_t len;
	int rc;

	rc = eeprom_read(eeprom, slot_off(slot), rec, HDR_SIZE);
	if (rc != 0) {
		return rc;
	}
	len = sys_get_le16(&rec[8]);
	if (sys_get_le32(&rec[0]) != STORE_MAGIC || rec[4] != STORE_VERSION ||
	    len > RECORD_MAX - HDR_SIZE) {
		return -ENOENT;
	}
	rc = eeprom_read(eeprom, slot_off(slot) + HDR_SIZE, &rec[HDR_SIZE], len);
	if (rc != 0) {
		return rc;
	}
	if (record_crc(rec, len) != sys_get_le16(&rec[10])) {
		return -EBADMSG;
	}
	return HDR_SIZE + len;
}

/* the newest valid copy into old; old_len = its length or -errno */
static void load(void)
{
	int len0 = load_slot(0, old);
	uint32_t seq0 = len0 > 0 ? sys_get_le32(&old[12]) : 0;
	int len1 = load_slot(1, check);
	uint32_t seq1 = len1 > 0 ? sys_get_le32(&check[12]) : 0;

	if (len1 > 0 && (len0 <= 0 || seq1 > seq0)) {
		memcpy(old, check, len1);
		old_len = len1;
		cur_slot = 1;
		cur_seq = seq1;
	} else if (len0 > 0) {
		old_len = len0;
		cur_slot = 0;
		cur_seq = seq0;
	} else {
		/* nothing valid: report the more telling error */
		old_len = (len0 == -ENOENT) ? len1 : len0;
		cur_slot = 1;		/* the first save goes to copy 0 */
		cur_seq = 0;
	}
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
	load();
	rc = old_len;
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

	LOG_INF("store: settings restored from the EEPROM (copy %u, #%u)", cur_slot, cur_seq);
	return 0;
}

static int save_locked(void)
{
	size_t len = build(cur_seq + 1);
	int slot = cur_slot ^ 1;
	int rc;

	/* same content (sequence number and CRC aside): no EEPROM write */
	if (old_len == (int)len && memcmp(&old[4], &buf[4], 6) == 0 &&
	    memcmp(&old[HDR_SIZE], &buf[HDR_SIZE], len - HDR_SIZE) == 0) {
		return 0;
	}

	rc = eeprom_write(eeprom, slot_off(slot), buf, len);
	if (rc == 0) {
		rc = eeprom_read(eeprom, slot_off(slot), check, len);
	}
	if (rc == 0 && memcmp(buf, check, len) != 0) {
		rc = -EIO;
	}
	last_rc = rc;
	if (rc != 0) {
		LOG_ERR("store: EEPROM write failed (%d), the previous settings stay", rc);
		return rc;
	}

	memcpy(old, buf, len);
	old_len = len;
	cur_slot = slot;
	cur_seq++;
	saves++;
	LOG_INF("store: settings saved (%u bytes, copy %d, #%u)", (unsigned int)len, slot,
		cur_seq);
	return 0;
}

int app_store_save(void)
{
	int rc;

	if (!device_is_ready(eeprom)) {
		return -ENODEV;
	}
	(void)k_work_cancel_delayable(&save_work);
	k_mutex_lock(&lock, K_FOREVER);
	if (old_len == 0) {
		load();		/* first save: find the copy in use */
	}
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
	rc = eeprom_write(eeprom, slot_off(0), blank, sizeof(blank));
	if (rc == 0) {
		rc = eeprom_write(eeprom, slot_off(1), blank, sizeof(blank));
	}
	last_rc = rc;
	if (rc == 0) {
		old_len = -ENOENT;
		cur_slot = 1;
		cur_seq = 0;
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
		.valid = old_len > 0,
		.restored = restored,
		.autosave = autosave,
		.pending = k_work_delayable_is_pending(&save_work),
		.slot = cur_slot,
		.seq = cur_seq,
		.saves = saves,
		.last_rc = last_rc,
		.offset = CONFIG_APP_STORE_OFFSET,
		.size = CONFIG_APP_STORE_SIZE,
	};
	k_mutex_unlock(&lock);
}

#ifdef CONFIG_MCUMGR_GRP_OS_RESET_HOOK
/* SMP reset (smp_client reset, web UI, firmware update): write a pending save first */
static enum mgmt_cb_return reset_hook(uint32_t event, enum mgmt_cb_return prev_status,
				      int32_t *rc, uint16_t *group, bool *abort_more,
				      void *data, size_t data_size)
{
	ARG_UNUSED(event);
	ARG_UNUSED(prev_status);
	ARG_UNUSED(rc);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data);
	ARG_UNUSED(data_size);

	if (k_work_delayable_is_pending(&save_work)) {
		LOG_INF("store: reset requested, saving now");
		(void)app_store_save();
	}
	return MGMT_CB_OK;
}

static struct mgmt_callback reset_cb = {
	.callback = reset_hook,
	.event_id = MGMT_EVT_OP_OS_MGMT_RESET,
};

static int app_store_hooks(void)
{
	mgmt_callback_register(&reset_cb);
	return 0;
}

SYS_INIT(app_store_hooks, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif
