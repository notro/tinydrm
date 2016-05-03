/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>

#include <drm/drm_crtc.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_panel_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/tinydrm/tinydrm.h>

static void tinydrm_display_pipe_enable(struct drm_simple_display_pipe *pipe,
					struct drm_crtc_state *crtc_state)
{
	struct tinydrm_device *tdev;

	tdev = container_of(pipe, struct tinydrm_device, pipe);
	DRM_DEBUG_KMS("prepared=%u, enabled=%u\n", tdev->prepared, tdev->enabled);

	/* The panel must be prepared on the first crtc enable after probe */
	tinydrm_prepare(tdev);
	/* The panel is enabled after the first display update */
}

static void tinydrm_display_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct tinydrm_device *tdev;

	tdev = container_of(pipe, struct tinydrm_device, pipe);
	DRM_DEBUG_KMS("prepared=%u, enabled=%u\n", tdev->prepared, tdev->enabled);

	tinydrm_disable(tdev);
}

static void
tinydrm_display_pipe_plane_update(struct drm_simple_display_pipe *pipe,
				  struct drm_plane_state *plane_state)
{
	struct tinydrm_device *tdev;

	tdev = container_of(pipe, struct tinydrm_device, pipe);
	DRM_DEBUG_KMS("next_update_full == %u\n", tdev->next_update_full);
/* TODO: Should a worker should do this update now instead of waiting for the next dirty()? */
	tdev->next_update_full = true;
}

struct drm_simple_display_pipe_funcs tinydrm_display_pipe_funcs = {
	.enable = tinydrm_display_pipe_enable,
	.disable = tinydrm_display_pipe_disable,
	.plane_update = tinydrm_display_pipe_plane_update,
};

int tinydrm_display_pipe_init(struct tinydrm_device *tdev,
			      const uint32_t *formats, unsigned int format_count)
{
	struct drm_device *dev = tdev->base;
	struct drm_connector *connector;
	int ret;

	tdev->next_update_full = true;
	connector = drm_panel_connector_create(dev, &tdev->panel,
					       DRM_MODE_CONNECTOR_VIRTUAL);
	if (IS_ERR(connector))
		return PTR_ERR(connector);

	ret = drm_simple_display_pipe_init(dev, &tdev->pipe,
				&tinydrm_display_pipe_funcs,
				formats, format_count,
				connector);

	return ret;
}
EXPORT_SYMBOL(tinydrm_display_pipe_init);

int tinydrm_panel_get_modes(struct drm_panel *panel)
{
	struct drm_display_mode *mode;
	struct tinydrm_device *tdev;

	tdev = container_of(panel, struct tinydrm_device, panel);
// TODO: get width/height somewhere else
	mode = drm_cvt_mode(panel->connector->dev, tdev->width, tdev->height,
			    60, false, false, false);
	if (!mode)
		return 0;

	mode->type |= DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(panel->connector, mode);

	return 1;
}
EXPORT_SYMBOL(tinydrm_panel_get_modes);
