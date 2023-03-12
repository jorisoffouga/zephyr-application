/*
 * Copyright (c) 2020, STMICROLECTRONICS
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <device.h>
#include <devicetree.h>
#include <drivers/ipm.h>
#include <drivers/gpio.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openamp/open_amp.h>
#include <metal/device.h>
#include <resource_table.h>

#include <logging/log.h>

#include "cJSON.h"

LOG_MODULE_REGISTER(openamp_rsc_table, LOG_LEVEL_DBG);

/* The devicetree node identifier for the "led0" alias. */
#define LED0_NODE DT_ALIAS(led0)

#if DT_NODE_HAS_STATUS(LED0_NODE, okay)
#define LED0	DT_GPIO_LABEL(LED0_NODE, gpios)
#define PIN	DT_GPIO_PIN(LED0_NODE, gpios)
#define FLAGS	DT_GPIO_FLAGS(LED0_NODE, gpios)
#else
/* A build error here means your board isn't set up to blink an LED. */
#error "Unsupported board: led0 devicetree alias is not defined"
#define LED0	""
#define PIN	0
#define FLAGS	0
#endif

#define SHM_DEVICE_NAME	"shm"

#if !DT_HAS_CHOSEN(zephyr_ipc_shm)
#error "Sample requires definition of shared memory for rpmsg"
#endif

/* Constants derived from device tree */
#define SHM_NODE		DT_CHOSEN(zephyr_ipc_shm)
#define SHM_START_ADDR	DT_REG_ADDR(SHM_NODE)
#define SHM_SIZE		DT_REG_SIZE(SHM_NODE)

#define APP_TASK_STACK_SIZE (512)

/* Add 512 extra bytes for the TTY task stack for the "tx_buff" buffer. */
#define APP_TTY_TASK_STACK_SIZE (1024)

K_THREAD_STACK_DEFINE(thread_mng_stack, APP_TASK_STACK_SIZE);
K_THREAD_STACK_DEFINE(thread_tty_stack, APP_TTY_TASK_STACK_SIZE);

static struct k_thread thread_mng_data;
static struct k_thread thread_tty_data;

static const struct device *ipm_handle;

static metal_phys_addr_t shm_physmap = SHM_START_ADDR;

struct metal_device shm_device = {
	.name = SHM_DEVICE_NAME,
	.num_regions = 2,
	.regions = {
		{.virt = NULL}, /* shared memory */
		{.virt = NULL}, /* rsc_table memory */
	},
	.node = { NULL },
	.irq_num = 0,
	.irq_info = NULL
};

struct rpmsg_rcv_msg {
	void *data;
	size_t len;
};

static struct metal_io_region *shm_io;
static struct rpmsg_virtio_shm_pool shpool;

static struct metal_io_region *rsc_io;
static struct rpmsg_virtio_device rvdev;

static void *rsc_table;
static struct rpmsg_device *rpdev;

static struct rpmsg_endpoint tty_ept;
static struct rpmsg_rcv_msg tty_msg;

static K_SEM_DEFINE(data_sem, 0, 1);
static K_SEM_DEFINE(data_tty_sem, 0, 1);

static void platform_ipm_callback(const struct device *dev, void *context,
				  uint32_t id, volatile void *data)
{
	printk("%s: msg received from mb %d\n", __func__, id);
	k_sem_give(&data_sem);
}

static int rpmsg_recv_tty_callback(struct rpmsg_endpoint *ept, void *data,
				   size_t len, uint32_t src, void *priv)
{
	printk("%s: in \n", __func__);
	struct rpmsg_rcv_msg *tty_msg = priv;

	rpmsg_hold_rx_buffer(ept, data);
	tty_msg->data = data;
	tty_msg->len = len;
	k_sem_give(&data_tty_sem);
	return RPMSG_SUCCESS;
}

static void receive_message(unsigned char **msg, unsigned int *len)
{
	int status = k_sem_take(&data_sem, K_FOREVER);

	if (status == 0) {
		printk("%s: in \n", __func__);
		rproc_virtio_notified(rvdev.vdev, VRING1_ID);
	}
}

