/*
 * Copyright (c) 2026, Joris Offouga
 * SPDX-License-Identifier: Apache-2.0
 *
 * m4io service: Linux drives the M4 IO devices over RPMsg. Announces an
 * "rpmsg-raw" channel (Linux rpmsg_char creates /dev/rpmsg<n>) and answers
 * each request (include/za/m4io_proto.h) with one reply. The devices come
 * from the io-* aliases of the warp7-io-board snippet; a missing one answers
 * M4IO_ERR_NODEV. Status goes to the console (WaRP7: UART2).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/drivers/rtc/mcp7940n.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <openamp/open_amp.h>
#include <metal/sys.h>
#include <metal/io.h>
#include <resource_table.h>
#include <addr_translation.h>

#include <za/drivers/mfd/pcf8591.h>
#include <za/m4io_proto.h>

#if !DT_HAS_CHOSEN(zephyr_ipc_shm) || !DT_HAS_CHOSEN(zephyr_ipc)
#error "Needs zephyr,ipc_shm (RPMsg shared memory) and zephyr,ipc (mailbox)"
#endif

#define SHM_NODE	DT_CHOSEN(zephyr_ipc_shm)
#define SHM_START_ADDR	DT_REG_ADDR(SHM_NODE)
#define SHM_SIZE	DT_REG_SIZE(SHM_NODE)

/* io-* alias of the warp7-io-board snippet, NULL when absent */
#define IO_DEV(alias)                                                          \
	COND_CODE_1(DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(alias)),                  \
		    (DEVICE_DT_GET(DT_ALIAS(alias))), (NULL))

#define IO_GPIO_COUNT                                                          \
	COND_CODE_1(DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(io_gpio)),                \
		    (DT_PROP(DT_ALIAS(io_gpio), ngpios)), (0))

#define DAC_CHANNEL	0

static const struct device *const ipm = DEVICE_DT_GET(DT_CHOSEN(zephyr_ipc));

static const struct device *const io_gpio = IO_DEV(io_gpio);
static const struct device *const io_adc = IO_DEV(io_adc);
static const struct device *const io_dac = IO_DEV(io_dac);
static const struct device *const io_rtc = IO_DEV(io_rtc);
static const struct device *const io_eeprom = IO_DEV(io_eeprom);
static const struct device *const io_temp = IO_DEV(io_temp);

static metal_phys_addr_t shm_physmap = SHM_START_ADDR;
static metal_phys_addr_t rsc_physmap;
static struct metal_io_region shm_io;
static struct metal_io_region rsc_io;

static struct rpmsg_virtio_device rvdev;
static struct rpmsg_endpoint ept;
static void *rsc_table;

static K_SEM_DEFINE(kick_sem, 0, 1);

/* Devices set up at startup, struct m4io_info.devices */
static uint16_t io_devices;

static uint8_t reply_buf[M4IO_MAX_MSG] __aligned(4);

static bool io_ready(const struct device *dev)
{
	return dev && device_is_ready(dev);
}

static int32_t m4io_status(int err)
{
	switch (err) {
	case 0:
		return M4IO_OK;
	case -EINVAL:
	case -ERANGE:
	case -EFBIG:
		return M4IO_ERR_ARG;
	case -ENODEV:
		return M4IO_ERR_NODEV;
	default:
		return M4IO_ERR_IO;
	}
}

/*
 * Request handlers: @req and @req_len the request payload, @rsp the reply
 * payload (M4IO_MAX_PAYLOAD bytes), @rsp_len set to its length. Return 0 or
 * a negative errno.
 */

static int cmd_info(const void *req, size_t req_len, void *rsp,
		    size_t *rsp_len)
{
	struct m4io_info *info = rsp;

	if (req_len) {
		return -EINVAL;
	}

	memset(info, 0, sizeof(*info));
	info->version = M4IO_VERSION;
	info->devices = io_devices;
	if (io_devices & M4IO_DEV_GPIO) {
		info->gpio_count = IO_GPIO_COUNT;
	}
	if (io_devices & M4IO_DEV_ADC) {
		info->adc_channels = PCF8591_ADC_CHANNELS;
		info->adc_vref_mv = adc_ref_internal(io_adc);
	}
	if (io_devices & M4IO_DEV_EEPROM) {
		info->eeprom_size = eeprom_get_size(io_eeprom);
	}
	*rsp_len = sizeof(*info);

	return 0;
}

