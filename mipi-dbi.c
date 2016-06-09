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
#include <drm/tinydrm/mipi-dbi.h>
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

/**
 * DOC: overview
 *
 * This library provides helpers for MIPI display controllers with
 * Display Bus Interface (DBI).
 *
 * Many controllers are MIPI compliant and can use this library.
 * If a controller uses registers 0x2A and 0x2B to set the area to update
 * and uses register 0x2C to write to frame memory, it is most likely MIPI
 * compliant.
 */

/**
 * mipi_dbi_dirty - framebuffer dirty callback
 * @fb: framebuffer
 * @cma_obj: CMA buffer object
 * @flags: dirty fb ioctl flags
 * @color: color for annotated clips
 * @clips: dirty clip rectangles
 * @num_clips: number of @clips
 *
 * This function provides framebuffer flushing for MIPI DBI controllers.
 * Drivers should use this as their &tinydrm_funcs ->dirty callback.
 */
int mipi_dbi_dirty(struct drm_framebuffer *fb,
		   struct drm_gem_cma_object *cma_obj,
		   unsigned flags, unsigned color,
		   struct drm_clip_rect *clips, unsigned num_clips)
{
	struct tinydrm_device *tdev = fb->dev->dev_private;
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);
	struct lcdreg *reg = mipi->reg;
	struct drm_clip_rect clip;
	int ret;

	tinydrm_merge_clips(&clip, clips, num_clips, flags,
			    fb->width, fb->height);

	clip.x1 = 0;
	clip.x2 = fb->width;

	DRM_DEBUG("Flushing [FB:%d] x1=%u, x2=%u, y1=%u, y2=%u\n", fb->base.id,
		  clip.x1, clip.x2, clip.y1, clip.y2);

	tinydrm_debugfs_dirty_begin(tdev, fb, &clip);

	lcdreg_writereg(reg, MIPI_DCS_SET_COLUMN_ADDRESS,
			(clip.x1 >> 8) & 0xFF, clip.x1 & 0xFF,
			(clip.x2 >> 8) & 0xFF, (clip.x2 - 1) & 0xFF);
	lcdreg_writereg(reg, MIPI_DCS_SET_PAGE_ADDRESS,
			(clip.y1 >> 8) & 0xFF, clip.y1 & 0xFF,
			(clip.y2 >> 8) & 0xFF, (clip.y2 - 1) & 0xFF);

	ret = tinydrm_lcdreg_flush_rgb565(reg, MIPI_DCS_WRITE_MEMORY_START,
					  fb, cma_obj->vaddr, &clip);
	if (ret)
		dev_err_once(tdev->base->dev, "Failed to update display %d\n",
			     ret);

	tinydrm_debugfs_dirty_end(tdev, 0, 16);

	return ret;
}
EXPORT_SYMBOL(mipi_dbi_dirty);

/**
 * mipi_dbi_enable_backlight - mipi enable backlight helper
 * @tdev: tinydrm device
 *
 * Helper to enable &mipi_dbi ->backlight for the &tinydrm_funcs ->enable
 * callback.
 */
int mipi_dbi_enable_backlight(struct tinydrm_device *tdev)
{
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);

	if (mipi->backlight)
		return tinydrm_enable_backlight(mipi->backlight);

	if (!mipi->regulator)
		/* Full flush of blanked display memory */
		schedule_work(&tdev->dirty_work);

	return 0;
}
EXPORT_SYMBOL(mipi_dbi_enable_backlight);

static void mipi_dbi_blank(struct mipi_dbi *mipi)
{
	struct drm_device *drm = mipi->tinydrm.base;
	int height = drm->mode_config.min_height;
	int width = drm->mode_config.min_width;
	unsigned num_pixels = width * height;
	struct lcdreg *reg = mipi->reg;
	struct lcdreg_transfer tr = {
		.index = 1,
		.width = 16,
		.count = num_pixels
	};

	tr.buf = kzalloc(num_pixels * 2, GFP_KERNEL);
	if (!tr.buf)
		return;

	lcdreg_writereg(reg, MIPI_DCS_SET_COLUMN_ADDRESS, 0, 0,
			(width >> 8) & 0xFF, (width - 1) & 0xFF);
	lcdreg_writereg(reg, MIPI_DCS_SET_PAGE_ADDRESS, 0, 0,
			(height >> 8) & 0xFF, (height - 1) & 0xFF);
	lcdreg_write(reg, MIPI_DCS_WRITE_MEMORY_START, &tr);
	kfree(tr.buf);
}

/**
 * mipi_dbi_disable_backlight - mipi disable backlight helper
 * @tdev: tinydrm device
 *
 * Helper to disable &mipi_dbi ->backlight for the &tinydrm_funcs
 * ->disable callback.
 * If there's no backlight nor power control, blank display by writing zeroes.
 */
void mipi_dbi_disable_backlight(struct tinydrm_device *tdev)
{
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);

	if (mipi->backlight)
		tinydrm_disable_backlight(mipi->backlight);
	else if (!mipi->regulator)
		mipi_dbi_blank(mipi);
}
EXPORT_SYMBOL(mipi_dbi_disable_backlight);

