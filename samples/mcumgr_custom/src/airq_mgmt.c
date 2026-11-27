/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Custom MCUmgr group: Air Quality 3 click (CCS811). Protocol in
 * include/airq_mgmt.h.
 *
 * The ccs811 node has zephyr,deferred-init: the driver starts from a
 * delayed work item, once the USB log is up, so a failure is visible in
 * the log with the I2C probe that explains it.
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_encode.h>

#include "airq_mgmt.h"

LOG_MODULE_REGISTER(airq, CONFIG_APP_LOG_LEVEL);

#define CCS811_NODE	DT_COMPAT_GET_ANY_STATUS_OKAY(ams_ccs811)

/* registers, from the CCS811 datasheet (drivers/sensor/ams/ccs811/ccs811.h) */
#define REG_STATUS		0x00
#define REG_HW_ID		0x20
#define REG_HW_VERSION		0x21
#define REG_FW_APP_VERSION	0x24
#define REG_ERROR_ID		0xE0
#define CCS811_HW_ID		0x81
#define CCS811_ADDR_LOW		0x5A
#define CCS811_ADDR_HIGH	0x5B

static const struct device *const ccs811 = DEVICE_DT_GET(CCS811_NODE);
static const struct device *const i2c_bus = DEVICE_DT_GET(DT_BUS(CCS811_NODE));
static const struct gpio_dt_spec wake = GPIO_DT_SPEC_GET_OR(CCS811_NODE, wake_gpios, {0});
static const struct gpio_dt_spec nreset = GPIO_DT_SPEC_GET_OR(CCS811_NODE, reset_gpios, {0});

static volatile bool init_done;
static volatile int init_rc;
static bool no_device_logged;

struct probe {
	uint8_t addr;
	bool ack;
	uint8_t hw_id;
	uint8_t hw_version;
	uint8_t status;
	uint8_t error_id;
	uint16_t fw_app;
};

/* raw probe of one address, with nWAKE asserted and nRESET released */
static void probe_addr(uint8_t addr, struct probe *p)
{
	uint8_t fw[2] = { 0 };

	*p = (struct probe){ .addr = addr };

	if (!device_is_ready(i2c_bus)) {
		return;
	}

	if (nreset.port && gpio_is_ready_dt(&nreset)) {
		(void)gpio_pin_configure_dt(&nreset, GPIO_OUTPUT_INACTIVE);
	}
	if (wake.port && gpio_is_ready_dt(&wake)) {
		(void)gpio_pin_configure_dt(&wake, GPIO_OUTPUT_ACTIVE);
		k_usleep(100);	/* t_AWAKE 50 us */
	}

	p->ack = i2c_reg_read_byte(i2c_bus, addr, REG_HW_ID, &p->hw_id) == 0;
	if (p->ack) {
		(void)i2c_reg_read_byte(i2c_bus, addr, REG_HW_VERSION, &p->hw_version);
		(void)i2c_reg_read_byte(i2c_bus, addr, REG_STATUS, &p->status);
		(void)i2c_reg_read_byte(i2c_bus, addr, REG_ERROR_ID, &p->error_id);
		(void)i2c_burst_read(i2c_bus, addr, REG_FW_APP_VERSION, fw, sizeof(fw));
		p->fw_app = sys_get_be16(fw);
	}

	if (wake.port && gpio_is_ready_dt(&wake)) {
		(void)gpio_pin_set_dt(&wake, 0);
	}
}

static void log_probe(const struct probe *p)
{
	if (!p->ack) {
		LOG_WRN("I2C 0x%02x: no answer", p->addr);
		return;
	}

	LOG_INF("I2C 0x%02x: hw_id 0x%02x%s, hw_ver 0x%02x, status 0x%02x "
		"(app %s, %s%s), error_id 0x%02x, app fw 0x%04x",
		p->addr, p->hw_id, p->hw_id == CCS811_HW_ID ? " (CCS811)" : " (not a CCS811)",
		p->hw_version, p->status,
		(p->status & BIT(4)) ? "valid" : "MISSING",
		(p->status & BIT(7)) ? "running" : "boot mode",
		(p->status & BIT(0)) ? ", ERROR" : "",
		p->error_id, p->fw_app);
}

