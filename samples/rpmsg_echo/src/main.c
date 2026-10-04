/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RPMsg echo for testing Linux remoteproc: announces an "rpmsg-tty" channel
 * (Linux creates /dev/ttyRPMSG<n>) and sends every message back unchanged.
 * Status goes to the console (WaRP7: UART2).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/sys/printk.h>

#include <openamp/open_amp.h>
#include <metal/sys.h>
#include <metal/io.h>
#include <resource_table.h>
#include <addr_translation.h>

#if !DT_HAS_CHOSEN(zephyr_ipc_shm) || !DT_HAS_CHOSEN(zephyr_ipc)
#error "Needs zephyr,ipc_shm (RPMsg shared memory) and zephyr,ipc (mailbox)"
#endif

#define SHM_NODE	DT_CHOSEN(zephyr_ipc_shm)
#define SHM_START_ADDR	DT_REG_ADDR(SHM_NODE)
#define SHM_SIZE	DT_REG_SIZE(SHM_NODE)

static const struct device *const ipm = DEVICE_DT_GET(DT_CHOSEN(zephyr_ipc));

static metal_phys_addr_t shm_physmap = SHM_START_ADDR;
static metal_phys_addr_t rsc_physmap;
static struct metal_io_region shm_io;
static struct metal_io_region rsc_io;

static struct rpmsg_virtio_device rvdev;
static struct rpmsg_endpoint ept;
static void *rsc_table;

static K_SEM_DEFINE(kick_sem, 0, 1);

/* Linux kicked us through the mailbox */
static void ipm_callback(const struct device *dev, void *context, uint32_t id,
			 volatile void *data)
{
	k_sem_give(&kick_sem);
}

/* Kick Linux: MU register id = vring id, as imx_rproc expects */
static int mailbox_notify(void *priv, uint32_t id)
{
	ARG_UNUSED(priv);

	return ipm_send(ipm, 0, id, &id, sizeof(id));
}

static int echo_cb(struct rpmsg_endpoint *ep, void *data, size_t len,
		   uint32_t src, void *priv)
{
	printk("echo: received %zu bytes from src %u\n", len, src);
	int ret = rpmsg_send(ep, data, len);

	if (ret < 0) {
		printk("echo: send failed: %d\n", ret);
	}

	return RPMSG_SUCCESS;
}

static void unbind_cb(struct rpmsg_endpoint *ep)
{
	printk("echo: channel closed by Linux\n");
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

	printk("rpmsg_echo: started\n");

	ret = platform_init();
	if (ret) {
		printk("rpmsg_echo: platform init failed: %d\n", ret);
		return 0;
	}

	rpdev = create_rpmsg_device();
	if (!rpdev) {
		printk("rpmsg_echo: cannot create the RPMsg device\n");
		return 0;
	}

	ret = rpmsg_create_ept(&ept, rpdev, "rpmsg-tty", RPMSG_ADDR_ANY,
			       RPMSG_ADDR_ANY, echo_cb, unbind_cb);
	if (ret) {
		printk("rpmsg_echo: cannot create the endpoint: %d\n", ret);
		return 0;
	}

	printk("rpmsg_echo: rpmsg-tty channel announced, echoing\n");

	/* Process what Linux sends; echo_cb runs from here */
	while (1) {
		k_sem_take(&kick_sem, K_FOREVER);
		rproc_virtio_notified(rvdev.vdev, VRING1_ID);
	}

	return 0;
}