static int cmd_gpio_config(const void *req, size_t req_len, void *rsp,
			   size_t *rsp_len)
{
	const struct m4io_gpio_config *cfg = req;
	gpio_flags_t flags = 0;

	if (!(io_devices & M4IO_DEV_GPIO)) {
		return -ENODEV;
	}
	if (req_len != sizeof(*cfg) || cfg->pin >= IO_GPIO_COUNT) {
		return -EINVAL;
	}

	if (cfg->flags & M4IO_GPIO_OUTPUT) {
		flags |= cfg->flags & M4IO_GPIO_INIT_HIGH ?
			 GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE;
	} else if (cfg->flags & M4IO_GPIO_INPUT) {
		flags |= GPIO_INPUT;
	} else {
		return -EINVAL;
	}
	if (cfg->flags & M4IO_GPIO_ACTIVE_LOW) {
		flags |= GPIO_ACTIVE_LOW;
	}
	if (cfg->flags & M4IO_GPIO_PULL_UP) {
		flags |= GPIO_PULL_UP;
	}

	return gpio_pin_configure(io_gpio, cfg->pin, flags);
}

static int cmd_gpio_set(const void *req, size_t req_len, void *rsp,
			size_t *rsp_len)
{
	const struct m4io_gpio *gpio = req;

	if (!(io_devices & M4IO_DEV_GPIO)) {
		return -ENODEV;
	}
	if (req_len != sizeof(*gpio) || gpio->pin >= IO_GPIO_COUNT) {
		return -EINVAL;
	}

	return gpio_pin_set(io_gpio, gpio->pin, gpio->value);
}

static int cmd_gpio_get(const void *req, size_t req_len, void *rsp,
			size_t *rsp_len)
{
	const struct m4io_gpio *gpio = req;
	struct m4io_gpio *out = rsp;
	int val;

	if (!(io_devices & M4IO_DEV_GPIO)) {
		return -ENODEV;
	}
	if (req_len != sizeof(*gpio) || gpio->pin >= IO_GPIO_COUNT) {
		return -EINVAL;
	}

	val = gpio_pin_get(io_gpio, gpio->pin);
	if (val < 0) {
		return val;
	}

	out->pin = gpio->pin;
	out->value = val;
	*rsp_len = sizeof(*out);

	return 0;
}

static int cmd_adc_read(const void *req, size_t req_len, void *rsp,
			size_t *rsp_len)
{
	const struct m4io_adc_req *areq = req;
	struct m4io_adc_rsp *out = rsp;
	uint16_t samples[PCF8591_ADC_CHANNELS];
	struct adc_sequence seq = {
		.buffer = samples,
		.buffer_size = sizeof(samples),
		.resolution = PCF8591_RESOLUTION,
	};
	uint32_t channels;
	size_t n = 0;
	int ret;

	if (!(io_devices & M4IO_DEV_ADC)) {
		return -ENODEV;
	}
	if (req_len != sizeof(*areq) || !areq->channels ||
	    areq->channels & ~BIT_MASK(PCF8591_ADC_CHANNELS)) {
		return -EINVAL;
	}

	seq.channels = areq->channels;
	ret = adc_read(io_adc, &seq);
	if (ret) {
		return ret;
	}

	memset(out, 0, sizeof(*out));
	out->channels = areq->channels;
	out->resolution = PCF8591_RESOLUTION;
	out->vref_mv = adc_ref_internal(io_adc);

	/* The driver stores the samples in channel order */
	channels = areq->channels;
	while (channels) {
		uint8_t ch = find_lsb_set(channels) - 1;

		out->raw[ch] = samples[n++];
		channels &= ~BIT(ch);
	}
	*rsp_len = sizeof(*out);

	return 0;
}

