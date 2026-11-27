/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Custom MCUmgr group: Air Quality 3 click (CCS811). Protocol in
 * include/airq_mgmt.h.
 *
 * The sensor is driven directly over I2C (see sensor_start()), from a
 * delayed work item once the USB log is up, so a failure is visible in
 * the log with the I2C probe that explains it.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
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

static const struct device *const i2c_bus = DEVICE_DT_GET(DT_BUS(CCS811_NODE));
static const struct gpio_dt_spec wake = GPIO_DT_SPEC_GET_OR(CCS811_NODE, wake_gpios, {0});
static const struct gpio_dt_spec nreset = GPIO_DT_SPEC_GET_OR(CCS811_NODE, reset_gpios, {0});
static const uint16_t addr = DT_REG_ADDR(CCS811_NODE);

#define REG_MEAS_MODE		0x01
#define REG_ALG_RESULT_DATA	0x02
#define REG_APP_START		0xF4
#define STATUS_ERROR		BIT(0)
#define STATUS_DATA_READY	BIT(3)
#define STATUS_APP_VALID	BIT(4)
#define STATUS_FW_MODE		BIT(7)
#define MEAS_MODE_1S		0x10	/* constant power, one result per second */

#define XFER_TRIES		3
#define INIT_TRIES		3
#define REINIT_PERIOD_MS	5000
#define FAILS_BEFORE_REINIT	3

/* I2C and sensor state: SMP work queue (measure, diag) and system work queue (init) */
static K_MUTEX_DEFINE(lock);
static volatile bool init_done;
static int init_rc;
static const char *init_step = "";
static bool ready;
static int64_t last_init_ms;
static int fails;
static bool no_device_logged;
static uint16_t last_eco2, last_tvoc;

struct probe {
	uint8_t addr;
	bool ack;
	uint8_t hw_id;
	uint8_t hw_version;
	uint8_t status;
	uint8_t error_id;
	uint16_t fw_app;
};

/*
 * The CCS811 is driven here, not with the Zephyr ccs811 driver: on this
 * board the driver init always failed on its last status read (I2C
 * timeout right after the MEAS_MODE write) and a failed device_init()
 * cannot be retried. Each transfer is retried, with pauses after the
 * commands, and the whole start is retried.
 */
static int reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
	int rc = -EIO;

	for (int i = 0; i < XFER_TRIES; i++) {
		rc = i2c_burst_read(i2c_bus, addr, reg, buf, len);
		if (rc == 0) {
			break;
		}
		k_msleep(5);
	}
	return rc;
}

static int reg_write(uint8_t reg, const uint8_t *data, size_t len)
{
	uint8_t msg[1 + 4];
	int rc = -EIO;

	msg[0] = reg;
	if (len > 0) {
		memcpy(&msg[1], data, len);
	}
	for (int i = 0; i < XFER_TRIES; i++) {
		rc = i2c_write(i2c_bus, msg, 1 + len, addr);
		if (rc == 0) {
			break;
		}
		k_msleep(5);
	}
	return rc;
}

static void pins_on(void)
{
	/* nWAKE stays asserted: the sensor always listens on I2C */
	if (wake.port && gpio_is_ready_dt(&wake)) {
		(void)gpio_pin_configure_dt(&wake, GPIO_OUTPUT_ACTIVE);
		k_usleep(100);	/* t_AWAKE 50 us */
	}
	if (nreset.port && gpio_is_ready_dt(&nreset)) {
		(void)gpio_pin_configure_dt(&nreset, GPIO_OUTPUT_INACTIVE);
	}
}

