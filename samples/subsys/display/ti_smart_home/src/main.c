/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/display.h>
#include <lvgl.h>
#include <lvgl_zephyr.h>
#include <ti_smart_home.h>

#define LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app);

int main(void)
{
	const struct device *display_dev;
	int ret;

	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));	
	if (!device_is_ready(display_dev)) {
		LOG_ERR("Display device not ready, aborting");
		return 0;
	}

	lvgl_lock();
	// lv_display_get_default();
	// lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_90);
	/*
	 * Render draw tasks directly into the panel's native-shaped buffer via a
	 * per-task coordinate matrix, instead of rendering into a logical-shaped
	 * buffer and rotating the whole buffer in the flush callback afterward.
	 */
	// lv_display_set_matrix_rotation(lv_display_get_default(), true);
	ti_smart_home_init(NULL);
	// thermostat_init(NULL);
	// motor_control_init(NULL);
	// washing_machine_init(NULL);
#ifndef CONFIG_LV_Z_RUN_LVGL_ON_WORKQUEUE
	lv_timer_handler();
#endif
	lvgl_unlock();

	ret = display_blanking_off(display_dev);
	if (ret < 0 && ret != -ENOSYS) {
		LOG_ERR("Failed to turn blanking off (error %d)", ret);
		return 0;
	}

	while (1) {
#ifndef CONFIG_LV_Z_RUN_LVGL_ON_WORKQUEUE
		uint32_t sleep_ms;

		lvgl_lock();
		sleep_ms = lv_timer_handler();
		lvgl_unlock();

		k_msleep(MIN(sleep_ms, INT32_MAX));
#else
		/* LVGL is serviced by its dedicated workqueue; just idle here. */
		k_msleep(10);
#endif
	}

	return 0;
}
