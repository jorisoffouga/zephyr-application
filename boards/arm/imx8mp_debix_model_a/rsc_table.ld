/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Resource table for Linux remoteproc, kept even if unreferenced.
 *
 * Before starting the M7, Linux copies the table back into the TCM with a
 * plain memcpy(), but imx_rproc maps the TCM as device memory, where every
 * access must be aligned. arm64 memcpy() ends with a 16-byte store at the end
 * of the buffer: start the table on 16 bytes and pad its size to a multiple
 * of 16 (Linux copies the whole section), or the copy faults and Linux panics.
 */
SECTION_PROLOGUE(.resource_table,, SUBALIGN(16))
{
	KEEP(*(.resource_table*))
	. = ALIGN(16);
} GROUP_LINK_IN(ROMABLE_REGION)
