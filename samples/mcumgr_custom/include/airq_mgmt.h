/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Custom MCUmgr group "airq": Air Quality 3 click (CCS811) on mikroBUS 3
 * (I2C3).
 *
 * Group 66 (APP_MGMT_GROUP_AIRQ), command 0 "measure" (read only):
 *
 *   read   request  {}
 *          response {"eco2": 412, "tvoc": 3, "fresh": true}
 *
 *   "eco2" equivalent CO2 in ppm (400..8192), "tvoc" total volatile
 *   organic compounds in ppb (0..1187). The CCS811 measures every second
 *   (CONFIG_CCS811_DRIVE_MODE_1); "fresh" is false when no new result
 *   came since the previous read (the values are then the previous
 *   ones). Right after power on it answers eco2 0 / 400 for about 20 s;
 *   a new sensor needs a 48 h burn-in for stable values.
 *
 * Command 1 "diag" (read only): driver state and a raw I2C probe of both
 * CCS811 addresses, with nWAKE asserted and nRESET released:
 *
 *   read   request  {}
 *          response {"init": "ok" | "pending" | "failed", "init_rc": -5,
 *                    "addr": 90,
 *                    "probe": [{"addr": 90, "ack": true, "hw_id": 129,
 *                               "hw_version": 18, "status": 152,
 *                               "error_id": 0, "fw_app": 4352},
 *                              {"addr": 91, "ack": false}]}
 *
 *   "addr" is the devicetree address. A CCS811 answers hw_id 0x81;
 *   status bit 4 (0x10) APP_VALID: application firmware present,
 *   bit 7 (0x80) FW_MODE: application running, bit 0 ERROR: see error_id.
 *
 * The driver starts CONFIG_APP_AIRQ_INIT_DELAY_MS after boot
 * (zephyr,deferred-init), so its result reaches the USB log.
 *
 * Errors: a group error.
 */

#ifndef AIRQ_MGMT_H_
#define AIRQ_MGMT_H_

#include "app_mgmt.h"

#define AIRQ_MGMT_GROUP_ID	APP_MGMT_GROUP_AIRQ

enum airq_mgmt_cmd {
	AIRQ_MGMT_ID_MEASURE = 0,
	AIRQ_MGMT_ID_DIAG = 1,
};

enum airq_mgmt_err {
	AIRQ_MGMT_ERR_OK = 0,
	AIRQ_MGMT_ERR_UNKNOWN,
	AIRQ_MGMT_ERR_NO_DEVICE,	/* driver init failed: see "diag" and the log */
	AIRQ_MGMT_ERR_IO,		/* I2C read failed */
	AIRQ_MGMT_ERR_INIT_PENDING,	/* driver not started yet (first seconds) */
};

#endif /* AIRQ_MGMT_H_ */
