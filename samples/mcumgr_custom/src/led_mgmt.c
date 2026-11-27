/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Custom MCUmgr group: LED state over SMP. Protocol in include/led_mgmt.h.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>

#include <app_version.h>

#include "led_ctrl.h"
#include "led_mgmt.h"

/* version signed in the MCUboot header by imgtool */
#ifdef CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION
#define LED_MGMT_IMAGE_VERSION	CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION
#else
#define LED_MGMT_IMAGE_VERSION	APP_VERSION_EXTENDED_STRING
#endif

static bool zcbor_tstr_equals(const struct zcbor_string *s, const char *lit)
{
	size_t len = strlen(lit);

	return s->len == len && memcmp(s->value, lit, len) == 0;
}

static int mode_from_string(const struct zcbor_string *s, enum led_mode *mode)
{
	if (zcbor_tstr_equals(s, "off")) {
		*mode = LED_MODE_OFF;
	} else if (zcbor_tstr_equals(s, "on")) {
		*mode = LED_MODE_ON;
	} else if (zcbor_tstr_equals(s, "blink")) {
		*mode = LED_MODE_BLINK;
	} else {
		return -EINVAL;
	}

	return 0;
}

static bool encode_led(zcbor_state_t *zse, uint32_t idx)
{
	enum led_mode mode;
	uint32_t period_ms;

	if (led_ctrl_get(idx, &mode, &period_ms) != 0) {
		return false;
	}

	return zcbor_tstr_put_lit(zse, "led") &&
	       zcbor_uint32_put(zse, idx) &&
	       zcbor_tstr_put_lit(zse, "name") &&
	       zcbor_tstr_put_term(zse, led_ctrl_name(idx), 32) &&
	       zcbor_tstr_put_lit(zse, "mode") &&
	       zcbor_tstr_put_term(zse, led_mode_name(mode), sizeof("blink")) &&
	       zcbor_tstr_put_lit(zse, "period_ms") &&
	       zcbor_uint32_put(zse, period_ms);
}

/* read: state of all the LEDs */
static int led_mgmt_state_read(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	size_t count = led_ctrl_count();
	bool ok;

	ok = zcbor_tstr_put_lit(zse, "leds") &&
	     zcbor_list_start_encode(zse, count);

	for (size_t i = 0; ok && i < count; i++) {
		ok = zcbor_map_start_encode(zse, 4) &&
		     encode_led(zse, i) &&
		     zcbor_map_end_encode(zse, 4);
	}

	ok = ok && zcbor_list_end_encode(zse, count);

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

/* write: set one LED, answer with its new state */
static int led_mgmt_state_write(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;
	struct zcbor_string mode_str = { 0 };
	uint32_t led = 0;
	uint32_t period_ms = 0;
	enum led_mode mode;
	size_t decoded;
	uint16_t err;
	bool ok;
	int rc;

	struct zcbor_map_decode_key_val decode[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("led", zcbor_uint32_decode, &led),
		ZCBOR_MAP_DECODE_KEY_DECODER("mode", zcbor_tstr_decode, &mode_str),
		ZCBOR_MAP_DECODE_KEY_DECODER("period_ms", zcbor_uint32_decode, &period_ms),
	};

	if (zcbor_map_decode_bulk(zsd, decode, ARRAY_SIZE(decode), &decoded) != 0 ||
	    !zcbor_map_decode_bulk_key_found(decode, ARRAY_SIZE(decode), "mode")) {
		return MGMT_ERR_EINVAL;
	}

	if (led >= led_ctrl_count()) {
		err = LED_MGMT_ERR_INVALID_LED;
	} else if (mode_from_string(&mode_str, &mode) != 0) {
		err = LED_MGMT_ERR_INVALID_MODE;
	} else {
		rc = led_ctrl_set(led, mode, period_ms);
		switch (rc) {
		case 0:
			err = LED_MGMT_ERR_OK;
			break;
		case -ERANGE:
			err = LED_MGMT_ERR_INVALID_PERIOD;
			break;
		case -EINVAL:
			err = LED_MGMT_ERR_INVALID_LED;
			break;
		default:
			err = LED_MGMT_ERR_GPIO;
			break;
		}
	}

	if (err != LED_MGMT_ERR_OK) {
		ok = smp_add_cmd_err(zse, LED_MGMT_GROUP_ID, err);
	} else {
		ok = encode_led(zse, led);
	}

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

/* read: protocol, image and build versions */
static int led_mgmt_version_read(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	bool ok;

	ok = zcbor_tstr_put_lit(zse, "protocol") &&
	     zcbor_uint32_put(zse, LED_MGMT_PROTOCOL_VERSION) &&
	     zcbor_tstr_put_lit(zse, "image") &&
	     zcbor_tstr_put_lit(zse, LED_MGMT_IMAGE_VERSION) &&
	     zcbor_tstr_put_lit(zse, "build") &&
	     zcbor_tstr_put_lit(zse, STRINGIFY(APP_BUILD_VERSION)) &&
	     zcbor_tstr_put_lit(zse, "leds") &&
	     zcbor_uint32_put(zse, led_ctrl_count());

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

#ifdef CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL
/* SMP v1 clients only get "rc": map the group errors to MGMT_ERR_* */
static int led_mgmt_translate_error(uint16_t err)
{
	switch (err) {
	case LED_MGMT_ERR_INVALID_LED:
		return MGMT_ERR_ENOENT;
	case LED_MGMT_ERR_INVALID_MODE:
	case LED_MGMT_ERR_INVALID_PERIOD:
		return MGMT_ERR_EINVAL;
	case LED_MGMT_ERR_GPIO:
		return MGMT_ERR_EBADSTATE;
	default:
		return MGMT_ERR_EUNKNOWN;
	}
}
#endif

static const struct mgmt_handler led_mgmt_handlers[] = {
	[LED_MGMT_ID_STATE] = {
		.mh_read = led_mgmt_state_read,
		.mh_write = led_mgmt_state_write,
	},
	[LED_MGMT_ID_VERSION] = {
		.mh_read = led_mgmt_version_read,
		.mh_write = NULL,
	},
};

static struct mgmt_group led_mgmt_group = {
	.mg_handlers = led_mgmt_handlers,
	.mg_handlers_count = ARRAY_SIZE(led_mgmt_handlers),
	.mg_group_id = LED_MGMT_GROUP_ID,
#ifdef CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL
	.mg_translate_error = led_mgmt_translate_error,
#endif
#ifdef CONFIG_MCUMGR_GRP_ENUM_DETAILS_NAME
	.mg_group_name = "led",
#endif
};

static void led_mgmt_register(void)
{
	mgmt_register_group(&led_mgmt_group);
}

MCUMGR_HANDLER_DEFINE(led_mgmt, led_mgmt_register);
