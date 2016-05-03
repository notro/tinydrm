/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __LINUX_DRM_SIMPLE_KMS_HELPER_H
#define __LINUX_DRM_SIMPLE_KMS_HELPER_H

struct drm_simple_display_pipe;

struct drm_simple_display_pipe_funcs {
	void (*enable)(struct drm_simple_display_pipe *pipe,
		       struct drm_crtc_state *crtc_state);
	void (*disable)(struct drm_simple_display_pipe *pipe);
	void (*plane_update)(struct drm_simple_display_pipe *pipe,
			     struct drm_plane_state *plane_state);
};

struct drm_simple_display_pipe {
	struct drm_crtc crtc;
	struct drm_plane plane;
	struct drm_encoder encoder;
	struct drm_connector *connector;

	struct drm_simple_display_pipe_funcs *funcs;
};

int drm_simple_display_pipe_init(struct drm_device *dev,
				 struct drm_simple_display_pipe *pipe,
				 struct drm_simple_display_pipe_funcs *funcs,
				 const uint32_t *formats, unsigned int format_count,
				 struct drm_connector *connector);

#endif /* __LINUX_DRM_SIMPLE_KMS_HELPER_H */