static void airq_init_work_fn(struct k_work *work)
{
	struct probe p[2];

	ARG_UNUSED(work);

	init_rc = device_init(ccs811);
	if (init_rc == -EALREADY) {
		init_rc = device_is_ready(ccs811) ? 0 : -ENODEV;
	}
	init_done = true;

	if (init_rc == 0) {
		LOG_INF("CCS811 ready at I2C 0x%02x", DT_REG_ADDR(CCS811_NODE));
		return;
	}

	LOG_ERR("CCS811 init failed (%d), I2C probe:", init_rc);
	probe_addr(CCS811_ADDR_LOW, &p[0]);
	probe_addr(CCS811_ADDR_HIGH, &p[1]);
	log_probe(&p[0]);
	log_probe(&p[1]);

	const struct probe *found = p[0].ack ? &p[0] : (p[1].ack ? &p[1] : NULL);

	if (found == NULL) {
		LOG_ERR("hint: nothing answers on I2C3 (PA8 SCL / PC9 SDA): click "
			"seated on mikroBUS 3? nWAKE (PE5), I2C pull-ups, power");
	} else if (found->addr != DT_REG_ADDR(CCS811_NODE)) {
		LOG_ERR("hint: CCS811 at 0x%02x, devicetree says 0x%02x: fix reg "
			"in boards/stm32f4_disco.overlay (or the ADDR jumper)",
			found->addr, DT_REG_ADDR(CCS811_NODE));
	} else if (found->hw_id != CCS811_HW_ID) {
		LOG_ERR("hint: device at 0x%02x is not a CCS811 (hw_id 0x%02x)",
			found->addr, found->hw_id);
	} else if (!(found->status & BIT(4))) {
		LOG_ERR("hint: CCS811 has no valid application firmware "
			"(status 0x%02x): it needs a firmware update", found->status);
	} else {
		LOG_ERR("hint: CCS811 answers, see the ccs811 driver error above");
	}
}

static K_WORK_DELAYABLE_DEFINE(airq_init_work, airq_init_work_fn);

