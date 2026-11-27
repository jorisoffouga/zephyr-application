/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Custom MCUmgr group "bargraph": BarGraph click (MIKROE-1423) on
 * mikroBUS 4, 10 segments on two 74HC595, brightness by PWM.
 *
 * Group 65 (APP_MGMT_GROUP_BARGRAPH), command 0 "state":
 *
 *   read   request  {}
 *          response {"mask": 7, "segments": 10, "brightness": 100}
 *
 *   write  request  {"level": 0..segments}       segments 0..level-1 on
 *                or {"mask": 0..(1 << segments) - 1}
 *                and/or {"brightness": 0..100}  percent
 *          response as read
 *
 *   A write needs at least one key; "level" and "mask" together are
 *   rejected. Segment n is bit n of "mask" and LED bar<n> of the "led"
 *   group: segments set here show there (and can blink from there; a
 *   blinking segment counts as on in "mask").
 *   The brightness applies to all segments; it is not saved (100 after
 *   a reset).
 *
 * Errors: MGMT_ERR_EINVAL for a malformed request, else a group error.
 */

#ifndef BARGRAPH_MGMT_H_
#define BARGRAPH_MGMT_H_

#include "app_mgmt.h"

#define BARGRAPH_MGMT_GROUP_ID	APP_MGMT_GROUP_BARGRAPH

enum bargraph_mgmt_cmd {
	BARGRAPH_MGMT_ID_STATE = 0,
};

enum bargraph_mgmt_err {
	BARGRAPH_MGMT_ERR_OK = 0,
	BARGRAPH_MGMT_ERR_UNKNOWN,
	BARGRAPH_MGMT_ERR_INVALID_LEVEL,	/* level > segments */
	BARGRAPH_MGMT_ERR_INVALID_MASK,		/* bit above the last segment */
	BARGRAPH_MGMT_ERR_INVALID_BRIGHTNESS,	/* > 100 */
	BARGRAPH_MGMT_ERR_NO_DEVICE,		/* 74HC595 or PWM not ready */
	BARGRAPH_MGMT_ERR_IO,			/* SPI or PWM write failed */
};

#endif /* BARGRAPH_MGMT_H_ */
