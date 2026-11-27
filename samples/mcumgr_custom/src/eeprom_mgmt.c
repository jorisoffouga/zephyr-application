/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Custom MCUmgr group: EEPROM click raw access and the LED settings
 * store. Protocol in include/eeprom_mgmt.h.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>

#include "app_store.h"
#include "eeprom_mgmt.h"

LOG_MODULE_REGISTER(eeprom_mgmt, CONFIG_APP_LOG_LEVEL);

#define EEPROM_NODE	DT_NODELABEL(eeprom)

static const struct device *const eeprom = DEVICE_DT_GET(EEPROM_NODE);

/* SMP requests run one at a time on the SMP work queue */
static uint8_t chunk[EEPROM_MGMT_CHUNK_MAX];
static uint8_t check[EEPROM_MGMT_CHUNK_MAX];

static int group_err(zcbor_state_t *zse, uint16_t err)
{
	return smp_add_cmd_err(zse, EEPROM_MGMT_GROUP_ID, err) ? MGMT_ERR_EOK
							      : MGMT_ERR_EMSGSIZE;
}

static bool range_ok(uint32_t off, uint32_t len, uint32_t max_len)
{
	size_t size = eeprom_get_size(eeprom);

	return len >= 1 && len <= max_len && off < size && len <= size - off;
}

