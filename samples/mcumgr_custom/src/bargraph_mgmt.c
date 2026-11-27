/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Custom MCUmgr group: BarGraph click. Protocol in include/bargraph_mgmt.h.
 *
 * The segments are the LEDs of the bargraph_leds bank of led_ctrl (one
 * state for both groups); the brightness is the PWM on the mikroBUS 3 PWM pin.
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>

#include "bargraph_mgmt.h"
#include "led_ctrl.h"

LOG_MODULE_REGISTER(bargraph, CONFIG_APP_LOG_LEVEL);

static const struct pwm_dt_spec oe = PWM_DT_SPEC_GET(DT_NODELABEL(bargraph_oe));

/* written by the SMP work queue only (and once before, at init) */
static uint32_t brightness = 100;

static int set_brightness(uint32_t percent)
{
	uint32_t pulse;

	if (!pwm_is_ready_dt(&oe)) {
		return -ENODEV;
	}

	/* the pulse is the time the segments are on (polarity in the overlay) */
	pulse = (uint32_t)(((uint64_t)oe.period * percent) / 100U);

	return pwm_set_pulse_dt(&oe, pulse);
}

static uint32_t read_mask(size_t first, size_t count)
{
	uint32_t mask = 0;

	for (size_t i = 0; i < count; i++) {
		enum led_mode mode;
		uint32_t period_ms;

		if (led_ctrl_get(first + i, &mode, &period_ms) == 0 &&
		    mode != LED_MODE_OFF) {
			mask |= BIT(i);
		}
	}

	return mask;
}

static int write_mask(size_t first, size_t count, uint32_t mask)
{
	for (size_t i = 0; i < count; i++) {
		int rc = led_ctrl_set(first + i,
				      (mask & BIT(i)) ? LED_MODE_ON : LED_MODE_OFF, 0);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static uint16_t errno_to_err(int rc)
{
	return (rc == -ENODEV) ? BARGRAPH_MGMT_ERR_NO_DEVICE : BARGRAPH_MGMT_ERR_IO;
}

static bool encode_state(zcbor_state_t *zse, size_t first, size_t count)
{
	return zcbor_tstr_put_lit(zse, "mask") &&
	       zcbor_uint32_put(zse, read_mask(first, count)) &&
	       zcbor_tstr_put_lit(zse, "segments") &&
	       zcbor_uint32_put(zse, count) &&
	       zcbor_tstr_put_lit(zse, "brightness") &&
	       zcbor_uint32_put(zse, brightness);
}

static int bargraph_mgmt_state_read(struct smp_streamer *ctxt)
{
	size_t first, count;

	led_ctrl_bank(LED_BANK_BARGRAPH, &first, &count);

	return encode_state(ctxt->writer->zs, first, count) ?
	       MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int bargraph_mgmt_state_write(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;
	uint32_t level = 0, mask = 0, percent = 0;
	bool has_level, has_mask, has_brightness;
	size_t first, count, decoded;
	uint16_t err = BARGRAPH_MGMT_ERR_OK;
	int rc;

	struct zcbor_map_decode_key_val decode[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("level", zcbor_uint32_decode, &level),
		ZCBOR_MAP_DECODE_KEY_DECODER("mask", zcbor_uint32_decode, &mask),
		ZCBOR_MAP_DECODE_KEY_DECODER("brightness", zcbor_uint32_decode, &percent),
	};

	if (zcbor_map_decode_bulk(zsd, decode, ARRAY_SIZE(decode), &decoded) != 0) {
		return MGMT_ERR_EINVAL;
	}

	has_level = zcbor_map_decode_bulk_key_found(decode, ARRAY_SIZE(decode), "level");
	has_mask = zcbor_map_decode_bulk_key_found(decode, ARRAY_SIZE(decode), "mask");
	has_brightness = zcbor_map_decode_bulk_key_found(decode, ARRAY_SIZE(decode),
							 "brightness");

	if ((!has_level && !has_mask && !has_brightness) || (has_level && has_mask)) {
		return MGMT_ERR_EINVAL;
	}

	led_ctrl_bank(LED_BANK_BARGRAPH, &first, &count);

	/* check everything before changing anything */
	if (count == 0) {
		err = BARGRAPH_MGMT_ERR_NO_DEVICE;
	} else if (has_level && level > count) {
		err = BARGRAPH_MGMT_ERR_INVALID_LEVEL;
	} else if (has_mask && (mask & ~BIT_MASK(count)) != 0) {
		err = BARGRAPH_MGMT_ERR_INVALID_MASK;
	} else if (has_brightness && percent > 100) {
		err = BARGRAPH_MGMT_ERR_INVALID_BRIGHTNESS;
	}

	if (err == BARGRAPH_MGMT_ERR_OK && (has_level || has_mask)) {
		rc = write_mask(first, count, has_level ? BIT_MASK(level) : mask);
		if (rc != 0) {
			err = errno_to_err(rc);
		}
	}

	if (err == BARGRAPH_MGMT_ERR_OK && has_brightness) {
		rc = set_brightness(percent);
		if (rc == 0) {
			brightness = percent;
		} else {
			err = errno_to_err(rc);
		}
	}

	if (err != BARGRAPH_MGMT_ERR_OK) {
		LOG_WRN("write rejected, error %u", err);
		return smp_add_cmd_err(zse, BARGRAPH_MGMT_GROUP_ID, err) ?
		       MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
	}

	LOG_INF("mask 0x%03x, brightness %u%%", read_mask(first, count), brightness);

	return encode_state(zse, first, count) ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

#ifdef CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL
static int bargraph_mgmt_translate_error(uint16_t err)
{
	switch (err) {
	case BARGRAPH_MGMT_ERR_INVALID_LEVEL:
	case BARGRAPH_MGMT_ERR_INVALID_MASK:
	case BARGRAPH_MGMT_ERR_INVALID_BRIGHTNESS:
		return MGMT_ERR_EINVAL;
	case BARGRAPH_MGMT_ERR_NO_DEVICE:
		return MGMT_ERR_ENOENT;
	case BARGRAPH_MGMT_ERR_IO:
		return MGMT_ERR_EBADSTATE;
	default:
		return MGMT_ERR_EUNKNOWN;
	}
}
#endif

static const struct mgmt_handler bargraph_mgmt_handlers[] = {
	[BARGRAPH_MGMT_ID_STATE] = {
		.mh_read = bargraph_mgmt_state_read,
		.mh_write = bargraph_mgmt_state_write,
	},
};

static struct mgmt_group bargraph_mgmt_group = {
	.mg_handlers = bargraph_mgmt_handlers,
	.mg_handlers_count = ARRAY_SIZE(bargraph_mgmt_handlers),
	.mg_group_id = BARGRAPH_MGMT_GROUP_ID,
#ifdef CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL
	.mg_translate_error = bargraph_mgmt_translate_error,
#endif
#ifdef CONFIG_MCUMGR_GRP_ENUM_DETAILS_NAME
	.mg_group_name = "bargraph",
#endif
};

static void bargraph_mgmt_register(void)
{
	mgmt_register_group(&bargraph_mgmt_group);
}

MCUMGR_HANDLER_DEFINE(bargraph_mgmt, bargraph_mgmt_register);

/* drive /OE at boot: segments enabled, full brightness */
static int bargraph_pwm_init(void)
{
	int rc = set_brightness(brightness);

	if (rc != 0) {
		LOG_WRN("PWM /OE not set (%d): segments may stay off", rc);
	}
	return 0;
}

SYS_INIT(bargraph_pwm_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
