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
struct gpio_desc;
	struct lcdreg2;

/**
 * mipi_dbi - MIPI DBI controller
 * @tinydrm: tinydrm base
 * @reg: register map
 * @rotation: initial rotation in degress Counter Clock Wise
 * @prepared_once: only prepare once (no backlight nor power control)
 * @backlight: backlight device (optional)
 * @regulator: power regulator (optional)
 */
struct mipi_dbi {
	struct tinydrm_device tinydrm;
	struct regmap *reg;
struct lcdreg2 *lcdreg2;
	struct gpio_desc *reset;
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

int mipi_dbi_spi_init(struct mipi_dbi *mipi, struct spi_device *spi,
		      struct gpio_desc *dc, struct gpio_desc *reset,
		      bool writeonly);
int mipi_dbi_init(struct device *dev, struct mipi_dbi *mipi,
		  struct drm_driver *driver,
		  const struct drm_display_mode *mode, unsigned int rotation);
void mipi_dbi_hw_reset(struct mipi_dbi *mipi);
int mipi_dbi_dirty(struct drm_framebuffer *fb,
		   struct drm_gem_cma_object *cma_obj,
		   unsigned flags, unsigned color,
		   struct drm_clip_rect *clips, unsigned num_clips);
int mipi_dbi_enable_backlight(struct tinydrm_device *tdev);
void mipi_dbi_disable_backlight(struct tinydrm_device *tdev);
bool mipi_dbi_display_is_on(struct regmap *reg);
void mipi_dbi_debug_dump_regs(struct regmap *reg);
void mipi_dbi_unprepare(struct tinydrm_device *tdev);

#define mipi_dbi_write(reg, regnr, seq...) \
({ \
	u32 d[] = { seq }; \
	mipi_dbi_write_buf32(reg, regnr, d, ARRAY_SIZE(d)); \
})

int mipi_dbi_write_buf32(struct regmap *reg, unsigned regnr, const u32 *buf32,
			 size_t count);

#endif /* __LINUX_MIPI_DBI_H */
