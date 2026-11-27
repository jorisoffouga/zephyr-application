/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Custom MCUmgr group "alcohol": Alcohol click (MIKROE-1586, MQ-3) on
 * mikroBUS 1. The click gives an analog voltage on the AN pin (PC1,
 * ADC1 IN11): it rises with the alcohol concentration.
 *
 * Group 68 (APP_MGMT_GROUP_ALCOHOL), command 0 "measure":
 *
 *   read   request  {}
 *          response {"raw": 812, "mv": 654, "full_mv": 3300,
 *                    "saturated": false, "warming": false,
 *                    "baseline_mv": 610, "delta_mv": 44}
 *
 *   "raw" ADC counts (12 bit, average of CONFIG_APP_ALCOHOL_SAMPLES),
 *   "mv" the AN voltage, "full_mv" the ADC full scale (VREF).
 *   "saturated": the voltage is at the full scale, the real one may be
 *   higher: lower the load resistor (potentiometer on the click), see
 *   the warning below.
 *   "warming": less than CONFIG_APP_ALCOHOL_WARMUP_S since boot, the
 *   heater output is not stable yet.
 *   "baseline_mv" / "delta_mv": only once a baseline is set (clean air
 *   voltage, RAM only): delta_mv = mv - baseline_mv, the alcohol signal.
 *
 *   write  request  {"calibrate": true}        baseline = the voltage now
 *                   {"baseline_mv": 610}       baseline set by hand
 *                   {"baseline_mv": 0}         no baseline
 *          response as the read
 *
 * The MQ-3 gives no ppm by itself: that needs its load resistor (the
 * potentiometer) and a calibration with a known concentration.
 *
 * WARNING: the click runs on 5 V and its AN output can go up to 5 V at
 * high concentration; the STM32 ADC input takes at most 3.3 V. Check
 * the AN voltage with a meter (and the potentiometer) before relying on
 * high readings, or add a divider.
 *
 * Errors: a group error.
 */

#ifndef ALCOHOL_MGMT_H_
#define ALCOHOL_MGMT_H_

#include "app_mgmt.h"

#define ALCOHOL_MGMT_GROUP_ID	APP_MGMT_GROUP_ALCOHOL

enum alcohol_mgmt_cmd {
	ALCOHOL_MGMT_ID_MEASURE = 0,
};

enum alcohol_mgmt_err {
	ALCOHOL_MGMT_ERR_OK = 0,
	ALCOHOL_MGMT_ERR_UNKNOWN,
	ALCOHOL_MGMT_ERR_NO_DEVICE,	/* ADC not ready or channel setup failed */
	ALCOHOL_MGMT_ERR_IO,		/* ADC read failed */
	ALCOHOL_MGMT_ERR_INVALID,	/* baseline_mv out of 0..full_mv */
};

#endif /* ALCOHOL_MGMT_H_ */
