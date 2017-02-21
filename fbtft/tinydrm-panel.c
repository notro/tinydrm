/*
 * Copyright 2017 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/device.h>
#include <linux/dma-buf.h>

#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fourcc.h>

//drivers/gpu/drm/tinydrm/fbtft/../include/drm/tinydrm/tinydrm-helpers.h:40:14: warning: ‘struct drm_framebuffer’ declared inside parameter list [enabled by default]
struct drm_framebuffer;
#include <drm/tinydrm/tinydrm-helpers.h>

#include "tinydrm-panel.h"
#include "tinydrm-regmap.h"



/**
 * DOC: overview
 *
 * This library provides helpers for
 */




static void tinydrm_panel_pipe_enable(struct drm_simple_display_pipe *pipe,
				      struct drm_crtc_state *crtc_state)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct tinydrm_panel *panel = to_tinydrm_panel(tdev);
//struct drm_framebuffer *fb = pipe->plane.fb;

	DRM_DEBUG_KMS("\n");


//panel->enabled = true;
//if (fb)
//	fb->funcs->dirty(fb, NULL, 0, 0, NULL, 0);


	if (panel->funcs && panel->funcs->enable)
		panel->funcs->enable(panel);
	else
		tinydrm_enable_backlight(panel->backlight);
}

static void tinydrm_panel_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct tinydrm_panel *panel = to_tinydrm_panel(tdev);

	DRM_DEBUG_KMS("\n");

//panel->enabled = false;

	if (panel->funcs && panel->funcs->disable)
		panel->funcs->enable(panel);
	else
		tinydrm_disable_backlight(panel->backlight);
}

static void tinydrm_panel_pipe_update(struct drm_simple_display_pipe *pipe,
				      struct drm_plane_state *old_state)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct tinydrm_panel *panel = to_tinydrm_panel(tdev);
	struct drm_framebuffer *fb = pipe->plane.state->fb;

	/* fb is set, not changed */
	if (fb && !old_state->fb && panel->funcs && panel->funcs->prepare)
		panel->funcs->prepare(panel);

	tinydrm_display_pipe_update(pipe, old_state);

	/* fb is unset */
	if (!fb && panel->funcs && panel->funcs->unprepare)
		panel->funcs->unprepare(panel);
}

static const struct drm_simple_display_pipe_funcs tinydrm_panel_pipe_funcs = {
	.enable = tinydrm_panel_pipe_enable,
	.disable = tinydrm_panel_pipe_disable,
	.update = tinydrm_panel_pipe_update,
	.prepare_fb = tinydrm_display_pipe_prepare_fb,
};

/**
 * tinydrm_panel_init - initialization XXXX
 * @dev: Parent device
 * @panel: &tinydrm_panel structure to initialize
 * @funcs: Callbacks for the panel (optional)
 * @formats: Array of supported formats (DRM_FORMAT\_\*)
 * @format_count: Number of elements in @formats
 * @fb_funcs: Framebuffer functions
 * @driver: DRM driver
 * @mode: Display mode
 * @rotation: Initial rotation in degrees Counter Clock Wise
 *
 * This function initializes a &tinydrm_panel structure and it's underlying
 * @tinydrm_device. It also sets up the display pipeline.
 *
 * Objects created by this function will be automatically freed on driver
 * detach (devres).
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int tinydrm_panel_init(struct device *dev, struct tinydrm_panel *panel,
			const struct tinydrm_panel_funcs *funcs,
			const uint32_t *formats, unsigned int format_count,
			const struct drm_framebuffer_funcs *fb_funcs,
		  	struct drm_driver *driver,
		  	const struct drm_display_mode *mode,
		  	unsigned int rotation)
{


size_t bufsize = mode->vdisplay * mode->hdisplay * sizeof(u16);


	struct tinydrm_device *tdev = &panel->tinydrm;
	const struct drm_format_info *format_info;
	int ret;

	panel->tx_buf = devm_kmalloc(dev, bufsize, GFP_KERNEL);
	if (!panel->tx_buf)
		return -ENOMEM;

	ret = devm_tinydrm_init(dev, tdev, fb_funcs, driver);
	if (ret)
		return ret;

	ret = tinydrm_display_pipe_init(tdev, &tinydrm_panel_pipe_funcs,
					DRM_MODE_CONNECTOR_VIRTUAL,
					formats, format_count, mode,
					rotation);
	if (ret)
		return ret;

	format_info = drm_format_info(formats[0]);
	tdev->drm->mode_config.preferred_depth = format_info->depth;

	panel->rotation = rotation;
	panel->funcs = funcs;

	drm_mode_config_reset(tdev->drm);

	DRM_DEBUG_KMS("preferred_depth=%u, rotation = %u\n",
		      tdev->drm->mode_config.preferred_depth, rotation);

	return 0;
}
EXPORT_SYMBOL(tinydrm_panel_init);

#ifdef CONFIG_DEBUG_FS

static const struct drm_info_list tinydrm_panel_debugfslist[] = {
	{ "fb",   drm_fb_cma_debugfs_show, 0 },
};

/**
 * tinydrm_panel_debugfs_init - Create tinydrm panel debugfs entries
 * @minor: DRM minor
 *
 * &tinydrm_panel drivers can use this as their
 * &drm_driver->debugfs_init callback.
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int tinydrm_panel_debugfs_init(struct drm_minor *minor)
{
	struct tinydrm_device *tdev = minor->dev->dev_private;
	struct tinydrm_panel *panel = to_tinydrm_panel(tdev);
	struct regmap *reg = panel->reg;
	int ret;

	if (reg) {
		ret = tinydrm_regmap_debugfs_init(reg, minor->debugfs_root);
		if (ret)
			return ret;
	}

	return drm_debugfs_create_files(tinydrm_panel_debugfslist,
					ARRAY_SIZE(tinydrm_panel_debugfslist),
					minor->debugfs_root, minor);
}
EXPORT_SYMBOL(tinydrm_panel_debugfs_init);

#endif
