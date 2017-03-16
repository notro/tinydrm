/*
 * Copyright (C) 2017 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __LINUX_TINYDRM_FBTFT_H
#define __LINUX_TINYDRM_FBTFT_H

#include <drm/tinydrm/tinydrm.h>

int tinydrm_fbtft_init(struct device *dev, struct regmap *reg);
int tinydrm_fbtft_get_gamma(struct device *dev, u16 *curves,
			    const char *gamma_str, size_t num_curves,
			    size_t num_values);
int tinydrm_fbtft_get_rotation(struct device *dev, u32 *rotation);

#if IS_ENABLED(CONFIG_BACKLIGHT_CLASS_DEVICE)
struct backlight_device *tinydrm_fbtft_get_backlight(struct device *dev);
#else
static inline struct backlight_device *
tinydrm_fbtft_get_backlight(struct device *dev)
{
	return NULL;
}
#endif

#endif /* __LINUX_TINYDRM_FBTFT_H */
