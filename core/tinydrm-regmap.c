/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drmP.h>
#include <drm/tinydrm/tinydrm.h>
#include <linux/module.h>
#include <linux/regmap.h>

/**
 * tinydrm_regmap_flush_rgb565 - flush framebuffer to LCD register
 * @reg: LCD register map
 * @regnr: register number
 * @fb: framebuffer
 * @vmem: buffer backing the framebuffer
 * @clip: part of buffer to write
 *
 * Flush framebuffer changes to LCD register supporting RGB565. XRGB8888 is
 * converted to RGB565.
 */
int tinydrm_regmap_flush_rgb565(struct regmap *reg, u32 regnr,
				struct drm_framebuffer *fb, void *vmem,
				struct drm_clip_rect *clip)
{
	unsigned width = clip->x2 - clip->x1;
	unsigned height = clip->y2 - clip->y1;
	unsigned num_pixels = width * height;
	u16 *tr, *buf = NULL;
	int ret;

	/*
	 * TODO: Add support for all widths (requires a buffer copy)
	 *
	 * Crude X windows usage numbers for a 320x240 (76.8k pixel) display,
	 * possible improvements:
	 * - 80-90% cut for <2k pixel transfers
	 * - 40-50% cut for <50k pixel tranfers
	 */
	if (width != fb->width) {
		dev_err(fb->dev->dev,
			"Only full width clip are supported: x1=%u, x2=%u\n",
			clip->x1, clip->x2);
		return -EINVAL;
	}

	switch (fb->pixel_format) {
	case DRM_FORMAT_RGB565:
		vmem += clip->y1 * width * 2;
		tr = vmem;
		break;
	case DRM_FORMAT_XRGB8888:
		vmem += clip->y1 * width * 4;
		buf = kmalloc(num_pixels * sizeof(u16), GFP_KERNEL);
		if (!buf)
			return -ENOMEM;

		tinydrm_xrgb8888_to_rgb565(vmem, buf, num_pixels);
		tr = buf;
		break;
	default:
		dev_err_once(fb->dev->dev, "Format is not supported: %s\n",
			     drm_get_format_name(fb->pixel_format));
		return -EINVAL;
	}

	ret = regmap_raw_write(reg, regnr, tr, num_pixels * 2);
	kfree(buf);

	return ret;
}
EXPORT_SYMBOL(tinydrm_regmap_flush_rgb565);