static void new_service_cb(struct rpmsg_device *rdev, const char *name,
			   uint32_t src)
{
	printk("%s: unexpected ns service receive for name %s\n",
		__func__, name);
}

int mailbox_notify(void *priv, uint32_t id)
{
	ARG_UNUSED(priv);

	printk("%s: msg received\n", __func__);
	ipm_send(ipm_handle, 0, id, NULL, 0);

	return 0;
}

int platform_init(void)
{
	void *rsc_tab_addr;
	int rsc_size;
	struct metal_device *device;
	struct metal_init_params metal_params = METAL_INIT_DEFAULTS;
	int status;

	status = metal_init(&metal_params);
	if (status) {
		printk("metal_init: failed: %d\n", status);
		return -1;
	}

	status = metal_register_generic_device(&shm_device);
	if (status) {
		printk("Couldn't register shared memory: %d\n", status);
		return -1;
	}

	status = metal_device_open("generic", SHM_DEVICE_NAME, &device);
	if (status) {
		printk("metal_device_open failed: %d\n", status);
		return -1;
	}

	/* declare shared memory region */
	metal_io_init(&device->regions[0], (void *)SHM_START_ADDR, &shm_physmap,
		      SHM_SIZE, -1, 0, NULL);

	shm_io = metal_device_io_region(device, 0);
	if (!shm_io) {
		printk("Failed to get shm_io region\n");
		return -1;
	}

	/* declare resource table region */
	rsc_table_get(&rsc_tab_addr, &rsc_size);
	rsc_table = (struct st_resource_table *)rsc_tab_addr;

	metal_io_init(&device->regions[1], rsc_table,
		      (metal_phys_addr_t *)rsc_table, rsc_size, -1, 0, NULL);

	rsc_io = metal_device_io_region(device, 1);
	if (!rsc_io) {
		printk("Failed to get rsc_io region\n");
		return -1;
	}

	/* setup IPM */
	ipm_handle = device_get_binding(CONFIG_OPENAMP_IPC_DEV_NAME);
	if (!ipm_handle) {
		printk("Failed to find ipm device\n");
		return -1;
	}

	ipm_register_callback(ipm_handle, platform_ipm_callback, NULL);

	status = ipm_set_enabled(ipm_handle, 1);
	if (status) {
		printk("ipm_set_enabled failed\n");
		return -1;
	}

	return 0;
}

static void cleanup_system(void)
{
	ipm_set_enabled(ipm_handle, 0);
	rpmsg_deinit_vdev(&rvdev);
	metal_finish();
}

struct  rpmsg_device *
platform_create_rpmsg_vdev(unsigned int vdev_index,
			   unsigned int role,
			   void (*rst_cb)(struct virtio_device *vdev),
			   rpmsg_ns_bind_cb ns_cb)
{
	struct fw_rsc_vdev_vring *vring_rsc;
	struct virtio_device *vdev;
	int ret;

	vdev = rproc_virtio_create_vdev(VIRTIO_DEV_SLAVE, VDEV_ID,
					rsc_table_to_vdev(rsc_table),
					rsc_io, NULL, mailbox_notify, NULL);

	if (!vdev) {
		printk("failed to create vdev\r\n");
		return NULL;
	}

	/* wait master rpmsg init completion */
	rproc_virtio_wait_remote_ready(vdev);

	vring_rsc = rsc_table_get_vring0(rsc_table);
	ret = rproc_virtio_init_vring(vdev, 0, vring_rsc->notifyid,
				      (void *)vring_rsc->da, rsc_io,
				      vring_rsc->num, vring_rsc->align);
	if (ret) {
		printk("failed to init vring 0\r\n");
		goto failed;
	}

	vring_rsc = rsc_table_get_vring1(rsc_table);
	ret = rproc_virtio_init_vring(vdev, 1, vring_rsc->notifyid,
				      (void *)vring_rsc->da, rsc_io,
				      vring_rsc->num, vring_rsc->align);
	if (ret) {
		printk("failed to init vring 1\r\n");
		goto failed;
	}

	rpmsg_virtio_init_shm_pool(&shpool, NULL, SHM_SIZE);
	ret =  rpmsg_init_vdev(&rvdev, vdev, ns_cb, shm_io, &shpool);

	if (ret) {
		printk("failed rpmsg_init_vdev\r\n");
		goto failed;
	}

	return rpmsg_virtio_get_rpmsg_device(&rvdev);

failed:
	rproc_virtio_remove_vdev(vdev);

	return NULL;
}