static int eeprom_info(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	bool ok;

	if (!device_is_ready(eeprom)) {
		return group_err(zse, EEPROM_MGMT_ERR_NO_DEVICE);
	}

	ok = zcbor_tstr_put_lit(zse, "size") &&
	     zcbor_uint32_put(zse, eeprom_get_size(eeprom)) &&
	     zcbor_tstr_put_lit(zse, "page") &&
	     zcbor_uint32_put(zse, DT_PROP(EEPROM_NODE, pagesize)) &&
	     zcbor_tstr_put_lit(zse, "chip") && zcbor_tstr_put_lit(zse, "24C08");
#ifdef CONFIG_APP_STORE
	ok = ok && zcbor_tstr_put_lit(zse, "store_off") &&
	     zcbor_uint32_put(zse, CONFIG_APP_STORE_OFFSET) &&
	     zcbor_tstr_put_lit(zse, "store_size") &&
	     zcbor_uint32_put(zse, CONFIG_APP_STORE_SIZE);
#endif
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int eeprom_data_read(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;
	uint32_t off = 0, len = 0;
	size_t decoded;
	int rc;

	struct zcbor_map_decode_key_val decode[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("off", zcbor_uint32_decode, &off),
		ZCBOR_MAP_DECODE_KEY_DECODER("len", zcbor_uint32_decode, &len),
	};

	if (zcbor_map_decode_bulk(zsd, decode, ARRAY_SIZE(decode), &decoded) != 0) {
		return MGMT_ERR_EINVAL;
	}
	if (!device_is_ready(eeprom)) {
		return group_err(zse, EEPROM_MGMT_ERR_NO_DEVICE);
	}
	if (!range_ok(off, len, sizeof(chunk))) {
		return group_err(zse, EEPROM_MGMT_ERR_INVALID_RANGE);
	}

	rc = eeprom_read(eeprom, off, chunk, len);
	if (rc != 0) {
		LOG_WRN("read 0x%03x +%u failed (%d)", off, len, rc);
		return group_err(zse, EEPROM_MGMT_ERR_IO);
	}

	return (zcbor_tstr_put_lit(zse, "off") && zcbor_uint32_put(zse, off) &&
		zcbor_tstr_put_lit(zse, "data") && zcbor_bstr_encode_ptr(zse, (const char *)chunk, len))
		       ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int write_verify(uint32_t off, const uint8_t *data, size_t len)
{
	int rc = eeprom_write(eeprom, off, data, len);

	if (rc == 0) {
		rc = eeprom_read(eeprom, off, check, len);
	}
	if (rc == 0 && memcmp(data, check, len) != 0) {
		rc = -EIO;
		LOG_ERR("write 0x%03x +%u: reads back different: write protect "
			"(WP on mikroBUS 1 PWM PA1)?", off, (unsigned int)len);
	}
	return rc;
}

static int eeprom_data_write(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;
	struct zcbor_string data = { 0 };
	uint32_t off = 0;
	size_t decoded;
	int rc;

	struct zcbor_map_decode_key_val decode[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("off", zcbor_uint32_decode, &off),
		ZCBOR_MAP_DECODE_KEY_DECODER("data", zcbor_bstr_decode, &data),
	};

	if (zcbor_map_decode_bulk(zsd, decode, ARRAY_SIZE(decode), &decoded) != 0 ||
	    !zcbor_map_decode_bulk_key_found(decode, ARRAY_SIZE(decode), "data")) {
		return MGMT_ERR_EINVAL;
	}
	if (!device_is_ready(eeprom)) {
		return group_err(zse, EEPROM_MGMT_ERR_NO_DEVICE);
	}
	if (!range_ok(off, data.len, EEPROM_MGMT_CHUNK_MAX)) {
		return group_err(zse, EEPROM_MGMT_ERR_INVALID_RANGE);
	}

	rc = write_verify(off, data.value, data.len);
	if (rc != 0) {
		LOG_WRN("write 0x%03x +%u failed (%d)", off, (unsigned int)data.len, rc);
		return group_err(zse, EEPROM_MGMT_ERR_IO);
	}
	LOG_INF("wrote 0x%03x +%u", off, (unsigned int)data.len);

	return (zcbor_tstr_put_lit(zse, "off") && zcbor_uint32_put(zse, off) &&
		zcbor_tstr_put_lit(zse, "len") && zcbor_uint32_put(zse, data.len))
		       ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int eeprom_erase(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;
	uint32_t off = 0, len = 0;
	size_t decoded, size;
	int rc = 0;

	struct zcbor_map_decode_key_val decode[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("off", zcbor_uint32_decode, &off),
		ZCBOR_MAP_DECODE_KEY_DECODER("len", zcbor_uint32_decode, &len),
	};

	if (zcbor_map_decode_bulk(zsd, decode, ARRAY_SIZE(decode), &decoded) != 0) {
		return MGMT_ERR_EINVAL;
	}
	if (!device_is_ready(eeprom)) {
		return group_err(zse, EEPROM_MGMT_ERR_NO_DEVICE);
	}
	size = eeprom_get_size(eeprom);
	if (!zcbor_map_decode_bulk_key_found(decode, ARRAY_SIZE(decode), "len")) {
		len = size - MIN(off, size);
	}
	if (!range_ok(off, len, size)) {
		return group_err(zse, EEPROM_MGMT_ERR_INVALID_RANGE);
	}

	memset(chunk, 0xff, sizeof(chunk));
	for (uint32_t done = 0; done < len && rc == 0;) {
		uint32_t n = MIN(len - done, sizeof(chunk));

		rc = write_verify(off + done, chunk, n);
		done += n;
	}
	if (rc != 0) {
		return group_err(zse, EEPROM_MGMT_ERR_IO);
	}
	LOG_INF("erased 0x%03x +%u", off, len);

	return (zcbor_tstr_put_lit(zse, "off") && zcbor_uint32_put(zse, off) &&
		zcbor_tstr_put_lit(zse, "len") && zcbor_uint32_put(zse, len))
		       ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

#ifdef CONFIG_APP_STORE
static int encode_store(zcbor_state_t *zse)
{
	struct app_store_status st;

	app_store_get_status(&st);
	return (zcbor_tstr_put_lit(zse, "ready") && zcbor_bool_put(zse, st.ready) &&
		zcbor_tstr_put_lit(zse, "valid") && zcbor_bool_put(zse, st.valid) &&
		zcbor_tstr_put_lit(zse, "restored") && zcbor_bool_put(zse, st.restored) &&
		zcbor_tstr_put_lit(zse, "autosave") && zcbor_bool_put(zse, st.autosave) &&
		zcbor_tstr_put_lit(zse, "pending") && zcbor_bool_put(zse, st.pending) &&
		zcbor_tstr_put_lit(zse, "saves") && zcbor_uint32_put(zse, st.saves) &&
		zcbor_tstr_put_lit(zse, "last_rc") && zcbor_int32_put(zse, st.last_rc))
		       ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int eeprom_store_read(struct smp_streamer *ctxt)
{
	return encode_store(ctxt->writer->zs);
}

static int eeprom_store_write(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;
	struct zcbor_string action = { 0 };
	bool autosave = false;
	size_t decoded;
	int rc = 0;

	struct zcbor_map_decode_key_val decode[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("action", zcbor_tstr_decode, &action),
		ZCBOR_MAP_DECODE_KEY_DECODER("autosave", zcbor_bool_decode, &autosave),
	};

	if (zcbor_map_decode_bulk(zsd, decode, ARRAY_SIZE(decode), &decoded) != 0) {
		return MGMT_ERR_EINVAL;
	}
	if (!device_is_ready(eeprom)) {
		return group_err(zse, EEPROM_MGMT_ERR_NO_DEVICE);
	}

	if (zcbor_map_decode_bulk_key_found(decode, ARRAY_SIZE(decode), "autosave")) {
		app_store_set_autosave(autosave);
	}

	if (action.len == 0) {
		/* autosave only */
	} else if (action.len == 4 && memcmp(action.value, "save", 4) == 0) {
		rc = app_store_save();
	} else if (action.len == 7 && memcmp(action.value, "restore", 7) == 0) {
		rc = app_store_restore();
		if (rc == -ENOENT || rc == -EBADMSG || rc == -ESTALE) {
			return group_err(zse, EEPROM_MGMT_ERR_NO_RECORD);
		}
	} else if (action.len == 5 && memcmp(action.value, "clear", 5) == 0) {
		rc = app_store_clear();
	} else {
		return group_err(zse, EEPROM_MGMT_ERR_INVALID_ACTION);
	}

	if (rc != 0) {
		return group_err(zse, EEPROM_MGMT_ERR_IO);
	}
	return encode_store(zse);
}
#else
static int eeprom_store_read(struct smp_streamer *ctxt)
{
	return group_err(ctxt->writer->zs, EEPROM_MGMT_ERR_NO_STORE);
}
#define eeprom_store_write eeprom_store_read
#endif

#ifdef CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL
static int eeprom_mgmt_translate_error(uint16_t err)
{
	switch (err) {
	case EEPROM_MGMT_ERR_NO_DEVICE:
	case EEPROM_MGMT_ERR_NO_RECORD:
		return MGMT_ERR_ENOENT;
	case EEPROM_MGMT_ERR_IO:
		return MGMT_ERR_EBADSTATE;
	case EEPROM_MGMT_ERR_INVALID_RANGE:
	case EEPROM_MGMT_ERR_INVALID_ACTION:
		return MGMT_ERR_EINVAL;
	case EEPROM_MGMT_ERR_NO_STORE:
		return MGMT_ERR_ENOTSUP;
	default:
		return MGMT_ERR_EUNKNOWN;
	}
}
#endif

static const struct mgmt_handler eeprom_mgmt_handlers[] = {
	[EEPROM_MGMT_ID_INFO] = {
		.mh_read = eeprom_info,
		.mh_write = NULL,
	},
	[EEPROM_MGMT_ID_DATA] = {
		.mh_read = eeprom_data_read,
		.mh_write = eeprom_data_write,
	},
	[EEPROM_MGMT_ID_ERASE] = {
		.mh_read = NULL,
		.mh_write = eeprom_erase,
	},
	[EEPROM_MGMT_ID_STORE] = {
		.mh_read = eeprom_store_read,
		.mh_write = eeprom_store_write,
	},
};

static struct mgmt_group eeprom_mgmt_group = {
	.mg_handlers = eeprom_mgmt_handlers,
	.mg_handlers_count = ARRAY_SIZE(eeprom_mgmt_handlers),
	.mg_group_id = EEPROM_MGMT_GROUP_ID,
#ifdef CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL
	.mg_translate_error = eeprom_mgmt_translate_error,
#endif
#ifdef CONFIG_MCUMGR_GRP_ENUM_DETAILS_NAME
	.mg_group_name = "eeprom",
#endif
};

static void eeprom_mgmt_register(void)
{
	mgmt_register_group(&eeprom_mgmt_group);
}

MCUMGR_HANDLER_DEFINE(eeprom_mgmt, eeprom_mgmt_register);