static int airq_measure(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	struct sensor_value eco2, tvoc;
	uint16_t err = AIRQ_MGMT_ERR_OK;
	bool fresh = true;
	int rc;

	if (!init_done) {
		err = AIRQ_MGMT_ERR_INIT_PENDING;
	} else if (init_rc != 0 || !device_is_ready(ccs811)) {
		err = AIRQ_MGMT_ERR_NO_DEVICE;
		if (!no_device_logged) {
			LOG_WRN("measure: no CCS811 (init %d), see the boot log or "
				"'airq diag'", init_rc);
			no_device_logged = true;
		}
	} else {
		/* -EAGAIN: no new result, the driver still holds the last one */
		rc = sensor_sample_fetch(ccs811);
		if (rc == -EAGAIN) {
			fresh = false;
		} else if (rc != 0) {
			err = AIRQ_MGMT_ERR_IO;
			LOG_WRN("measure: I2C read failed (%d)", rc);
		}
	}

	if (err == AIRQ_MGMT_ERR_OK &&
	    (sensor_channel_get(ccs811, SENSOR_CHAN_CO2, &eco2) != 0 ||
	     sensor_channel_get(ccs811, SENSOR_CHAN_VOC, &tvoc) != 0)) {
		err = AIRQ_MGMT_ERR_IO;
	}

	if (err != AIRQ_MGMT_ERR_OK) {
		return smp_add_cmd_err(zse, AIRQ_MGMT_GROUP_ID, err) ?
		       MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
	}

	LOG_DBG("eCO2 %d ppm, TVOC %d ppb%s", eco2.val1, tvoc.val1,
		fresh ? "" : " (no new result)");

	return (zcbor_tstr_put_lit(zse, "eco2") &&
		zcbor_uint32_put(zse, eco2.val1) &&
		zcbor_tstr_put_lit(zse, "tvoc") &&
		zcbor_uint32_put(zse, tvoc.val1) &&
		zcbor_tstr_put_lit(zse, "fresh") &&
		zcbor_bool_put(zse, fresh)) ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static bool encode_probe(zcbor_state_t *zse, const struct probe *p)
{
	bool ok = zcbor_map_start_encode(zse, 7) &&
		  zcbor_tstr_put_lit(zse, "addr") && zcbor_uint32_put(zse, p->addr) &&
		  zcbor_tstr_put_lit(zse, "ack") && zcbor_bool_put(zse, p->ack);

	if (ok && p->ack) {
		ok = zcbor_tstr_put_lit(zse, "hw_id") && zcbor_uint32_put(zse, p->hw_id) &&
		     zcbor_tstr_put_lit(zse, "hw_version") &&
		     zcbor_uint32_put(zse, p->hw_version) &&
		     zcbor_tstr_put_lit(zse, "status") && zcbor_uint32_put(zse, p->status) &&
		     zcbor_tstr_put_lit(zse, "error_id") && zcbor_uint32_put(zse, p->error_id) &&
		     zcbor_tstr_put_lit(zse, "fw_app") && zcbor_uint32_put(zse, p->fw_app);
	}

	return ok && zcbor_map_end_encode(zse, 7);
}

static int airq_diag(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	struct probe p[2];
	const char *init = !init_done ? "pending" : (init_rc == 0 ? "ok" : "failed");
	bool ok;

	/* with a running driver, the probe only reads registers */
	probe_addr(CCS811_ADDR_LOW, &p[0]);
	probe_addr(CCS811_ADDR_HIGH, &p[1]);
	log_probe(&p[0]);
	log_probe(&p[1]);

	ok = zcbor_tstr_put_lit(zse, "init") &&
	     zcbor_tstr_put_term(zse, init, sizeof("pending")) &&
	     zcbor_tstr_put_lit(zse, "init_rc") && zcbor_int32_put(zse, init_rc) &&
	     zcbor_tstr_put_lit(zse, "addr") &&
	     zcbor_uint32_put(zse, DT_REG_ADDR(CCS811_NODE)) &&
	     zcbor_tstr_put_lit(zse, "probe") &&
	     zcbor_list_start_encode(zse, 2) &&
	     encode_probe(zse, &p[0]) && encode_probe(zse, &p[1]) &&
	     zcbor_list_end_encode(zse, 2);

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

#ifdef CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL
static int airq_mgmt_translate_error(uint16_t err)
{
	switch (err) {
	case AIRQ_MGMT_ERR_NO_DEVICE:
		return MGMT_ERR_ENOENT;
	case AIRQ_MGMT_ERR_IO:
		return MGMT_ERR_EBADSTATE;
	case AIRQ_MGMT_ERR_INIT_PENDING:
		return MGMT_ERR_EBUSY;
	default:
		return MGMT_ERR_EUNKNOWN;
	}
}
#endif

static const struct mgmt_handler airq_mgmt_handlers[] = {
	[AIRQ_MGMT_ID_MEASURE] = {
		.mh_read = airq_measure,
		.mh_write = NULL,
	},
	[AIRQ_MGMT_ID_DIAG] = {
		.mh_read = airq_diag,
		.mh_write = NULL,
	},
};

static struct mgmt_group airq_mgmt_group = {
	.mg_handlers = airq_mgmt_handlers,
	.mg_handlers_count = ARRAY_SIZE(airq_mgmt_handlers),
	.mg_group_id = AIRQ_MGMT_GROUP_ID,
#ifdef CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL
	.mg_translate_error = airq_mgmt_translate_error,
#endif
#ifdef CONFIG_MCUMGR_GRP_ENUM_DETAILS_NAME
	.mg_group_name = "airq",
#endif
};

static void airq_mgmt_register(void)
{
	mgmt_register_group(&airq_mgmt_group);
}

MCUMGR_HANDLER_DEFINE(airq_mgmt, airq_mgmt_register);

static int airq_start(void)
{
	k_work_schedule(&airq_init_work, K_MSEC(CONFIG_APP_AIRQ_INIT_DELAY_MS));
	return 0;
}

SYS_INIT(airq_start, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
