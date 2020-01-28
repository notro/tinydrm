/*
 * Copyright (C) 2017 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __LINUX_TINYDRM_ILI9325_H
#define __LINUX_TINYDRM_ILI9325_H

#include <drm/drm_drv.h>
#include <drm/drm_simple_kms_helper.h>

struct drm_minor;

struct tinydrm_ili9325 {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct drm_display_mode mode;
	struct regmap *reg;
	bool enabled;
	void *tx_buf;
	bool swap_bytes;
	bool always_tx_buf;
	unsigned int rotation;
	struct gpio_desc *reset;
	struct backlight_device *backlight;
	struct regulator *regulator;
};

static inline struct tinydrm_ili9325 *
drm_to_ili9325(struct drm_device *drm)
{
	return container_of(drm, struct tinydrm_ili9325, drm);
}

void tinydrm_ili9325_fb_dirty(struct drm_framebuffer *fb, struct drm_rect *rect);

struct regmap *tinydrm_ili9325_spi_init(struct spi_device *spi,
					unsigned int id);

int tinydrm_ili9325_debugfs_init(struct drm_minor *minor);

#endif /* __LINUX_TINYDRM_ILI9325_H */