void app_rpmsg_tty(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);
	int ret = 0;
	int status_response = false;

	k_sem_take(&data_tty_sem,  K_FOREVER);

	printk("\r\nOpenAMP[remote] Linux tty responder started\r\n");

	const struct device * dev_led = device_get_binding(LED0);
	if (dev_led == NULL) {
		return;
	}

	ret = gpio_pin_configure(dev_led, PIN, GPIO_OUTPUT_ACTIVE | FLAGS);
	if (ret < 0) {
		return;
	}

	tty_ept.priv = &tty_msg;
	ret = rpmsg_create_ept(&tty_ept, rpdev, "rpmsg-tty",
			       RPMSG_ADDR_ANY, RPMSG_ADDR_ANY,
			       rpmsg_recv_tty_callback, NULL);

	while (tty_ept.addr !=  RPMSG_ADDR_ANY)
	{
		k_sem_take(&data_tty_sem,  K_FOREVER);
		if (tty_msg.len)
		{
			cJSON *json_data = cJSON_Parse(tty_msg.data);
			if(json_data == NULL)
			{
				const char *error_ptr = cJSON_GetErrorPtr();
				if (error_ptr != NULL)
				{
					printk("Error before: %s\n", error_ptr);
				}
				cJSON_Delete(json_data);
			}else
			{
				const cJSON *id = cJSON_GetObjectItemCaseSensitive(json_data, "id");
				const cJSON *data = cJSON_GetObjectItemCaseSensitive(json_data, "data");
				if(cJSON_IsString(id) && (id->valuestring != NULL))
				{
					if(strcmp(id->valuestring, "led") == 0)
					{
						if(cJSON_IsNumber(data))
						{
							gpio_pin_set(dev_led, PIN, data->valueint);
							status_response = true;
						}
					}
				}
				cJSON_Delete(json_data);
			}
			cJSON *json_response = cJSON_CreateObject();

			if (cJSON_AddBoolToObject(json_response, "status", status_response) == NULL)
			{
				printk("Failed to create reponse json");
			}else{
				const char* response = cJSON_Print(json_response);
				rpmsg_send(&tty_ept, response, strlen(response));
				rpmsg_release_rx_buffer(&tty_ept, tty_msg.data);
			}
			cJSON_Delete(json_response);
		}
		status_response = false;
		tty_msg.len = 0;
		tty_msg.data = NULL;
	}
	rpmsg_destroy_ept(&tty_ept);

	printk("OpenAMP Linux TTY responder ended\n");
}

void rpmsg_mng_task(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);
	unsigned char *msg;
	unsigned int len;
	int ret = 0;

	printk("\r\nOpenAMP[remote]  linux responder demo started\r\n");

	/* Initialize platform */
	ret = platform_init();
	if (ret) {
		printk("Failed to initialize platform\n");
		ret = -1;
		goto task_end;
	}

	rpdev = platform_create_rpmsg_vdev(0, VIRTIO_DEV_SLAVE, NULL,
					   new_service_cb);
	if (!rpdev) {
		printk("Failed to create rpmsg virtio device\n");
		ret = -1;
		goto task_end;
	}

	/* start the rpmsg clients */
	k_sem_give(&data_tty_sem);

	while (1) {
		receive_message(&msg, &len);
	}

task_end:
	cleanup_system();

	printk("OpenAMP demo ended\n");
}

void main(void)
{
	printk("Starting application threads!\n");
	k_thread_create(&thread_mng_data, thread_mng_stack, APP_TASK_STACK_SIZE,
			(k_thread_entry_t)rpmsg_mng_task,
			NULL, NULL, NULL, K_PRIO_COOP(8), 0, K_NO_WAIT);
	k_thread_create(&thread_tty_data, thread_tty_stack, APP_TTY_TASK_STACK_SIZE,
			(k_thread_entry_t)app_rpmsg_tty,
			NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);
}
