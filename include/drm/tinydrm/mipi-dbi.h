/*
 * MIPI Display Bus Interface (DBI) LCD controller support
 *
 * Copyright 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __LINUX_MIPI_DBI_H
#define __LINUX_MIPI_DBI_H

#include <drm/tinydrm/tinydrm.h>

struct drm_gem_cma_object;
struct drm_framebuffer;
struct drm_clip_rect;
struct lcdreg;

/**
 * mipi_dbi - MIPI DBI controller
 * @tinydrm: tinydrm base
 * @reg: LCD register
 * @rotation: initial rotation in degress Counter Clock Wise
 * @prepared_once: only prepare once (no backlight nor power control)
 * @backlight: backlight device (optional)
 * @regulator: power regulator (optional)
 */
struct mipi_dbi {
	struct tinydrm_device tinydrm;
	struct lcdreg *reg;
	unsigned int rotation;
	bool prepared_once;
	struct backlight_device *backlight;
	struct regulator *regulator;
};

static inline struct mipi_dbi *
mipi_dbi_from_tinydrm(struct tinydrm_device *tdev)
{
	return container_of(tdev, struct mipi_dbi, tinydrm);
}

int mipi_dbi_init(struct device *dev, struct mipi_dbi *mipi,
		  struct lcdreg *reg, struct drm_driver *driver,
		  const struct drm_display_mode *mode, unsigned int rotation);
int mipi_dbi_dirty(struct drm_framebuffer *fb,
		   struct drm_gem_cma_object *cma_obj,
		   unsigned flags, unsigned color,
		   struct drm_clip_rect *clips, unsigned num_clips);
int mipi_dbi_enable_backlight(struct tinydrm_device *tdev);
void mipi_dbi_disable_backlight(struct tinydrm_device *tdev);
bool mipi_dbi_display_is_on(struct lcdreg *reg);
void mipi_dbi_debug_dump_regs(struct lcdreg *reg);
void mipi_dbi_unprepare(struct tinydrm_device *tdev);

#endif /* __LINUX_MIPI_DBI_H */
