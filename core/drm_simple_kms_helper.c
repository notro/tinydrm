/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drmP.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_panel.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <linux/slab.h>

struct drm_simple_kms_connector {
	struct drm_connector base;
	struct drm_panel *panel;
};

static inline struct drm_simple_kms_connector *
to_simple_connector(struct drm_connector *connector)
{
	return container_of(connector, struct drm_simple_kms_connector, base);
}

static int drm_simple_kms_connector_get_modes(struct drm_connector *connector)
{
	return drm_panel_get_modes(to_simple_connector(connector)->panel);
}

static struct drm_encoder *
drm_simple_kms_connector_best_encoder(struct drm_connector *connector)
{
	return drm_encoder_find(connector->dev, connector->encoder_ids[0]);
}

static const struct drm_connector_helper_funcs drm_simple_kms_connector_helper_funcs = {
	.get_modes = drm_simple_kms_connector_get_modes,
	.best_encoder = drm_simple_kms_connector_best_encoder,
};

static enum drm_connector_status
drm_simple_kms_connector_detect(struct drm_connector *connector, bool force)
{
	if (drm_device_is_unplugged(connector->dev))
		return connector_status_disconnected;

	return connector->status;
}

static void drm_simple_kms_connector_destroy(struct drm_connector *connector)
{
	struct drm_simple_kms_connector *panel_connector;

	panel_connector = to_simple_connector(connector);
	drm_panel_detach(panel_connector->panel);
	drm_panel_remove(panel_connector->panel);
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
	kfree(panel_connector);
}

static const struct drm_connector_funcs drm_simple_kms_connector_funcs = {
	.dpms = drm_atomic_helper_connector_dpms,
	.reset = drm_atomic_helper_connector_reset,
	.detect = drm_simple_kms_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_simple_kms_connector_destroy,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/**
 * drm_simple_kms_panel_connector_create - Create simple connector for panel
 * @dev: DRM device
 * @panel: DRM panel
 * @connector_type: user visible type of the connector
 *
 * Creates a simple connector for a panel.
 * The panel needs to provide a get_modes() function.
 *
 * Returns:
 * Pointer to new connector or ERR_PTR on failure.
 */
struct drm_connector *
drm_simple_kms_panel_connector_create(struct drm_device *dev,
				      struct drm_panel *panel,
				      int connector_type)
{
	struct drm_simple_kms_connector *panel_connector;
	struct drm_connector *connector;
	int ret;

	panel_connector = kzalloc(sizeof(*panel_connector), GFP_KERNEL);
	if (!panel_connector)
		return ERR_PTR(-ENOMEM);

	panel_connector->panel = panel;
	connector = &panel_connector->base;
	drm_connector_helper_add(connector, &drm_simple_kms_connector_helper_funcs);
	ret = drm_connector_init(dev, connector, &drm_simple_kms_connector_funcs,
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
EXPORT_SYMBOL(drm_simple_kms_panel_connector_create);

static void drm_simple_kms_encoder_disable(struct drm_encoder *encoder)
{
}

static void drm_simple_kms_encoder_enable(struct drm_encoder *encoder)
{
}

static const struct drm_encoder_helper_funcs drm_simple_kms_encoder_helper_funcs = {
	.disable = drm_simple_kms_encoder_disable,
	.enable = drm_simple_kms_encoder_enable,
};

static const struct drm_encoder_funcs drm_simple_kms_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static void drm_simple_kms_crtc_enable(struct drm_crtc *crtc)
{
	struct drm_simple_display_pipe *pipe;

	pipe = container_of(crtc, struct drm_simple_display_pipe, crtc);
	if (!pipe->funcs || !pipe->funcs->enable)
		return;

	pipe->funcs->enable(pipe, crtc->state);
}

static void drm_simple_kms_crtc_disable(struct drm_crtc *crtc)
{
	struct drm_simple_display_pipe *pipe;

	pipe = container_of(crtc, struct drm_simple_display_pipe, crtc);
	if (!pipe->funcs || !pipe->funcs->disable)
		return;

	pipe->funcs->disable(pipe);
}

static const struct drm_crtc_helper_funcs drm_simple_kms_crtc_helper_funcs = {
	.disable = drm_simple_kms_crtc_disable,
	.enable = drm_simple_kms_crtc_enable,
};

static const struct drm_crtc_funcs drm_simple_kms_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static void drm_simple_kms_plane_atomic_update(struct drm_plane *plane,
					struct drm_plane_state *old_state)
{
	struct drm_simple_display_pipe *pipe;

	pipe = container_of(plane, struct drm_simple_display_pipe, plane);
	if (!pipe->funcs || !pipe->funcs->plane_update)
		return;

	pipe->funcs->plane_update(pipe, old_state);
}

static const struct drm_plane_helper_funcs drm_simple_kms_plane_helper_funcs = {
	.atomic_update = drm_simple_kms_plane_atomic_update,
};

static const struct drm_plane_funcs drm_simple_kms_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= drm_plane_cleanup,
	.reset			= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state	= drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
};

/**
 * drm_simple_display_pipe_init - Initialize a simple display pipe
 * @dev: DRM device
 * @pipe: simple display pipe object to initialize
 * @funcs: callbacks for the display pipe
 * @formats: array of supported formats (%DRM_FORMAT_*)
 * @format_count: number of elements in @formats
 * @connector: connector to attach and register
 *
 * Sets up a display pipe which consist of a really simple plane-crtc-encoder
 * pipe coupled with the provided connector.
 *
 * Returns:
 * Zero on success, error code on failure.
 */
int drm_simple_display_pipe_init(struct drm_device *dev,
				 struct drm_simple_display_pipe *pipe,
				 struct drm_simple_display_pipe_funcs *funcs,
				 const uint32_t *formats, unsigned int format_count,
				 struct drm_connector *connector)
{
	struct drm_encoder *encoder = &pipe->encoder;
	struct drm_plane *plane = &pipe->plane;
	struct drm_crtc *crtc = &pipe->crtc;
	int ret;

	pipe->funcs = funcs;

	drm_plane_helper_add(plane, &drm_simple_kms_plane_helper_funcs);
	ret = drm_universal_plane_init(dev, plane, 0, &drm_simple_kms_plane_funcs,
				       formats, format_count,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;

	drm_crtc_helper_add(crtc, &drm_simple_kms_crtc_helper_funcs);
	ret = drm_crtc_init_with_planes(dev, crtc, plane, NULL,
					&drm_simple_kms_crtc_funcs, NULL);
	if (ret)
		return ret;

	encoder->possible_crtcs = 1 << drm_crtc_index(crtc);
	drm_encoder_helper_add(encoder, &drm_simple_kms_encoder_helper_funcs);
	ret = drm_encoder_init(dev, encoder, &drm_simple_kms_encoder_funcs,
			       DRM_MODE_ENCODER_NONE, NULL);
	if (ret)
		return ret;

	ret = drm_mode_connector_attach_encoder(connector, encoder);
	if (ret)
		return ret;

	return drm_connector_register(connector);
}
EXPORT_SYMBOL(drm_simple_display_pipe_init);

MODULE_LICENSE("GPL");