/**
 * mipi_dbi_unprepare - mipi power off helper
 * @tdev: tinydrm device
 *
 * Helper to power off a MIPI controller.
 * Puts the controller in sleep mode if backlight control is enabled. It's done
 * like this to make sure we don't have backlight glaring through a panel with
 * all pixels turned off. If a regulator is registered it will be disabled.
 * Drivers can use this as their &tinydrm_funcs ->unprepare callback.
 */
void mipi_dbi_unprepare(struct tinydrm_device *tdev)
{
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);
	struct lcdreg *reg = mipi->reg;

	if (mipi->backlight) {
		lcdreg_writereg(reg, MIPI_DCS_SET_DISPLAY_OFF);
		lcdreg_writereg(reg, MIPI_DCS_ENTER_SLEEP_MODE);
	} else if (!mipi->regulator) {
		mipi->prepared_once = true;
	}

	if (mipi->regulator)
		regulator_disable(mipi->regulator);
}
EXPORT_SYMBOL(mipi_dbi_unprepare);

static const uint32_t mipi_dbi_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

int tinydrm_rotate_mode(struct drm_display_mode *mode, unsigned int rotation)
{
	if (rotation == 0 || rotation == 180) {
		return 0;
	} else if (rotation == 90 || rotation == 270) {
		swap(mode->hdisplay, mode->vdisplay);
		swap(mode->hsync_start, mode->vsync_start);
		swap(mode->hsync_end, mode->vsync_end);
		swap(mode->htotal, mode->vtotal);
		swap(mode->width_mm, mode->height_mm);
		return 0;
	} else {
		return -EINVAL;
	}
}

/**
 * mipi_dbi_init - MIPI DBI initialization
 * @dev: parent device
 * @mipi: &mipi_dbi structure to initialize
 * @reg: LCD register
 * @driver: DRM driver
 * @mode: display mode
 * @rotation: initial rotation in degress Counter Clock Wise
 *
 * This function initializes a &mipi_dbi structure and it's underlying
 * @tinydrm_device and &drm_device. It also sets up the display pipeline.
 * Native RGB565 format is supported and XRGB8888 is emulated.
 * Objects created by this function will be automatically freed on driver
 * detach (devres).
 */
int mipi_dbi_init(struct device *dev, struct mipi_dbi *mipi,
		  struct lcdreg *reg, struct drm_driver *driver,
		  const struct drm_display_mode *mode, unsigned int rotation)
{
	struct tinydrm_device *tdev = &mipi->tinydrm;
	struct drm_display_mode *mode_copy;
	struct drm_device *drm;
	int ret;

	mode_copy = devm_kmalloc(dev, sizeof(*mode_copy), GFP_KERNEL);
	if (!mode_copy)
		return -ENOMEM;

	*mode_copy = *mode;
	mipi->rotation = rotation;
	ret = tinydrm_rotate_mode(mode_copy, rotation);
	if (ret) {
		DRM_ERROR("Illegal rotation value %u\n", rotation);
		return -EINVAL;
	}

	ret = devm_tinydrm_init(dev, tdev, driver);
	if (ret)
		return ret;

	reg->def_width = 8;
	mipi->reg = reg;

	drm = tdev->base;
	drm->mode_config.min_width = mode_copy->hdisplay;
	drm->mode_config.max_width = mode_copy->hdisplay;
	drm->mode_config.min_height = mode_copy->vdisplay;
	drm->mode_config.max_height = mode_copy->vdisplay;
	drm->mode_config.preferred_depth = 16;
	ret = drm_mode_create_dirty_info_property(drm);
	if (ret)
		return ret;

	ret = tinydrm_display_pipe_init(tdev, mipi_dbi_formats,
					ARRAY_SIZE(mipi_dbi_formats),
					mode_copy);
	if (ret)
		return ret;

	drm_object_attach_property(&tdev->pipe.connector->base,
				   drm->mode_config.dirty_info_property,
				   DRM_MODE_DIRTY_ON);

	drm_mode_config_reset(drm);

	tinydrm_debugfs_dirty_init(tdev);

	DRM_DEBUG_KMS("preferred_depth=%u, rotation = %u\n",
		      drm->mode_config.preferred_depth, rotation);

	return 0;
}
EXPORT_SYMBOL(mipi_dbi_init);

/**
 * mipi_dbi_display_is_on - check if display is on
 * @reg: LCD register
 *
 * This function checks the Power Mode register (if readable) to see if
 * display output is turned on. This can be used to see if the bootloader
 * has already turned on the display avoiding flicker when the pipeline is
 * enabled.
 *
 * Returns:
 * true if the display can be verified to be on
 * false otherwise.
 */
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

/**
 * mipi_dbi_debug_dump_regs - dump some MIPI DCS registers
 * @reg: LCD register
 *
 * Dump some MIPI DCS registers using DRM_DEBUG_DRIVER().
 */
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

MODULE_LICENSE("GPL");
