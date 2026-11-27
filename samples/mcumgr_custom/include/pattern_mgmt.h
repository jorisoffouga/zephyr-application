/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Custom MCUmgr group "pattern": LED patterns run by the firmware
 * (include/pattern.h).
 *
 * Group 67 (APP_MGMT_GROUP_PATTERN), command 0 "state":
 *
 *   read   request  {}
 *          response {"patterns": ["off", "k2000", "chase", "fill",
 *                                 "alternate", "counter"],
 *                    "banks": [{"bank": "board", "leds": 4,
 *                               "pattern": "off", "step_ms": 0, "width": 0},
 *                              {"bank": "bargraph", "leds": 10,
 *                               "pattern": "k2000", "step_ms": 80,
 *                               "width": 2}, ...]}
 *
 *   write  request  {"bank": "board" | "expander" | "bargraph",
 *                    "pattern": "k2000", "step_ms": 80, "width": 2}
 *          response the bank entry, as in "banks"
 *
 *   "step_ms" 20..5000 (default 100), "width" 1..4 (default 2, k2000 and
 *   chase). "pattern": "off" stops it and switches the bank off. Setting
 *   an LED of the bank (led group, or the bargraph group for the
 *   bargraph) stops its pattern.
 *
 * Errors: MGMT_ERR_EINVAL for a malformed request, else a group error.
 */

#ifndef PATTERN_MGMT_H_
#define PATTERN_MGMT_H_

#include "app_mgmt.h"

#define PATTERN_MGMT_GROUP_ID	APP_MGMT_GROUP_PATTERN

enum pattern_mgmt_cmd {
	PATTERN_MGMT_ID_STATE = 0,
};

enum pattern_mgmt_err {
	PATTERN_MGMT_ERR_OK = 0,
	PATTERN_MGMT_ERR_UNKNOWN,
	PATTERN_MGMT_ERR_INVALID_BANK,		/* unknown name or no LED in it */
	PATTERN_MGMT_ERR_INVALID_PATTERN,
	PATTERN_MGMT_ERR_INVALID_STEP,		/* step_ms or width out of range */
};

#define PATTERN_DEFAULT_STEP_MS	100
#define PATTERN_DEFAULT_WIDTH	2

#endif /* PATTERN_MGMT_H_ */
