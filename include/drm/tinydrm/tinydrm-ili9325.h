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

#include <drm/tinydrm/tinydrm-helpers2.h>
#include <drm/tinydrm/tinydrm-panel.h>
#include <drm/tinydrm/tinydrm-regmap.h>

int tinydrm_ili9325_init(struct device *dev, struct tinydrm_panel *panel,
			 const struct tinydrm_panel_funcs *funcs,
			 struct regmap *reg, struct drm_driver *driver,
			 const struct drm_display_mode *mode,
			 unsigned int rotation);

static inline void tinydrm_ili9325_reset(struct tinydrm_panel *panel)
{
	tinydrm_hw_reset(panel->reset, 1, 10);
}

struct regmap *tinydrm_ili9325_spi_init(struct spi_device *spi,
					unsigned int id);

#endif /* __LINUX_TINYDRM_ILI9325_H */
