/*
 * Copyright (c) 2026, Joris Offouga
 * SPDX-License-Identifier: Apache-2.0
 *
 * m4io: Linux controls the Cortex-M4 IO devices (GPIO, ADC, DAC, RTC,
 * EEPROM, temperature) over RPMsg. The M4 announces an "rpmsg-raw" channel,
 * bound by Linux rpmsg_char to /dev/rpmsg<n>: one write() is one request,
 * one read() its reply.
 *
 * Every message is a struct m4io_hdr followed by hdr.len payload bytes,
 * little-endian. The reply has the request cmd and seq, a status (enum
 * m4io_status) and the reply payload. Used by Zephyr (samples/rpmsg_io) and
 * by the Linux m4io tool (bdx-iot-external package/m4io): keep both copies
 * in sync and bump M4IO_VERSION on incompatible changes.
 */

#ifndef ZA_M4IO_PROTO_H_
#define ZA_M4IO_PROTO_H_

#include <stdint.h>

#define M4IO_RPMSG_NAME		"rpmsg-raw"
#define M4IO_VERSION		1

/* RPMsg buffer (512 bytes) less the RPMsg header (16 bytes) */
#define M4IO_MAX_MSG		496
#define M4IO_MAX_PAYLOAD	(M4IO_MAX_MSG - sizeof(struct m4io_hdr))
/* EEPROM bytes per request, with the offset and length in front */
#define M4IO_EEPROM_MAX_DATA	256

enum m4io_cmd {
	M4IO_CMD_INFO = 1,	/* -> struct m4io_info */
	M4IO_CMD_GPIO_CONFIG,	/* struct m4io_gpio_config */
	M4IO_CMD_GPIO_SET,	/* struct m4io_gpio */
	M4IO_CMD_GPIO_GET,	/* struct m4io_gpio (pin) -> struct m4io_gpio */
	M4IO_CMD_ADC_READ,	/* struct m4io_adc_req -> struct m4io_adc_rsp */
	M4IO_CMD_DAC_WRITE,	/* struct m4io_dac */
	M4IO_CMD_TIME_GET,	/* -> struct m4io_time */
	M4IO_CMD_TIME_SET,	/* struct m4io_time */
	M4IO_CMD_EEPROM_READ,	/* struct m4io_eeprom (no data) -> data */
	M4IO_CMD_EEPROM_WRITE,	/* struct m4io_eeprom + data */
	M4IO_CMD_TEMP_GET,	/* -> struct m4io_temp */
};

enum m4io_status {
	M4IO_OK = 0,
	M4IO_ERR_CMD = -1,	/* unknown command */
	M4IO_ERR_ARG = -2,	/* bad length or argument */
	M4IO_ERR_NODEV = -3,	/* device not present or not ready */
	M4IO_ERR_IO = -4,	/* driver error */
};

/* Devices present on the M4, struct m4io_info.devices */
#define M4IO_DEV_GPIO		(1U << 0)
#define M4IO_DEV_ADC		(1U << 1)
#define M4IO_DEV_DAC		(1U << 2)
#define M4IO_DEV_RTC		(1U << 3)
#define M4IO_DEV_EEPROM		(1U << 4)
#define M4IO_DEV_TEMP		(1U << 5)

struct m4io_hdr {
	uint8_t cmd;
	uint8_t seq;
	uint16_t len;
	int32_t status;
} __attribute__((packed));

struct m4io_info {
	uint16_t version;
	uint16_t devices;
	uint8_t gpio_count;
	uint8_t adc_channels;
	uint16_t adc_vref_mv;
	uint16_t eeprom_size;
} __attribute__((packed));

/* GPIO configuration flags */
#define M4IO_GPIO_INPUT		(1U << 0)
#define M4IO_GPIO_OUTPUT	(1U << 1)
#define M4IO_GPIO_INIT_HIGH	(1U << 2)	/* output, logical 1 */
#define M4IO_GPIO_ACTIVE_LOW	(1U << 3)
#define M4IO_GPIO_PULL_UP	(1U << 4)

struct m4io_gpio_config {
	uint8_t pin;
	uint8_t flags;
} __attribute__((packed));

struct m4io_gpio {
	uint8_t pin;
	uint8_t value;	/* logical */
} __attribute__((packed));

struct m4io_adc_req {
	uint8_t channels;	/* mask, bit n = AINn */
} __attribute__((packed));

struct m4io_adc_rsp {
	uint8_t channels;
	uint8_t resolution;
	uint16_t vref_mv;
	uint16_t raw[8];	/* raw[n] for AINn, 0 when not requested */
} __attribute__((packed));

struct m4io_dac {
	uint8_t channel;
	uint8_t pad;
	uint16_t value;
} __attribute__((packed));

struct m4io_time {
	uint32_t seconds;	/* Unix time, UTC */
} __attribute__((packed));

struct m4io_eeprom {
	uint16_t offset;
	uint16_t len;		/* <= M4IO_EEPROM_MAX_DATA */
	uint8_t data[];
} __attribute__((packed));

struct m4io_temp {
	int32_t millicelsius;
} __attribute__((packed));

#endif /* ZA_M4IO_PROTO_H_ */