static int cmd_dac_write(const void *req, size_t req_len, void *rsp,
			 size_t *rsp_len)
{
	const struct m4io_dac *dac = req;

	if (!(io_devices & M4IO_DEV_DAC)) {
		return -ENODEV;
	}
	if (req_len != sizeof(*dac) || dac->channel != DAC_CHANNEL ||
	    dac->value > BIT_MASK(PCF8591_RESOLUTION)) {
		return -EINVAL;
	}

	return dac_write_value(io_dac, dac->channel, dac->value);
}

static int cmd_time_get(const void *req, size_t req_len, void *rsp,
			size_t *rsp_len)
{
	struct m4io_time *time = rsp;
	uint32_t seconds;
	int ret;

	if (!(io_devices & M4IO_DEV_RTC)) {
		return -ENODEV;
	}
	if (req_len) {
		return -EINVAL;
	}

	/* The MCP7940N counter value is the Unix time */
	ret = counter_get_value(io_rtc, &seconds);
	if (ret) {
		return ret;
	}

	time->seconds = seconds;
	*rsp_len = sizeof(*time);

	return 0;
}

static int cmd_time_set(const void *req, size_t req_len, void *rsp,
			size_t *rsp_len)
{
	const struct m4io_time *time = req;

	if (!(io_devices & M4IO_DEV_RTC)) {
		return -ENODEV;
	}
	if (req_len != sizeof(*time)) {
		return -EINVAL;
	}

	return mcp7940n_rtc_set_time(io_rtc, time->seconds);
}

static int eeprom_check(const struct m4io_eeprom *ee)
{
	size_t size = eeprom_get_size(io_eeprom);

	if (!ee->len || ee->len > M4IO_EEPROM_MAX_DATA ||
	    ee->offset >= size || ee->len > size - ee->offset) {
		return -EINVAL;
	}

	return 0;
}

static int cmd_eeprom_read(const void *req, size_t req_len, void *rsp,
			   size_t *rsp_len)
{
	const struct m4io_eeprom *ee = req;
	int ret;

	if (!(io_devices & M4IO_DEV_EEPROM)) {
		return -ENODEV;
	}
	if (req_len != sizeof(*ee)) {
		return -EINVAL;
	}

	ret = eeprom_check(ee);
	if (ret) {
		return ret;
	}

	ret = eeprom_read(io_eeprom, ee->offset, rsp, ee->len);
	if (ret) {
		return ret;
	}

	*rsp_len = ee->len;

	return 0;
}

static int cmd_eeprom_write(const void *req, size_t req_len, void *rsp,
			    size_t *rsp_len)
{
	const struct m4io_eeprom *ee = req;
	int ret;

	if (!(io_devices & M4IO_DEV_EEPROM)) {
		return -ENODEV;
	}
	if (req_len < sizeof(*ee) || req_len != sizeof(*ee) + ee->len) {
		return -EINVAL;
	}

	ret = eeprom_check(ee);
	if (ret) {
		return ret;
	}

	return eeprom_write(io_eeprom, ee->offset, ee->data, ee->len);
}

static int cmd_temp_get(const void *req, size_t req_len, void *rsp,
			size_t *rsp_len)
{
	struct m4io_temp *temp = rsp;
	struct sensor_value val;
	int ret;

	if (!(io_devices & M4IO_DEV_TEMP)) {
		return -ENODEV;
	}
	if (req_len) {
		return -EINVAL;
	}

	ret = sensor_sample_fetch(io_temp);
	if (!ret) {
		ret = sensor_channel_get(io_temp, SENSOR_CHAN_AMBIENT_TEMP,
					 &val);
	}
	if (ret) {
		return ret;
	}

	temp->millicelsius = sensor_value_to_milli(&val);
	*rsp_len = sizeof(*temp);

	return 0;
}

typedef int (*m4io_handler)(const void *req, size_t req_len, void *rsp,
			    size_t *rsp_len);

