/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Custom MCUmgr group "led": the protocol shared by the firmware and the
 * host client (scripts/smp_client.py).
 *
 * Group 64 (APP_MGMT_GROUP_LED), command 0 "state":
 *
 *   read   request  {}
 *          response {"leds": [{"led": 0, "name": "User LD4", "mode": "blink",
 *                              "period_ms": 500},
 *                             {"led": 4, "name": "exp0", "mode": "off",
 *                              "period_ms": 0}, ...]}
 *
 *   write  request  {"led": 0, "mode": "on" | "off" | "blink",
 *                    "period_ms": 500}
 *          response {"led": 0, "name": "User LD4", "mode": "blink",
 *                    "period_ms": 500}
 *
 *   All the LEDs of the board and the click boards, see led_ctrl.h for the
 *   indexes. "name" is the devicetree label. "led" defaults to 0. "period_ms" is the toggle interval (the LED is
 *   on for period_ms then off for period_ms), only used and required for
 *   "blink", LED_PERIOD_MIN_MS..LED_PERIOD_MAX_MS.
 *
 * Command 1 "version" (read only):
 *
 *   read   request  {}
 *          response {"protocol": 2, "image": "2.0.0+0",
 *                    "build": "fafb169561d8", "leds": 22}
 *
 *   "protocol" is LED_MGMT_PROTOCOL_VERSION, raised when a command is added
 *   or changed: a client checks it before using a newer command.
 *   "image" is the version signed in the MCUboot header (the same as in
 *   the image group state), "build" the git describe of the application.
 *
 * Errors: MGMT_ERR_EINVAL for a malformed request (missing "mode", wrong
 * CBOR types); a group error for a well-formed but wrong request:
 *   SMP v2 (version 1 in the header): {"err": {"group": 64, "rc": <led_mgmt_err>}}
 *   SMP v1: {"rc": <MGMT_ERR_*>} (see led_mgmt_translate_error)
 */

#ifndef LED_MGMT_H_
#define LED_MGMT_H_

#include "app_mgmt.h"

#define LED_MGMT_GROUP_ID	APP_MGMT_GROUP_LED

/*
 * 1: "state" (read, write) and "version" (read)
 * 2: "name" in the LED state; expander and bargraph LEDs
 */
#define LED_MGMT_PROTOCOL_VERSION	2

enum led_mgmt_cmd {
	LED_MGMT_ID_STATE = 0,
	LED_MGMT_ID_VERSION = 1,
};

enum led_mgmt_err {
	LED_MGMT_ERR_OK = 0,
	LED_MGMT_ERR_UNKNOWN,
	LED_MGMT_ERR_INVALID_LED,	/* no LED with this index */
	LED_MGMT_ERR_INVALID_MODE,	/* not "on", "off" or "blink" */
	LED_MGMT_ERR_INVALID_PERIOD,	/* blink period missing or out of range */
	LED_MGMT_ERR_GPIO,		/* GPIO not ready (click board missing?) or write failed */
};

#define LED_PERIOD_MIN_MS	20
#define LED_PERIOD_MAX_MS	10000

#endif /* LED_MGMT_H_ */
