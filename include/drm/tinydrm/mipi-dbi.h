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

struct drm_gem_cma_object;
struct drm_framebuffer;
struct tinydrm_device;
struct drm_clip_rect;
struct lcdreg;

int mipi_dbi_init(struct device *dev, struct tinydrm_device *tdev);
int mipi_dbi_dirty(struct drm_framebuffer *fb,
		   struct drm_gem_cma_object *cma_obj,
		   unsigned flags, unsigned color,
		   struct drm_clip_rect *clips, unsigned num_clips);
bool mipi_dbi_display_is_on(struct lcdreg *reg);
void mipi_dbi_debug_dump_regs(struct lcdreg *reg);
void mipi_dbi_unprepare(struct tinydrm_device *tdev);

#endif /* __LINUX_MIPI_DBI_H */
