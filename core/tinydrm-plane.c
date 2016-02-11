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
#include <drm/drm_crtc.h>
#include <drm/drm_plane_helper.h>
#include <drm/tinydrm/tinydrm.h>

/* TODO: Configurable */
static const uint32_t tinydrm_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

static void tinydrm_plane_atomic_update(struct drm_plane *plane,
					struct drm_plane_state *old_state)
{
	DRM_DEBUG("handle 0x%x, crtc %dx%d+%d+%d\n", 0,
		  plane->state->crtc_w, plane->state->crtc_h,
		  plane->state->crtc_x, plane->state->crtc_y);
}

static const struct drm_plane_helper_funcs tinydrm_plane_helper_funcs = {
	.atomic_update = tinydrm_plane_atomic_update,
};

static const struct drm_plane_funcs tinydrm_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= drm_plane_cleanup,
	.reset			= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state	= drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
};

int tinydrm_plane_init(struct tinydrm_device *tdev)
{
	drm_plane_helper_add(&tdev->plane, &tinydrm_plane_helper_funcs);
	return drm_universal_plane_init(tdev->base, &tdev->plane, 0,
					&tinydrm_plane_funcs, tinydrm_formats,
					ARRAY_SIZE(tinydrm_formats),
					DRM_PLANE_TYPE_PRIMARY);
}
