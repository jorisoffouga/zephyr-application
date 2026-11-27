/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Custom MCUmgr group IDs of the application, in one place so they do not
 * collide. User groups start at MGMT_GROUP_ID_PERUSER (64).
 *
 * Each group has its protocol in include/<name>_mgmt.h and its handlers
 * in src/<name>_mgmt.c. Group error codes: 0 OK, 1 UNKNOWN, then
 * group-specific (SMP v2 {"err": {"group": G, "rc": N}}).
 */

#ifndef APP_MGMT_H_
#define APP_MGMT_H_

#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>

#define APP_MGMT_GROUP_LED	(MGMT_GROUP_ID_PERUSER + 0)	/* 64 */
#define APP_MGMT_GROUP_BARGRAPH	(MGMT_GROUP_ID_PERUSER + 1)	/* 65 */
#define APP_MGMT_GROUP_AIRQ	(MGMT_GROUP_ID_PERUSER + 2)	/* 66 */
#define APP_MGMT_GROUP_PATTERN	(MGMT_GROUP_ID_PERUSER + 3)	/* 67 */
#define APP_MGMT_GROUP_ALCOHOL	(MGMT_GROUP_ID_PERUSER + 4)	/* 68 */
#define APP_MGMT_GROUP_EEPROM	(MGMT_GROUP_ID_PERUSER + 5)	/* 69 */

#endif /* APP_MGMT_H_ */
