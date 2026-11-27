/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Custom MCUmgr group: Alcohol click (MQ-3) on the mikroBUS 1 AN pin.
 * Protocol in include/alcohol_mgmt.h.
 *
 * The ADC channel comes from the devicetree: zephyr,user io-channels,
 * name "alcohol" (boards/stm32f4_disco.overlay).
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>

#include "alcohol_mgmt.h"

LOG_MODULE_REGISTER(alcohol, CONFIG_APP_LOG_LEVEL);

static const struct adc_dt_spec adc = ADC_DT_SPEC_GET_BY_NAME(DT_PATH(zephyr_user), alcohol);

static K_MUTEX_DEFINE(lock);
static bool setup_done;
static int setup_rc;
static bool saturated_logged;
static int32_t baseline_mv;	/* 0: none */

struct reading {
	int32_t raw;
	int32_t mv;
	int32_t full_mv;
	bool saturated;
};

/* lazy: first SMP request, from the SMP work queue */
static int adc_setup(void)
{
	if (setup_done) {
		return setup_rc;
	}
	setup_done = true;

	if (!adc_is_ready_dt(&adc)) {
		setup_rc = -ENODEV;
	} else {
		setup_rc = adc_channel_setup_dt(&adc);
	}

	if (setup_rc == 0) {
		LOG_INF("alcohol: ADC %s channel %u, %u bit, VREF %u mV", adc.dev->name,
			adc.channel_id, adc.resolution, adc.vref_mv);
	} else {
		LOG_ERR("alcohol: ADC setup failed (%d)", setup_rc);
	}
	return setup_rc;
}

static int measure(struct reading *r)
{
	uint16_t sample;
	struct adc_sequence seq = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};
	int32_t sum = 0;
	int32_t top = BIT(adc.resolution) - 1;
	int rc;

	rc = adc_sequence_init_dt(&adc, &seq);
	if (rc != 0) {
		return rc;
	}

	for (int i = 0; i < CONFIG_APP_ALCOHOL_SAMPLES; i++) {
		rc = adc_read_dt(&adc, &seq);
		if (rc != 0) {
			return rc;
		}
		sum += sample;
	}

	r->raw = sum / CONFIG_APP_ALCOHOL_SAMPLES;
	r->mv = r->raw;
	r->full_mv = adc.vref_mv;
	/* within 0.5 % of the full scale: clipped, the real voltage may be higher */
	r->saturated = r->raw >= top - top / 200;

	rc = adc_raw_to_millivolts_dt(&adc, &r->mv);
	if (rc != 0) {
		return rc;
	}

	if (r->saturated && !saturated_logged) {
		LOG_WRN("alcohol: AN at the ADC full scale (%d mV): the click output "
			"may exceed 3.3 V, lower its load resistor", r->mv);
		saturated_logged = true;
	}
	return 0;
}

