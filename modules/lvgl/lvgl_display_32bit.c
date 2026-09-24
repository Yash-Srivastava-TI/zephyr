/*
 * Copyright (c) 2019 Jan Van Winkel <jan.van_winkel@dxplore.eu>
 * Copyright 2023 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <lvgl.h>
#include "lvgl_display.h"

void lvgl_flush_cb_32bit(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
	/*
	 * CONFIG_LV_Z_FULL_REFRESH sets LV_DISPLAY_RENDER_MODE_DIRECT (lvgl.c),
	 * not LV_DISPLAY_RENDER_MODE_FULL. In DIRECT mode `px_map` is always the
	 * base pointer of the whole screen-sized buffer (LVGL keeps both VDBs
	 * fully in sync automatically) - only `area` may shrink to the actual
	 * dirty sub-rectangle (e.g. a scrolled strip). Using area's w/h here
	 * would rotate/flush the wrong sub-block of px_map for any non-full-
	 * screen dirty area, corrupting exactly the region that changed. So
	 * always treat px_map as the complete logical-resolution buffer.
	 */
	uint16_t w = (uint16_t)lv_display_get_horizontal_resolution(display);
	uint16_t h = (uint16_t)lv_display_get_vertical_resolution(display);
	struct lvgl_disp_data *data = (struct lvgl_disp_data *)lv_display_get_user_data(display);
	struct lvgl_display_flush flush;

	ARG_UNUSED(area);

	/*
	 * LVGL owns all buffers — the driver-framebuffer path is commented out.
	 *
	 * OLD PATH (driver-owned framebuffer):
	 * With display_get_framebuffer() returning a driver-allocated buffer,
	 * LVGL would blit each dirty region into that buffer and call
	 * display_write() with it on the last flush.  This created a tight
	 * coupling where LVGL rendered into memory the driver controlled.
	 *
	 * NEW PATH (LVGL-owned buffers, MCU+ SDK queue model):
	 * The driver manages the queue state machine (reqQ / currQ / doneQ)
	 * but allocates NO framebuffers.  LVGL allocates its own draw buffers
	 * (LV_Z_DOUBLE_VDB=y) and passes px_map directly to display_write().
	 * The driver queues the pointer, commits it to HW at the next VSYNC
	 * safe window, and signals completion via flip_done_sem.
	 *
	 * display_get_framebuffer() returns active_fb (the old front buffer
	 * just freed by VSYNC) for non-LVGL callers, but LVGL does not use
	 * it — LVGL always uses its own px_map.
	 */

	// /* --- OLD driver-framebuffer path (commented out) --- */
	// uint8_t *fb = display_get_framebuffer(data->display_dev);
	// if (fb != NULL) {
	// 	uint32_t fb_stride = data->cap.x_resolution * 4U;
	// 	uint32_t src_stride = ROUND_UP(w * 4U, LV_DRAW_BUF_STRIDE_ALIGN);
	// 	uint8_t *dst = fb + (uint32_t)area->y1 * fb_stride +
	// 		       (uint32_t)area->x1 * 4U;
	// 	for (int32_t y = 0; y < h; y++) {
	// 		memcpy(dst, px_map, w * 4U);
	// 		dst += fb_stride;
	// 		px_map += src_stride;
	// 	}
	// 	if (!lv_display_flush_is_last(display)) {
	// 		lv_display_flush_ready(display);
	// 		return;
	// 	}
	// 	sys_cache_data_flush_range(fb, fb_stride * data->cap.y_resolution);
	// 	flush.display = display;
	// 	flush.x = 0; flush.y = 0;
	// 	flush.desc.buf_size = fb_stride * data->cap.y_resolution;
	// 	flush.desc.width = data->cap.x_resolution;
	// 	flush.desc.pitch = data->cap.x_resolution;
	// 	flush.desc.height = data->cap.y_resolution;
	// 	flush.buf = fb;
	// 	lvgl_flush_display(&flush);
	// 	return;
	// }

	/*
	 * LVGL-owned buffer path.
	 *
	 * With LV_Z_FULL_REFRESH=y and LV_Z_DOUBLE_VDB=y:
	 *   - LVGL renders the full screen into one of its own draw buffers.
	 *   - Only one flush call per frame (lv_display_flush_is_last always true).
	 *   - px_map is a full-screen LVGL-allocated buffer (w=hres, h=vres).
	 *
	 * Cache flush: px_map is in CPU cache; the DSS DMA reads from physical
	 * RAM.  Flush the entire LVGL draw buffer before handing it to the
	 * driver so the DMA sees fresh pixel data.
	 *
	 * display_write() enqueues px_map into the driver reqQ and blocks
	 * until the buffer is committed to hardware (flip_done_sem), at
	 * which point LVGL may reuse px_map (or its companion VDB) for the
	 * next frame.
	 */
	//  printk("LVGL flush_cb: area (%d,%d)-(%d,%d) w=%d h=%d px_map=%p\n",
	// 	area->x1, area->y1, area->x2, area->y2, w, h, px_map);
	if (!lv_display_flush_is_last(display)) {
		lv_display_flush_ready(display);
		return;
	}

	/*
	 * lv_display_set_rotation() only changes the *logical* resolution LVGL
	 * exposes to widgets and to `area` here — it does not rotate pixel data
	 * or remap coordinates into the panel's native buffer. The panel's DSS
	 * timing (and cap.x_resolution/y_resolution) stay fixed at the native
	 * portrait geometry, so a driver-side rotate is still required, same as
	 * LVGL's own reference drivers (sdl, st_ltdc, renesas_glcdc) do in their
	 * flush callbacks.
	 */
	if (lv_display_get_rotation(display) == LV_DISPLAY_ROTATION_90) {
		static uint8_t *rotate_buf[2];
		static size_t rotate_buf_size;
		static uint8_t rotate_buf_idx;
		/* Full logical-resolution area (not the possibly-partial `area` arg). */
		lv_area_t rotated_area = {
			.x1 = 0,
			.y1 = 0,
			.x2 = w - 1,
			.y2 = h - 1,
		};
		uint32_t src_stride = w * 4U;
		uint32_t dst_stride = h * 4U;
		size_t needed = (size_t)w * h * 4U;
		uint8_t *dst;

		/* Maps the logical (rotated) area back to native panel coordinates. */
		lv_display_rotate_area(display, &rotated_area);

		if (rotate_buf_size < needed) {
			lv_free(rotate_buf[0]);
			lv_free(rotate_buf[1]);
			rotate_buf[0] = lv_malloc(needed);
			rotate_buf[1] = lv_malloc(needed);
			__ASSERT(rotate_buf[0] != NULL && rotate_buf[1] != NULL,
				 "Failed to allocate LVGL rotation buffer");
			rotate_buf_size = needed;
		}
		
		dst = rotate_buf[rotate_buf_idx];
		rotate_buf_idx ^= 1U;

		lv_draw_sw_rotate(px_map, dst, w, h, src_stride, dst_stride,
				   LV_DISPLAY_ROTATION_90, LV_COLOR_FORMAT_ARGB8888);

		/* The rotated buffer, not px_map, is what the DMA/HW will read. */
		// sys_cache_data_flush_range(dst, needed);

		flush.display = display;
		flush.x = rotated_area.x1;
		flush.y = rotated_area.y1;
		flush.desc.buf_size = (uint32_t)needed;
		flush.desc.width = h;
		flush.desc.pitch = ROUND_UP(h * 4U, LV_DRAW_BUF_STRIDE_ALIGN) / 4U;
		flush.desc.height = w;
		flush.buf = (void *)dst;
		lvgl_flush_display(&flush);
		return;
	}

	/* Flush the entire LVGL draw buffer to physical RAM (one call/frame) */
	// {
	// 	uint32_t fb_stride = data->cap.x_resolution * 4U;

	// 	sys_cache_data_flush_range(px_map,
	// 				   fb_stride * data->cap.y_resolution);
	// }

	flush.display = display;
	flush.x = 0;
	flush.y = 0;
	flush.desc.buf_size = w * 4U * h;
	flush.desc.width = w;
	flush.desc.pitch = ROUND_UP(w * 4U, LV_DRAW_BUF_STRIDE_ALIGN) / 4U;
	flush.desc.height = h;
	flush.buf = (void *)px_map;
	lvgl_flush_display(&flush);
}
