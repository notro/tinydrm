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
#include <linux/regulator/consumer.h>
#include <linux/swab.h>
#include <video/mipi_display.h>

#define DCS_POWER_MODE_DISPLAY			BIT(2)
#define DCS_POWER_MODE_DISPLAY_NORMAL_MODE	BIT(3)
#define DCS_POWER_MODE_SLEEP_MODE		BIT(4)
#define DCS_POWER_MODE_PARTIAL_MODE		BIT(5)
#define DCS_POWER_MODE_IDLE_MODE		BIT(6)
#define DCS_POWER_MODE_RESERVED_MASK		(BIT(0) | BIT(1) | BIT(7))

/* TODO: Move common functions to a separate module */
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

// TODO: Pass in regnr
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

static void mipi_dbi_deferred_update(struct work_struct *work)
{
	struct tinydrm_device *tdev = work_to_tinydrm(work);
	struct lcdreg *reg = tdev->lcdreg;
	struct tinydrm_fb_clip fb_clip;
	struct drm_clip_rect *clip = &fb_clip.clip;
	int ret;

	dev_dbg(tdev->base->dev, "%s\n", __func__);

	if (!tinydrm_deferred_begin(tdev, &fb_clip))
		return;

	dev_dbg(tdev->base->dev, "%s: cma_obj=%p, vaddr=%p, paddr=%pad\n", __func__, fb_clip.cma_obj, fb_clip.cma_obj->vaddr, &fb_clip.cma_obj->paddr);
	dev_dbg(tdev->base->dev, "%s: x1=%u, x2=%u, y1=%u, y2=%u\n", __func__, clip->x1, clip->x2, clip->y1, clip->y2);
	dev_dbg(tdev->base->dev, "\n");

	lcdreg_writereg(reg, MIPI_DCS_SET_COLUMN_ADDRESS,
			(clip->x1 >> 8) & 0xFF, clip->x1 & 0xFF,
			(clip->x2 >> 8) & 0xFF, clip->x2 & 0xFF);
	lcdreg_writereg(reg, MIPI_DCS_SET_PAGE_ADDRESS,
			(clip->y1 >> 8) & 0xFF, clip->y1 & 0xFF,
			(clip->y2 >> 8) & 0xFF, clip->y2 & 0xFF);

	ret = tinydrm_update_rgb565_lcdreg(tdev, fb_clip.fb, fb_clip.cma_obj, clip);
	if (ret)
		dev_err_once(tdev->base->dev, "Failed to update display %d\n", ret);

	tinydrm_deferred_end(tdev);
}

int mipi_dbi_init(struct device *dev, struct tinydrm_device *tdev)
{
	tdev->deferred = devm_kzalloc(dev, sizeof(*tdev->deferred),
				      GFP_KERNEL);
	if (!tdev->deferred)
		return -ENOMEM;

	INIT_DELAYED_WORK(&tdev->deferred->dwork, mipi_dbi_deferred_update);
	tinydrm_reset_clip(&tdev->deferred->fb_clip.clip);
	tdev->lcdreg->def_width = 8;
	tdev->dirtyfb = tinydrm_dirtyfb;

	return 0;
}
EXPORT_SYMBOL(mipi_dbi_init);

/* Returns true if the display can be verified to be on */
bool mipi_dbi_display_is_on(struct lcdreg *reg)
{
	u32 val;

	if (!lcdreg_is_readable(reg))
		return false;

	if (lcdreg_readreg_buf32(reg, MIPI_DCS_GET_POWER_MODE, &val, 1))
		return false;

	val &= ~DCS_POWER_MODE_RESERVED_MASK;

	if (val != (DCS_POWER_MODE_DISPLAY |
	    DCS_POWER_MODE_DISPLAY_NORMAL_MODE | DCS_POWER_MODE_SLEEP_MODE))
		return false;

	DRM_DEBUG_DRIVER("Display is ON\n");

	return true;
}
EXPORT_SYMBOL(mipi_dbi_display_is_on);

void mipi_dbi_debug_dump_regs(struct lcdreg *reg)
{
	u32 val[4];
	int ret;

	if (!(lcdreg_is_readable(reg) && (drm_debug & DRM_UT_DRIVER)))
		return;

	ret = lcdreg_readreg_buf32(reg, MIPI_DCS_GET_DISPLAY_ID, val, 3);
	if (ret) {
		dev_warn(reg->dev,
			 "failed to read from controller: %d", ret);
		return;
	}

	/* RDDID is not part of the MIPI standard, but seems to be common */
	DRM_DEBUG_DRIVER("Display ID (%02x): %02x %02x %02x\n",
			 MIPI_DCS_GET_DISPLAY_ID, val[0], val[1], val[2]);

	lcdreg_readreg_buf32(reg, MIPI_DCS_GET_DISPLAY_STATUS, val, 4);
	DRM_DEBUG_DRIVER("Display status (%02x): %02x %02x %02x %02x\n",
			 MIPI_DCS_GET_DISPLAY_STATUS, val[0], val[1], val[2], val[3]);

	lcdreg_readreg_buf32(reg, MIPI_DCS_GET_POWER_MODE, val, 1);
	DRM_DEBUG_DRIVER("Power mode (%02x): %02x\n",
			 MIPI_DCS_GET_POWER_MODE, val[0]);

	lcdreg_readreg_buf32(reg, MIPI_DCS_GET_ADDRESS_MODE, val, 1);
	DRM_DEBUG_DRIVER("Address mode (%02x): %02x\n",
			 MIPI_DCS_GET_ADDRESS_MODE, val[0]);

	lcdreg_readreg_buf32(reg, MIPI_DCS_GET_PIXEL_FORMAT, val, 1);
	DRM_DEBUG_DRIVER("Pixel format (%02x): %02x\n",
			 MIPI_DCS_GET_PIXEL_FORMAT, val[0]);

	lcdreg_readreg_buf32(reg, MIPI_DCS_GET_DISPLAY_MODE, val, 1);
	DRM_DEBUG_DRIVER("Display mode (%02x): %02x\n",
			 MIPI_DCS_GET_DISPLAY_MODE, val[0]);

	lcdreg_readreg_buf32(reg, MIPI_DCS_GET_SIGNAL_MODE, val, 1);
	DRM_DEBUG_DRIVER("Display signal mode (%02x): %02x\n",
			 MIPI_DCS_GET_SIGNAL_MODE, val[0]);

	lcdreg_readreg_buf32(reg, MIPI_DCS_GET_DIAGNOSTIC_RESULT, val, 1);
	DRM_DEBUG_DRIVER("Diagnostic result (%02x): %02x\n",
			 MIPI_DCS_GET_DIAGNOSTIC_RESULT, val[0]);
}
EXPORT_SYMBOL(mipi_dbi_debug_dump_regs);

int mipi_dbi_panel_unprepare(struct drm_panel *panel)
{
	struct tinydrm_device *tdev = tinydrm_from_panel(panel);
	struct lcdreg *reg = tdev->lcdreg;

	/*
	 * Only do this if we have turned off backlight because if it's on the
	 * display will in most cases turn all white when the pixels are
	 * turned off.
	 */
	if (tdev->backlight) {
		lcdreg_writereg(reg, MIPI_DCS_SET_DISPLAY_OFF);
		lcdreg_writereg(reg, MIPI_DCS_ENTER_SLEEP_MODE);
	}

	if (tdev->regulator)
		regulator_disable(tdev->regulator);

	return 0;
}
EXPORT_SYMBOL(mipi_dbi_panel_unprepare);

MODULE_LICENSE("GPL");
