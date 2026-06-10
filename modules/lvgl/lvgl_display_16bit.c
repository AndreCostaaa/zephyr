/*
 * Copyright (c) 2019 Jan Van Winkel <jan.van_winkel@dxplore.eu>
 * Copyright 2023 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <lvgl.h>
#include "lvgl_display.h"

void lvgl_flush_cb_16bit(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
	if(!lv_display_flush_is_last(display))
	{
		lv_display_flush_ready(display);
		return;
	}


	uint16_t w = lv_display_get_original_horizontal_resolution(display);
	uint16_t h = lv_display_get_original_vertical_resolution(display);
	struct lvgl_display_flush flush;

	flush.display = display;
	flush.x = 0;
	flush.y = 0;
	flush.desc.buf_size = w * 2U * h;
	flush.desc.width = w;
	flush.desc.pitch = ROUND_UP(w * 2U, LV_DRAW_BUF_STRIDE_ALIGN) / 2U;
	flush.desc.height = h;
	flush.buf = (void *)px_map;

	lvgl_flush_display(&flush);
}
