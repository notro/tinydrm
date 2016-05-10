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
#include <drm/drm_simple_kms_helper.h>
#include <drm/tinydrm/tinydrm.h>


#include <linux/slab.h>

#include <drm/drmP.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>

struct drm_panel_connector {
	struct drm_connector base;
	struct drm_panel *panel;
};

static inline struct drm_panel_connector *
to_drm_panel_connector(struct drm_connector *connector)
{
	return container_of(connector, struct drm_panel_connector, base);
}

static int drm_panel_connector_get_modes(struct drm_connector *connector)
{
	return drm_panel_get_modes(to_drm_panel_connector(connector)->panel);
}

static const struct drm_connector_helper_funcs drm_panel_connector_hfuncs = {
	.get_modes = drm_panel_connector_get_modes,
	.best_encoder = drm_atomic_helper_best_encoder,
};

static enum drm_connector_status
drm_panel_connector_detect(struct drm_connector *connector, bool force)
{
	if (drm_device_is_unplugged(connector->dev))
		return connector_status_disconnected;

	return connector->status;
}

static void drm_panel_connector_destroy(struct drm_connector *connector)
{
	struct drm_panel_connector *panel_connector;

	panel_connector = to_drm_panel_connector(connector);
	drm_panel_detach(panel_connector->panel);
	drm_panel_remove(panel_connector->panel);
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
	kfree(panel_connector);
}

static const struct drm_connector_funcs drm_panel_connector_funcs = {
	.dpms = drm_atomic_helper_connector_dpms,
	.reset = drm_atomic_helper_connector_reset,
	.detect = drm_panel_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_panel_connector_destroy,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

struct drm_connector *drm_panel_connector_create(struct drm_device *dev,
						 struct drm_panel *panel,
						 int connector_type)
{
	struct drm_panel_connector *panel_connector;
	struct drm_connector *connector;
	int ret;

	panel_connector = kzalloc(sizeof(*panel_connector), GFP_KERNEL);
	if (!panel_connector)
		return ERR_PTR(-ENOMEM);

	panel_connector->panel = panel;
	connector = &panel_connector->base;
	drm_connector_helper_add(connector, &drm_panel_connector_hfuncs);
	ret = drm_connector_init(dev, connector, &drm_panel_connector_funcs,
				 connector_type);
	if (ret) {
		kfree(panel_connector);
		return ERR_PTR(ret);
	}

	connector->status = connector_status_connected;
	drm_panel_init(panel);
	drm_panel_add(panel);
	drm_panel_attach(panel, connector);

	return connector;
}





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
tinydrm_display_pipe_update(struct drm_simple_display_pipe *pipe,
			    struct drm_plane_state *pstate)
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
	.update = tinydrm_display_pipe_update,
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
