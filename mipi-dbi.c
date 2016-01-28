//#define DEBUG
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

#include <drm/drm_gem_cma_helper.h>
#include <drm/tinydrm/lcdreg.h>
#include <drm/tinydrm/tinydrm.h>
#include <linux/module.h>
#include <linux/swab.h>
#include <video/mipi_display.h>

void tinydrm_xrgb8888_to_rgb565(u32 *src, u16 *dst, unsigned num_pixels, bool swap_bytes)
{
	int i;

	for (i = 0; i < num_pixels; i++) {
		*dst = ((*src & 0x00F80000) >> 8) |
		       ((*src & 0x0000FC00) >> 5) |
		       ((*src & 0x000000F8) >> 3);
		if (swap_bytes)
			*dst = swab16(*dst);
		src++;
		dst++;
	}
}

int tinydrm_update_rgb565_lcdreg(struct tinydrm_device *tdev, struct drm_framebuffer *fb, struct drm_gem_cma_object *cma_obj, struct drm_clip_rect *clip)
{
	unsigned num_pixels = (clip->x2 - clip->x1 + 1) *
			      (clip->y2 - clip->y1 + 1);
	struct lcdreg_transfer tr = {
		.index = 1,
		.width = 16,
		.count = num_pixels
	};
	bool byte_swap = false;
	u16 *buf = NULL;
	int ret;

	dev_err_once(tdev->base->dev, "pixel_format = %s, bpw = 0x%08x\n", drm_get_format_name(fb->pixel_format), tdev->lcdreg->bits_per_word_mask);

	switch (fb->pixel_format) {
	case DRM_FORMAT_RGB565:
		tr.buf = cma_obj->vaddr;
		break;
	case DRM_FORMAT_XRGB8888:
		buf = kmalloc(num_pixels * sizeof(u16), GFP_KERNEL);
		if (!buf)
			return -ENOMEM;

#if defined(__LITTLE_ENDIAN)
		byte_swap = !lcdreg_bpw_supported(tdev->lcdreg, 16);
#endif
		tinydrm_xrgb8888_to_rgb565(cma_obj->vaddr, buf, num_pixels, byte_swap);
		tr.buf = buf;
		if (byte_swap) {
			tr.width = 8;
			tr.count *= 2;
		}
		break;
	default:
		dev_err_once(tdev->base->dev,
			     "pixel_format '%s' is not supported\n",
			     drm_get_format_name(fb->pixel_format));
		return -EINVAL;
	}

	ret = lcdreg_write(tdev->lcdreg, MIPI_DCS_WRITE_MEMORY_START, &tr);
	kfree(buf);

	return ret;
}

int mipi_dbi_update(struct tinydrm_device *tdev)
{
	struct drm_gem_cma_object *cma_obj;
	struct lcdreg *reg = tdev->lcdreg;
	struct drm_framebuffer *fb;
	struct drm_clip_rect clip;

	spin_lock(&tdev->dirty.lock);
	clip = tdev->dirty.clip;
	tinydrm_reset_clip(&tdev->dirty.clip);
	cma_obj = tdev->dirty.cma_obj;
	fb = tdev->dirty.fb;
	spin_unlock(&tdev->dirty.lock);

	if (!cma_obj || !fb)
		return -EINVAL;

	/* TODO: support partial updates */
	clip.x1 = 0;
	clip.x2 = fb->width - 1;
	clip.y1 = 0;
	clip.y2 = fb->height - 1;

	dev_dbg(tdev->base->dev, "%s: cma_obj=%p, vaddr=%p, paddr=%pad\n", __func__, cma_obj, cma_obj->vaddr, &cma_obj->paddr);
	dev_dbg(tdev->base->dev, "%s: x1=%u, x2=%u, y1=%u, y2=%u\n", __func__, clip.x1, clip.x2, clip.y1, clip.y2);
	dev_dbg(tdev->base->dev, "\n");

	lcdreg_writereg(reg, MIPI_DCS_SET_COLUMN_ADDRESS,
			(clip.x1 >> 8) & 0xFF, clip.x1 & 0xFF,
			(clip.x2 >> 8) & 0xFF, clip.x2 & 0xFF);
	lcdreg_writereg(reg, MIPI_DCS_SET_PAGE_ADDRESS,
			(clip.y1 >> 8) & 0xFF, clip.y1 & 0xFF,
			(clip.y2 >> 8) & 0xFF, clip.y2 & 0xFF);

	return tinydrm_update_rgb565_lcdreg(tdev, fb, cma_obj, &clip);
}
EXPORT_SYMBOL(mipi_dbi_update);

#define MIPI_REGISTER_LOADING_DETECTION		BIT(7)
#define MIPI_FUNCTIONALITY_DETECTION		BIT(6)

/* value: 0=sleep mode on, 1=sleep mode off */
int mipi_dbi_check(struct lcdreg *reg)
{
	u32 val[4];
	int ret;

#if defined(DEBUG) || defined(DYNAMIC_DEBUG)
	lcdreg_readreg_buf32(reg, MIPI_DCS_GET_DISPLAY_ID, val, 3);
	dev_dbg(reg->dev, "Display ID (%02x): %02x %02x %02x\n",
		MIPI_DCS_GET_DISPLAY_ID, val[0], val[1], val[2]);

	lcdreg_readreg_buf32(reg, MIPI_DCS_GET_DISPLAY_STATUS, val, 4);
	dev_dbg(reg->dev, "Display status (%02x): %02x %02x %02x %02x\n",
		MIPI_DCS_GET_DISPLAY_STATUS, val[0], val[1], val[2], val[3]);

	lcdreg_readreg_buf32(reg, MIPI_DCS_GET_POWER_MODE, val, 1);
	dev_dbg(reg->dev, "Power mode (%02x): %02x\n",
		MIPI_DCS_GET_POWER_MODE, val[0]);

	lcdreg_readreg_buf32(reg, MIPI_DCS_GET_ADDRESS_MODE, val, 1);
	dev_dbg(reg->dev, "Address mode (%02x): %02x\n",
		MIPI_DCS_GET_ADDRESS_MODE, val[0]);

	lcdreg_readreg_buf32(reg, MIPI_DCS_GET_PIXEL_FORMAT, val, 1);
	dev_dbg(reg->dev, "Pixel format (%02x): %02x\n",
		MIPI_DCS_GET_PIXEL_FORMAT, val[0]);

	lcdreg_readreg_buf32(reg, MIPI_DCS_GET_DISPLAY_MODE, val, 1);
	dev_dbg(reg->dev, "Display mode (%02x): %02x\n",
		MIPI_DCS_GET_DISPLAY_MODE, val[0]);

	lcdreg_readreg_buf32(reg, MIPI_DCS_GET_SIGNAL_MODE, val, 1);
	dev_dbg(reg->dev, "Display signal mode (%02x): %02x\n",
		MIPI_DCS_GET_SIGNAL_MODE, val[0]);
#endif

	ret = lcdreg_readreg_buf32(reg, MIPI_DCS_GET_DIAGNOSTIC_RESULT,
				   val, 1);
	if (ret) {
		dev_warn(reg->dev,
			"failed to read from controller: %d", ret);
		return ret;
	}

	dev_dbg(reg->dev, "Diagnostic result (%02x): %02x\n",
		MIPI_DCS_GET_DIAGNOSTIC_RESULT, val[0]);

	return 0;
}
EXPORT_SYMBOL(mipi_dbi_check);

MODULE_DESCRIPTION("MIPI DBI LCD controller support");
MODULE_AUTHOR("Noralf Trønnes");
MODULE_LICENSE("GPL");
