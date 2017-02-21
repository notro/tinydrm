/*
 * Copyright (C) 2017 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __LINUX_TINYDRM_HELPERS_ADD_H
#define __LINUX_TINYDRM_HELPERS_ADD_H

#include <drm/tinydrm/tinydrm-helpers.h>

struct gpio_desc;

int tinydrm_rgb565_buf_copy(void *dst, struct drm_framebuffer *fb,
			    struct drm_clip_rect *clip, bool swap);
void tinydrm_hw_reset(struct gpio_desc *reset, unsigned int assert_ms,
		      unsigned int settle_ms);

#endif /* __LINUX_TINYDRM_HELPERS_ADD_H */
