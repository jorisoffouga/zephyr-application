/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * MCUmgr over UART (USART2 on the MikroE shield FT232RL):
 * - image group (upload / test / confirm) and OS group (echo / reset):
 *   firmware update with MCUboot rollback
 * - custom groups (IDs in include/app_mgmt.h):
 *   - 64 "led" (src/led_mgmt.c): board, expander and bargraph LEDs
 *     on / off / blink, application version
 *   - 65 "bargraph" (src/bargraph_mgmt.c): BarGraph click level, mask,
 *     brightness
 *   - 66 "airq" (src/airq_mgmt.c): Air Quality 3 click eCO2 / TVOC
 *
 * At boot LED 0 blinks at CONFIG_APP_BOOT_BLINK_MS, so the running image
 * is visible. If MCUboot started this image in test mode, the image runs
 * its self-test (the board LEDs; a missing click board does not fail it)
 * and confirms itself (never, with
 * CONFIG_APP_SIMULATE_BAD_IMAGE: MCUboot then reverts it at the next reset).
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/logging/log.h>
#include <app_version.h>

#include "led_ctrl.h"

LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);

#ifdef CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION
#define IMAGE_VERSION CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION
#else
#define IMAGE_VERSION APP_VERSION_EXTENDED_STRING
#endif

int main(void)
{
	bool confirmed = boot_is_img_confirmed();
	int rc;

	LOG_INF("mcumgr_custom image %s, build %s, board %s", IMAGE_VERSION,
		STRINGIFY(APP_BUILD_VERSION), CONFIG_BOARD_TARGET);

	rc = led_ctrl_init();

	if (IS_ENABLED(CONFIG_APP_SIMULATE_BAD_IMAGE)) {
		LOG_WRN("simulated bad image: self-test fails");
		rc = -EIO;
	}

	if (confirmed) {
		LOG_INF("image confirmed");
	} else if (rc == 0) {
		rc = boot_write_img_confirmed();
		if (rc == 0) {
			LOG_INF("image in test: self-test ok, confirmed");
		} else {
			LOG_ERR("image in test: confirm failed (%d)", rc);
		}
	} else {
		LOG_ERR("image in test: self-test failed (%d), not confirmed: "
			"MCUboot reverts it at the next reset", rc);
	}

	(void)led_ctrl_set(0, LED_MODE_BLINK, CONFIG_APP_BOOT_BLINK_MS);

	/* everything else runs from the SMP work queue and the LED work queue */
	return 0;
}