/* one start: reset, application mode, measurement mode. 0 or -errno, *step set */
static int sensor_start(const char **step)
{
	uint8_t v, mode = MEAS_MODE_1S;
	uint8_t fw[2];
	int rc;

	pins_on();
	if (nreset.port && gpio_is_ready_dt(&nreset)) {
		*step = "reset";
		(void)gpio_pin_set_dt(&nreset, 1);
		k_msleep(1);		/* t_RESET 15 us */
		(void)gpio_pin_set_dt(&nreset, 0);
	}
	k_msleep(20);			/* t_START */

	*step = "hw_id";
	rc = reg_read(REG_HW_ID, &v, 1);
	if (rc != 0) {
		return rc;
	}
	if (v != CCS811_HW_ID) {
		return -ENODEV;
	}

	*step = "status";
	rc = reg_read(REG_STATUS, &v, 1);
	if (rc != 0) {
		return rc;
	}
	if (!(v & STATUS_APP_VALID)) {
		*step = "app_valid";
		return -ENOEXEC;
	}

	if (!(v & STATUS_FW_MODE)) {
		*step = "app_start";
		rc = reg_write(REG_APP_START, NULL, 0);
		if (rc != 0) {
			return rc;
		}
		k_msleep(10);		/* t_APP_START 1 ms */
		rc = reg_read(REG_STATUS, &v, 1);
		if (rc != 0) {
			return rc;
		}
		if (!(v & STATUS_FW_MODE)) {
			return -EIO;
		}
	}

	*step = "meas_mode";
	rc = reg_write(REG_MEAS_MODE, &mode, 1);
	if (rc != 0) {
		return rc;
	}
	k_msleep(10);

	*step = "check";
	rc = reg_read(REG_STATUS, &v, 1);
	if (rc != 0) {
		return rc;
	}
	if (v & STATUS_ERROR) {
		(void)reg_read(REG_ERROR_ID, &v, 1);
		LOG_WRN("CCS811 error 0x%02x after MEAS_MODE", v);
		return -EIO;
	}

	if (reg_read(REG_FW_APP_VERSION, fw, sizeof(fw)) == 0) {
		LOG_INF("CCS811 app firmware %u.%u.%u", fw[0] >> 4, fw[0] & 0xf, fw[1]);
	}
	*step = "";
	return 0;
}

/* lock held */
static int sensor_init_locked(int tries)
{
	int rc = -EIO;

	for (int i = 0; i < tries; i++) {
		rc = sensor_start(&init_step);
		if (rc == 0) {
			break;
		}
		LOG_WRN("CCS811 start failed at '%s' (%d), try %d of %d", init_step, rc,
			i + 1, tries);
		k_msleep(100);
	}
	init_rc = rc;
	ready = rc == 0;
	fails = 0;
	last_init_ms = k_uptime_get();
	if (ready) {
		no_device_logged = false;
		LOG_INF("CCS811 ready at I2C 0x%02x, one result per second", addr);
	}
	return rc;
}

/* raw probe of one address, with nWAKE asserted and nRESET released */
static void probe_addr(uint8_t a, struct probe *p)
{
	uint8_t fw[2] = { 0 };

	*p = (struct probe){ .addr = a };

	if (!device_is_ready(i2c_bus)) {
		return;
	}
	pins_on();

	p->ack = i2c_reg_read_byte(i2c_bus, a, REG_HW_ID, &p->hw_id) == 0;
	if (p->ack) {
		(void)i2c_reg_read_byte(i2c_bus, a, REG_HW_VERSION, &p->hw_version);
		(void)i2c_reg_read_byte(i2c_bus, a, REG_STATUS, &p->status);
		(void)i2c_reg_read_byte(i2c_bus, a, REG_ERROR_ID, &p->error_id);
		(void)i2c_burst_read(i2c_bus, a, REG_FW_APP_VERSION, fw, sizeof(fw));
		p->fw_app = sys_get_be16(fw);
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
		(p->status & STATUS_APP_VALID) ? "valid" : "MISSING",
		(p->status & STATUS_FW_MODE) ? "running" : "boot mode",
		(p->status & STATUS_ERROR) ? ", ERROR" : "",
		p->error_id, p->fw_app);
}

static void airq_init_work_fn(struct k_work *work)
{
	struct probe p[2];
	int rc;

	ARG_UNUSED(work);

	if (!device_is_ready(i2c_bus)) {
		LOG_ERR("CCS811: I2C bus not ready");
		init_rc = -ENODEV;
		init_done = true;
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);
	rc = sensor_init_locked(INIT_TRIES);
	init_done = true;
	if (rc != 0) {
		probe_addr(CCS811_ADDR_LOW, &p[0]);
		probe_addr(CCS811_ADDR_HIGH, &p[1]);
	}
	k_mutex_unlock(&lock);

	if (rc == 0) {
		return;
	}

	LOG_ERR("CCS811 start failed at '%s' (%d), I2C probe:", init_step, rc);
	log_probe(&p[0]);
	log_probe(&p[1]);

	const struct probe *found = p[0].ack ? &p[0] : (p[1].ack ? &p[1] : NULL);

	if (found == NULL) {
		LOG_ERR("hint: nothing answers on I2C3 (PA8 SCL / PC9 SDA): click "
			"seated on mikroBUS 3? nWAKE (PE5), I2C pull-ups, power");
	} else if (found->addr != addr) {
		LOG_ERR("hint: CCS811 at 0x%02x, devicetree says 0x%02x: fix reg "
			"in boards/stm32f4_disco.overlay (or the ADDR jumper)",
			found->addr, addr);
	} else if (found->hw_id != CCS811_HW_ID) {
		LOG_ERR("hint: device at 0x%02x is not a CCS811 (hw_id 0x%02x)",
			found->addr, found->hw_id);
	} else if (!(found->status & STATUS_APP_VALID)) {
		LOG_ERR("hint: CCS811 has no valid application firmware "
			"(status 0x%02x): it needs a firmware update", found->status);
	} else {
		LOG_ERR("hint: CCS811 answers but did not start: the measure "
			"command retries every %d s", REINIT_PERIOD_MS / 1000);
	}
}

