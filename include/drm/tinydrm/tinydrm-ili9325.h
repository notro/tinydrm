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

#include <drm/tinydrm/tinydrm.h>
#include <drm/tinydrm/tinydrm-helpers2.h>

struct drm_minor;

/**
 * struct tinydrm_ili9325 - tinydrm ILI9325 device
 * @tinydrm: Base &tinydrm_device
 * @reg: Register map (optional)
 * @enabled: Pipeline is enabled
 * @tx_buf: Transmit buffer
 * @swap_bytes: Swap pixel data bytes
 * @always_tx_buf:
 * @rotation: Rotation in degrees Counter Clock Wise
 * @reset: Optional reset gpio
 * @backlight: Optional backlight device
 * @regulator: Optional regulator
 */
struct tinydrm_ili9325 {
	struct tinydrm_device tinydrm;
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
tinydrm_to_ili9325(struct tinydrm_device *tdev)
{
	return container_of(tdev, struct tinydrm_ili9325, tinydrm);
}

int tinydrm_ili9325_init(struct device *dev, struct tinydrm_ili9325 *cntrl,
			 const struct drm_simple_display_pipe_funcs *funcs,
			 struct regmap *reg, struct drm_driver *driver,
			 const struct drm_display_mode *mode,
			 unsigned int rotation);

static inline void tinydrm_ili9325_reset(struct tinydrm_ili9325 *controller)
{
	tinydrm_hw_reset(controller->reset, 1, 10);
}

struct regmap *tinydrm_ili9325_spi_init(struct spi_device *spi,
					unsigned int id);

int tinydrm_ili9325_debugfs_init(struct drm_minor *minor);

#endif /* __LINUX_TINYDRM_ILI9325_H */