static const m4io_handler handlers[] = {
	[M4IO_CMD_INFO] = cmd_info,
	[M4IO_CMD_GPIO_CONFIG] = cmd_gpio_config,
	[M4IO_CMD_GPIO_SET] = cmd_gpio_set,
	[M4IO_CMD_GPIO_GET] = cmd_gpio_get,
	[M4IO_CMD_ADC_READ] = cmd_adc_read,
	[M4IO_CMD_DAC_WRITE] = cmd_dac_write,
	[M4IO_CMD_TIME_GET] = cmd_time_get,
	[M4IO_CMD_TIME_SET] = cmd_time_set,
	[M4IO_CMD_EEPROM_READ] = cmd_eeprom_read,
	[M4IO_CMD_EEPROM_WRITE] = cmd_eeprom_write,
	[M4IO_CMD_TEMP_GET] = cmd_temp_get,
};

/*
 * One request, one reply. The request is in the shared memory, writable by
 * Linux: every length and argument is checked before use.
 */
static int m4io_cb(struct rpmsg_endpoint *ep, void *data, size_t len,
		   uint32_t src, void *priv)
{
	struct m4io_hdr *rsp = (struct m4io_hdr *)reply_buf;
	struct m4io_hdr req;
	size_t rsp_len = 0;
	int ret;

	if (len < sizeof(req)) {
		printk("m4io: short message (%zu bytes)\n", len);
		return RPMSG_SUCCESS;
	}

	memcpy(&req, data, sizeof(req));

	rsp->cmd = req.cmd;
	rsp->seq = req.seq;

	if (req.len != len - sizeof(req)) {
		ret = -EINVAL;
		rsp->status = M4IO_ERR_ARG;
	} else if (req.cmd >= ARRAY_SIZE(handlers) || !handlers[req.cmd]) {
		ret = -ENOTSUP;
		rsp->status = M4IO_ERR_CMD;
	} else {
		ret = handlers[req.cmd]((uint8_t *)data + sizeof(req), req.len,
					rsp + 1, &rsp_len);
		rsp->status = m4io_status(ret);
	}

	if (ret) {
		rsp_len = 0;
		printk("m4io: cmd %u failed: %d\n", req.cmd, ret);
	}
	rsp->len = rsp_len;

	ret = rpmsg_sendto(ep, rsp, sizeof(*rsp) + rsp_len, src);
	if (ret < 0) {
		printk("m4io: send failed: %d\n", ret);
	}

	return RPMSG_SUCCESS;
}

static void unbind_cb(struct rpmsg_endpoint *ep)
{
	printk("m4io: channel closed by Linux\n");
}

static void io_init(void)
{
	if (io_ready(io_gpio)) {
		io_devices |= M4IO_DEV_GPIO;
	}

	if (io_ready(io_adc)) {
		struct adc_channel_cfg cfg = {
			.gain = ADC_GAIN_1,
			.reference = ADC_REF_INTERNAL,
			.acquisition_time = ADC_ACQ_TIME_DEFAULT,
		};
		int ret = 0;

		for (uint8_t ch = 0; ch < PCF8591_ADC_CHANNELS && !ret; ch++) {
			cfg.channel_id = ch;
			ret = adc_channel_setup(io_adc, &cfg);
		}
		if (!ret) {
			io_devices |= M4IO_DEV_ADC;
		}
	}

	if (io_ready(io_dac)) {
		const struct dac_channel_cfg cfg = {
			.channel_id = DAC_CHANNEL,
			.resolution = PCF8591_RESOLUTION,
		};

		if (!dac_channel_setup(io_dac, &cfg)) {
			io_devices |= M4IO_DEV_DAC;
		}
	}

	if (io_ready(io_rtc)) {
		io_devices |= M4IO_DEV_RTC;
	}
	if (io_ready(io_eeprom)) {
		io_devices |= M4IO_DEV_EEPROM;
	}
	if (io_ready(io_temp)) {
		io_devices |= M4IO_DEV_TEMP;
	}

	printk("m4io: devices 0x%02x\n", io_devices);
}

