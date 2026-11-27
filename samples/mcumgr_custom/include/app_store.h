/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings kept in the EEPROM click (src/app_store.c): the mode and blink
 * period of each LED and the pattern of each LED bank, restored at boot.
 *
 * Two copies of the record at CONFIG_APP_STORE_OFFSET (default 0x300, the
 * last 256-byte block of the 24C08), 128 bytes each, little endian:
 *
 *   0   u32  magic "MCst" (0x7473434d)
 *   4   u8   version (2)
 *   5   u8   number of LEDs per bank: board, expander, bargraph (3 bytes)
 *   8   u16  payload length
 *   10  u16  CRC-16/CCITT of bytes 12 .. 16 + length
 *   12  u32  sequence number: the valid copy with the highest one is used
 *   16  payload: per LED {u8 mode, u16 period_ms},
 *                per bank {u8 pattern, u16 step_ms, u8 width}
 *
 * A save writes the other copy and reads it back: a save cut by a reset
 * or a power loss leaves the previous copy valid.
 *
 * The record is written CONFIG_APP_STORE_DELAY_MS after the last change
 * made over SMP (autosave), and only if it differs from the stored one;
 * a pending save is written before an SMP reset (OS group reset hook).
 * A record of another LED layout (other click boards) is not restored.
 * LED 0 is not restored: it blinks the image identity (APP_BOOT_BLINK_MS).
 */

#ifndef APP_STORE_H_
#define APP_STORE_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

struct app_store_status {
	bool ready;		/* EEPROM device ready */
	bool valid;		/* a valid record is stored */
	bool restored;		/* it was applied at boot */
	bool autosave;
	bool pending;		/* a save is scheduled */
	uint8_t slot;		/* copy in use (0 or 1) */
	uint32_t seq;		/* its sequence number */
	uint32_t saves;		/* records written since boot */
	int last_rc;		/* result of the last EEPROM access, 0 or -errno */
	uint32_t offset;
	uint32_t size;
};

#ifdef CONFIG_APP_STORE

/** At boot, after led_ctrl_init(): applies the stored record. 0, -ENOENT, -errno. */
int app_store_restore(void);

/** An LED or a pattern was changed over SMP: save soon (autosave). */
void app_store_changed(void);

/** Writes the record now. */
int app_store_save(void);

/** Invalidates the record (the next boot starts with all LEDs off). */
int app_store_clear(void);

void app_store_set_autosave(bool on);

void app_store_get_status(struct app_store_status *st);

#else

static inline int app_store_restore(void) { return -ENOTSUP; }
static inline void app_store_changed(void) { }

#endif

#endif /* APP_STORE_H_ */
