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