static K_WORK_DELAYABLE_DEFINE(airq_init_work, airq_init_work_fn);

static int airq_measure(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	uint16_t err = AIRQ_MGMT_ERR_OK;
	bool fresh = false;
	uint8_t st, data[4];
	int rc;

	if (!init_done) {
		return smp_add_cmd_err(zse, AIRQ_MGMT_GROUP_ID, AIRQ_MGMT_ERR_INIT_PENDING) ?
		       MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
	}

	k_mutex_lock(&lock, K_FOREVER);

	/* not started (yet), or lost: start again, at most every 5 s */
	if (!ready && k_uptime_get() - last_init_ms >= REINIT_PERIOD_MS) {
		(void)sensor_init_locked(1);
	}

	if (!ready) {
		err = AIRQ_MGMT_ERR_NO_DEVICE;
		if (!no_device_logged) {
			LOG_WRN("measure: CCS811 not started ('%s', %d), see the boot "
				"log or 'airq --diag'", init_step, init_rc);
			no_device_logged = true;
		}
	} else {
		rc = reg_read(REG_STATUS, &st, 1);
		if (rc == 0 && (st & STATUS_ERROR)) {
			(void)reg_read(REG_ERROR_ID, data, 1);
			LOG_WRN("measure: CCS811 error 0x%02x", data[0]);
			rc = -EIO;
		}
		if (rc == 0 && (st & STATUS_DATA_READY)) {
			rc = reg_read(REG_ALG_RESULT_DATA, data, sizeof(data));
			if (rc == 0) {
				last_eco2 = sys_get_be16(&data[0]);
				last_tvoc = sys_get_be16(&data[2]);
				fresh = true;
			}
		}
		if (rc != 0) {
			err = AIRQ_MGMT_ERR_IO;
			LOG_WRN("measure: I2C read failed (%d)", rc);
			if (++fails >= FAILS_BEFORE_REINIT) {
				LOG_WRN("measure: CCS811 lost, starting it again");
				ready = false;
				last_init_ms = 0;
			}
		} else {
			fails = 0;
		}
	}

	k_mutex_unlock(&lock);

	if (err != AIRQ_MGMT_ERR_OK) {
		return smp_add_cmd_err(zse, AIRQ_MGMT_GROUP_ID, err) ?
		       MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
	}

	LOG_DBG("eCO2 %u ppm, TVOC %u ppb%s", last_eco2, last_tvoc,
		fresh ? "" : " (no new result)");

	return (zcbor_tstr_put_lit(zse, "eco2") &&
		zcbor_uint32_put(zse, last_eco2) &&
		zcbor_tstr_put_lit(zse, "tvoc") &&
		zcbor_uint32_put(zse, last_tvoc) &&
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
	const char *init = !init_done ? "pending" : (ready ? "ok" : "failed");
	bool ok;

	/* the probe only reads registers: it does not disturb a running sensor */
	k_mutex_lock(&lock, K_FOREVER);
	probe_addr(CCS811_ADDR_LOW, &p[0]);
	probe_addr(CCS811_ADDR_HIGH, &p[1]);
	k_mutex_unlock(&lock);
	log_probe(&p[0]);
	log_probe(&p[1]);

	ok = zcbor_tstr_put_lit(zse, "init") &&
	     zcbor_tstr_put_term(zse, init, sizeof("pending")) &&
	     zcbor_tstr_put_lit(zse, "init_rc") && zcbor_int32_put(zse, ready ? 0 : init_rc) &&
	     zcbor_tstr_put_lit(zse, "step") &&
	     zcbor_tstr_put_term(zse, ready ? "" : init_step, sizeof("meas_mode")) &&
	     zcbor_tstr_put_lit(zse, "addr") && zcbor_uint32_put(zse, addr) &&
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
