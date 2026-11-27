/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Custom MCUmgr group: LED patterns. Protocol in include/pattern_mgmt.h.
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>

#include "led_ctrl.h"
#include "pattern.h"
#include "pattern_mgmt.h"

static bool encode_bank(zcbor_state_t *zse, enum led_bank bank)
{
	struct pattern_state st;
	size_t first, n;

	pattern_get(bank, &st);
	led_ctrl_bank(bank, &first, &n);

	return zcbor_map_start_encode(zse, 5) &&
	       zcbor_tstr_put_lit(zse, "bank") &&
	       zcbor_tstr_put_term(zse, led_bank_name(bank), 16) &&
	       zcbor_tstr_put_lit(zse, "leds") && zcbor_uint32_put(zse, n) &&
	       zcbor_tstr_put_lit(zse, "pattern") &&
	       zcbor_tstr_put_term(zse, pattern_name(st.id), 16) &&
	       zcbor_tstr_put_lit(zse, "step_ms") &&
	       zcbor_uint32_put(zse, st.id == PATTERN_OFF ? 0 : st.step_ms) &&
	       zcbor_tstr_put_lit(zse, "width") &&
	       zcbor_uint32_put(zse, st.id == PATTERN_OFF ? 0 : st.width) &&
	       zcbor_map_end_encode(zse, 5);
}

static int pattern_mgmt_read(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	bool ok;

	ok = zcbor_tstr_put_lit(zse, "patterns") &&
	     zcbor_list_start_encode(zse, PATTERN_COUNT);
	for (int i = 0; ok && i < PATTERN_COUNT; i++) {
		ok = zcbor_tstr_put_term(zse, pattern_name(i), 16);
	}
	ok = ok && zcbor_list_end_encode(zse, PATTERN_COUNT) &&
	     zcbor_tstr_put_lit(zse, "banks") &&
	     zcbor_list_start_encode(zse, LED_BANK_COUNT);
	for (int b = 0; ok && b < LED_BANK_COUNT; b++) {
		size_t first, n;

		led_ctrl_bank(b, &first, &n);
		if (n > 0) {
			ok = encode_bank(zse, b);
		}
	}
	ok = ok && zcbor_list_end_encode(zse, LED_BANK_COUNT);

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int pattern_mgmt_write(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;
	struct zcbor_string bank_str = { 0 }, pattern_str = { 0 };
	uint32_t step_ms = PATTERN_DEFAULT_STEP_MS;
	uint32_t width = PATTERN_DEFAULT_WIDTH;
	enum led_bank bank;
	enum pattern_id id;
	uint16_t err = PATTERN_MGMT_ERR_OK;
	size_t decoded;
	int rc;

	struct zcbor_map_decode_key_val decode[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("bank", zcbor_tstr_decode, &bank_str),
		ZCBOR_MAP_DECODE_KEY_DECODER("pattern", zcbor_tstr_decode, &pattern_str),
		ZCBOR_MAP_DECODE_KEY_DECODER("step_ms", zcbor_uint32_decode, &step_ms),
		ZCBOR_MAP_DECODE_KEY_DECODER("width", zcbor_uint32_decode, &width),
	};

	if (zcbor_map_decode_bulk(zsd, decode, ARRAY_SIZE(decode), &decoded) != 0 ||
	    !zcbor_map_decode_bulk_key_found(decode, ARRAY_SIZE(decode), "bank") ||
	    !zcbor_map_decode_bulk_key_found(decode, ARRAY_SIZE(decode), "pattern")) {
		return MGMT_ERR_EINVAL;
	}

	if (led_bank_from_name(bank_str.value, bank_str.len, &bank) != 0) {
		err = PATTERN_MGMT_ERR_INVALID_BANK;
	} else if (pattern_from_name(pattern_str.value, pattern_str.len, &id) != 0) {
		err = PATTERN_MGMT_ERR_INVALID_PATTERN;
	} else {
		rc = pattern_start(bank, id, step_ms, width);
		if (rc == -ERANGE) {
			err = PATTERN_MGMT_ERR_INVALID_STEP;
		} else if (rc == -ENODEV) {
			err = PATTERN_MGMT_ERR_INVALID_BANK;
		} else if (rc != 0) {
			err = PATTERN_MGMT_ERR_UNKNOWN;
		}
	}

	if (err != PATTERN_MGMT_ERR_OK) {
		return smp_add_cmd_err(zse, PATTERN_MGMT_GROUP_ID, err) ?
		       MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
	}

	/* the response is the bank entry, flattened in the base map */
	{
		struct pattern_state st;
		size_t first, n;
		bool ok;

		pattern_get(bank, &st);
		led_ctrl_bank(bank, &first, &n);
		ok = zcbor_tstr_put_lit(zse, "bank") &&
		     zcbor_tstr_put_term(zse, led_bank_name(bank), 16) &&
		     zcbor_tstr_put_lit(zse, "leds") && zcbor_uint32_put(zse, n) &&
		     zcbor_tstr_put_lit(zse, "pattern") &&
		     zcbor_tstr_put_term(zse, pattern_name(st.id), 16) &&
		     zcbor_tstr_put_lit(zse, "step_ms") &&
		     zcbor_uint32_put(zse, st.id == PATTERN_OFF ? 0 : st.step_ms) &&
		     zcbor_tstr_put_lit(zse, "width") &&
		     zcbor_uint32_put(zse, st.id == PATTERN_OFF ? 0 : st.width);
		return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
	}
}

#ifdef CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL
static int pattern_mgmt_translate_error(uint16_t err)
{
	switch (err) {
	case PATTERN_MGMT_ERR_INVALID_BANK:
		return MGMT_ERR_ENOENT;
	case PATTERN_MGMT_ERR_INVALID_PATTERN:
	case PATTERN_MGMT_ERR_INVALID_STEP:
		return MGMT_ERR_EINVAL;
	default:
		return MGMT_ERR_EUNKNOWN;
	}
}
#endif

static const struct mgmt_handler pattern_mgmt_handlers[] = {
	[PATTERN_MGMT_ID_STATE] = {
		.mh_read = pattern_mgmt_read,
		.mh_write = pattern_mgmt_write,
	},
};

static struct mgmt_group pattern_mgmt_group = {
	.mg_handlers = pattern_mgmt_handlers,
	.mg_handlers_count = ARRAY_SIZE(pattern_mgmt_handlers),
	.mg_group_id = PATTERN_MGMT_GROUP_ID,
#ifdef CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL
	.mg_translate_error = pattern_mgmt_translate_error,
#endif
#ifdef CONFIG_MCUMGR_GRP_ENUM_DETAILS_NAME
	.mg_group_name = "pattern",
#endif
};

static void pattern_mgmt_register(void)
{
	mgmt_register_group(&pattern_mgmt_group);
}

MCUMGR_HANDLER_DEFINE(pattern_mgmt, pattern_mgmt_register);