static int encode(zcbor_state_t *zse, const struct reading *r)
{
	bool warming = k_uptime_get() < (int64_t)CONFIG_APP_ALCOHOL_WARMUP_S * MSEC_PER_SEC;
	bool ok;

	ok = zcbor_tstr_put_lit(zse, "raw") && zcbor_int32_put(zse, r->raw) &&
	     zcbor_tstr_put_lit(zse, "mv") && zcbor_int32_put(zse, r->mv) &&
	     zcbor_tstr_put_lit(zse, "full_mv") && zcbor_int32_put(zse, r->full_mv) &&
	     zcbor_tstr_put_lit(zse, "saturated") && zcbor_bool_put(zse, r->saturated) &&
	     zcbor_tstr_put_lit(zse, "warming") && zcbor_bool_put(zse, warming);

	if (ok && baseline_mv > 0) {
		ok = zcbor_tstr_put_lit(zse, "baseline_mv") &&
		     zcbor_int32_put(zse, baseline_mv) &&
		     zcbor_tstr_put_lit(zse, "delta_mv") &&
		     zcbor_int32_put(zse, r->mv - baseline_mv);
	}

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int group_err(zcbor_state_t *zse, uint16_t err)
{
	return smp_add_cmd_err(zse, ALCOHOL_MGMT_GROUP_ID, err) ? MGMT_ERR_EOK
							       : MGMT_ERR_EMSGSIZE;
}

/* reads the sensor; err set on failure */
static uint16_t read_locked(struct reading *r)
{
	int rc;

	if (adc_setup() != 0) {
		return ALCOHOL_MGMT_ERR_NO_DEVICE;
	}

	rc = measure(r);
	if (rc != 0) {
		LOG_WRN("alcohol: ADC read failed (%d)", rc);
		return ALCOHOL_MGMT_ERR_IO;
	}

	LOG_DBG("alcohol: raw %d, %d mV", r->raw, r->mv);
	return ALCOHOL_MGMT_ERR_OK;
}

static int alcohol_measure_read(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	struct reading r;
	uint16_t err;

	k_mutex_lock(&lock, K_FOREVER);
	err = read_locked(&r);
	k_mutex_unlock(&lock);

	return err != ALCOHOL_MGMT_ERR_OK ? group_err(zse, err) : encode(zse, &r);
}

static int alcohol_measure_write(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;
	bool calibrate = false;
	int32_t set_mv = -1;
	size_t decoded;
	struct reading r;
	uint16_t err;

	struct zcbor_map_decode_key_val decode[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("calibrate", zcbor_bool_decode, &calibrate),
		ZCBOR_MAP_DECODE_KEY_DECODER("baseline_mv", zcbor_int32_decode, &set_mv),
	};

	if (zcbor_map_decode_bulk(zsd, decode, ARRAY_SIZE(decode), &decoded) != 0) {
		return MGMT_ERR_EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);
	err = read_locked(&r);
	if (err == ALCOHOL_MGMT_ERR_OK) {
		if (calibrate) {
			baseline_mv = MAX(r.mv, 1);
			LOG_INF("alcohol: baseline %d mV (clean air)", baseline_mv);
		} else if (set_mv == 0) {
			baseline_mv = 0;
			LOG_INF("alcohol: no baseline");
		} else if (set_mv > 0 && set_mv <= r.full_mv) {
			baseline_mv = set_mv;
			LOG_INF("alcohol: baseline %d mV", baseline_mv);
		} else if (set_mv != -1) {
			err = ALCOHOL_MGMT_ERR_INVALID;
		}
	}
	k_mutex_unlock(&lock);

	return err != ALCOHOL_MGMT_ERR_OK ? group_err(zse, err) : encode(zse, &r);
}

#ifdef CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL
static int alcohol_mgmt_translate_error(uint16_t err)
{
	switch (err) {
	case ALCOHOL_MGMT_ERR_NO_DEVICE:
		return MGMT_ERR_ENOENT;
	case ALCOHOL_MGMT_ERR_IO:
		return MGMT_ERR_EBADSTATE;
	case ALCOHOL_MGMT_ERR_INVALID:
		return MGMT_ERR_EINVAL;
	default:
		return MGMT_ERR_EUNKNOWN;
	}
}
#endif

static const struct mgmt_handler alcohol_mgmt_handlers[] = {
	[ALCOHOL_MGMT_ID_MEASURE] = {
		.mh_read = alcohol_measure_read,
		.mh_write = alcohol_measure_write,
	},
};

static struct mgmt_group alcohol_mgmt_group = {
	.mg_handlers = alcohol_mgmt_handlers,
	.mg_handlers_count = ARRAY_SIZE(alcohol_mgmt_handlers),
	.mg_group_id = ALCOHOL_MGMT_GROUP_ID,
#ifdef CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL
	.mg_translate_error = alcohol_mgmt_translate_error,
#endif
#ifdef CONFIG_MCUMGR_GRP_ENUM_DETAILS_NAME
	.mg_group_name = "alcohol",
#endif
};

static void alcohol_mgmt_register(void)
{
	mgmt_register_group(&alcohol_mgmt_group);
}

MCUMGR_HANDLER_DEFINE(alcohol_mgmt, alcohol_mgmt_register);
