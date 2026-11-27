/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Custom MCUmgr group "eeprom": EEPROM click (ST 24C08, 1 KiB) on
 * mikroBUS 1, and the LED settings kept in it (include/app_store.h).
 *
 * Group 69 (APP_MGMT_GROUP_EEPROM):
 *
 * Command 0 "info" (read):
 *   response {"size": 1024, "page": 16, "chip": "24C08",
 *             "store_off": 768, "store_size": 256}
 *
 * Command 1 "data":
 *   read   request  {"off": 0, "len": 64}            len 1..256
 *          response {"off": 0, "data": h'...'}
 *   write  request  {"off": 16, "data": h'48656c6c6f'} 1..256 bytes
 *          response {"off": 16, "len": 5}
 *   A write is read back and compared (IO error if it differs: WP?).
 *   Writing into the store area (store_off..) breaks the stored record:
 *   the next boot does not restore it, the next save rewrites it.
 *
 * Command 2 "erase" (write): fills with 0xff
 *   request  {"off": 0, "len": 1024}   (both optional: the whole EEPROM)
 *   response {"off": 0, "len": 1024}
 *
 * Command 3 "store": the LED settings record
 *   read   response {"ready": true, "valid": true, "restored": true,
 *                    "autosave": true, "pending": false, "slot": 1,
 *                    "seq": 7, "saves": 3,
 *                    "last_rc": 0}
 *   write  request  {"action": "save" | "restore" | "clear"}
 *                   {"autosave": false}
 *          response as the read
 *
 * Errors: a group error.
 */

#ifndef EEPROM_MGMT_H_
#define EEPROM_MGMT_H_

#include "app_mgmt.h"

#define EEPROM_MGMT_GROUP_ID	APP_MGMT_GROUP_EEPROM

#define EEPROM_MGMT_CHUNK_MAX	256

enum eeprom_mgmt_cmd {
	EEPROM_MGMT_ID_INFO = 0,
	EEPROM_MGMT_ID_DATA = 1,
	EEPROM_MGMT_ID_ERASE = 2,
	EEPROM_MGMT_ID_STORE = 3,
};

enum eeprom_mgmt_err {
	EEPROM_MGMT_ERR_OK = 0,
	EEPROM_MGMT_ERR_UNKNOWN,
	EEPROM_MGMT_ERR_NO_DEVICE,	/* EEPROM not ready (click missing?) */
	EEPROM_MGMT_ERR_IO,		/* I2C error, or written data reads back different */
	EEPROM_MGMT_ERR_INVALID_RANGE,	/* off / len outside the EEPROM, or len > 256 */
	EEPROM_MGMT_ERR_INVALID_ACTION,	/* store: unknown action */
	EEPROM_MGMT_ERR_NO_RECORD,	/* store restore: no valid record */
	EEPROM_MGMT_ERR_NO_STORE,	/* store: CONFIG_APP_STORE is off */
};

#endif /* EEPROM_MGMT_H_ */