/* Linux kicked us through the mailbox */
static void ipm_callback(const struct device *dev, void *context, uint32_t id,
			 volatile void *data)
{
	k_sem_give(&kick_sem);
}

/*
 * Kick Linux. imx_rproc listens on one MU register (its "rx" channel) and
 * then processes every vring, so kick on that register when it is set.
 */
static int mailbox_notify(void *priv, uint32_t id)
{
	uint32_t reg = CONFIG_RPMSG_IO_MU_KICK_ID >= 0 ?
		       CONFIG_RPMSG_IO_MU_KICK_ID : id;

	ARG_UNUSED(priv);

	return ipm_send(ipm, 0, reg, &id, sizeof(id));
}

static int platform_init(void)
{
	struct metal_init_params params = METAL_INIT_DEFAULTS;
	int size;
	int ret;

	ret = metal_init(&params);
	if (ret) {
		return ret;
	}

	metal_io_init(&shm_io, (void *)SHM_START_ADDR, &shm_physmap, SHM_SIZE,
		      -1, 0, addr_translation_get_ops(shm_physmap));

	rsc_table_get(&rsc_table, &size);
	rsc_physmap = (uintptr_t)rsc_table;
	metal_io_init(&rsc_io, rsc_table, &rsc_physmap, size, -1, 0, NULL);

	if (!device_is_ready(ipm)) {
		return -ENODEV;
	}
	ipm_register_callback(ipm, ipm_callback, NULL);

	return ipm_set_enabled(ipm, 1);
}

static struct rpmsg_device *create_rpmsg_device(void)
{
	struct fw_rsc_vdev_vring *vring;
	struct virtio_device *vdev;

	vdev = rproc_virtio_create_vdev(VIRTIO_DEV_DEVICE, VDEV_ID,
					rsc_table_to_vdev(rsc_table), &rsc_io,
					NULL, mailbox_notify, NULL);
	if (!vdev) {
		return NULL;
	}

	/* Linux fills the vring addresses in the resource table */
	rproc_virtio_wait_remote_ready(vdev);

	vring = rsc_table_get_vring0(rsc_table);
	if (rproc_virtio_init_vring(vdev, 0, vring->notifyid, (void *)vring->da,
				    &rsc_io, vring->num, vring->align)) {
		goto err;
	}

	vring = rsc_table_get_vring1(rsc_table);
	if (rproc_virtio_init_vring(vdev, 1, vring->notifyid, (void *)vring->da,
				    &rsc_io, vring->num, vring->align)) {
		goto err;
	}

	if (rpmsg_init_vdev(&rvdev, vdev, NULL, &shm_io, NULL)) {
		goto err;
	}

	return rpmsg_virtio_get_rpmsg_device(&rvdev);
err:
	rproc_virtio_remove_vdev(vdev);
	return NULL;
}

int main(void)
{
	struct rpmsg_device *rpdev;
	int ret;

	printk("m4io: started, protocol %u\n", M4IO_VERSION);

	io_init();

	ret = platform_init();
	if (ret) {
		printk("m4io: platform init failed: %d\n", ret);
		return 0;
	}

	rpdev = create_rpmsg_device();
	if (!rpdev) {
		printk("m4io: cannot create the RPMsg device\n");
		return 0;
	}

	ret = rpmsg_create_ept(&ept, rpdev, M4IO_RPMSG_NAME, RPMSG_ADDR_ANY,
			       RPMSG_ADDR_ANY, m4io_cb, unbind_cb);
	if (ret) {
		printk("m4io: cannot create the endpoint: %d\n", ret);
		return 0;
	}

	printk("m4io: %s channel announced\n", M4IO_RPMSG_NAME);

	/* Process what Linux sends; m4io_cb runs from here */
	while (1) {
		k_sem_take(&kick_sem, K_FOREVER);
		rproc_virtio_notified(rvdev.vdev, VRING1_ID);
	}

	return 0;
}
