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
#include <drm/drm_modes.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/tinydrm/tinydrm.h>

/**
 * DOC: Display pipeline
 *
 * The display pipeline is very simple and consists of a simple
 * plane-crtc-encoder-connector pipe with only one mode.
 * When the pipeline is enabled, the &tinydrm_funcs ->prepare hook is called
 * if it is in the unprepared state. The ->enable hook is called after the
 * first ->dirty call.
 * When the framebuffer is changed, a worker is started to flush it making
 * sure that the display is in sync.
 * The pipeline is initialized using tinydrm_display_pipe_init().
 */

static int tinydrm_connector_get_modes(struct drm_connector *connector)
{
	struct tinydrm_device *tdev = connector->dev->dev_private;
	struct drm_display_mode *mode;

	mode = drm_cvt_mode(connector->dev, tdev->width, tdev->height, 60,
			    false, false, false);
	if (!mode)
		return 0;

	mode->type |= DRM_MODE_TYPE_PREFERRED;
	mode->width_mm = tdev->width_mm;
	mode->height_mm = tdev->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_connector_helper_funcs tinydrm_connector_hfuncs = {
	.get_modes = tinydrm_connector_get_modes,
	.best_encoder = drm_atomic_helper_best_encoder,
};

static enum drm_connector_status
tinydrm_connector_detect(struct drm_connector *connector, bool force)
{
	if (drm_device_is_unplugged(connector->dev))
		return connector_status_disconnected;

	return connector->status;
}

static void tinydrm_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static const struct drm_connector_funcs tinydrm_connector_funcs = {
	.dpms = drm_atomic_helper_connector_dpms,
	.reset = drm_atomic_helper_connector_reset,
	.detect = tinydrm_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = tinydrm_connector_destroy,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static inline struct tinydrm_device *
pipe_to_tinydrm(struct drm_simple_display_pipe *pipe)
{
	return container_of(pipe, struct tinydrm_device, pipe);
}

static void tinydrm_display_pipe_enable(struct drm_simple_display_pipe *pipe,
					struct drm_crtc_state *crtc_state)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	int ret = 0;

	DRM_DEBUG_KMS("\n");

	mutex_lock(&tdev->dev_lock);

	if (tdev->funcs && tdev->funcs->prepare)
		ret = tdev->funcs->prepare(tdev);

	if (ret)
		DRM_ERROR("Failed to enable pipeline: %d\n", ret);
	else
		tdev->prepared = true;

	/* .enable() is called after the first .dirty() */

	mutex_unlock(&tdev->dev_lock);
}

static void tinydrm_display_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);

	DRM_DEBUG_KMS("\n");

	mutex_lock(&tdev->dev_lock);

	if (tdev->enabled && tdev->funcs && tdev->funcs->disable)
		tdev->funcs->disable(tdev);
	tdev->enabled = false;

	if (tdev->prepared && tdev->funcs && tdev->funcs->unprepare)
		tdev->funcs->unprepare(tdev);
	tdev->prepared = false;

	mutex_unlock(&tdev->dev_lock);
}

static void tinydrm_dirty_work(struct work_struct *work)
{
	struct tinydrm_device *tdev = container_of(work, struct tinydrm_device,
						  dirty_work);
	struct drm_framebuffer *fb = tdev->pipe.plane.fb;

	if (fb)
		fb->funcs->dirty(fb, NULL, 0, 0, NULL, 0);
}

static void tinydrm_display_pipe_update(struct drm_simple_display_pipe *pipe,
					struct drm_plane_state *old_state)
{
	struct drm_framebuffer *fb = pipe->plane.state->fb;

	if (fb && (fb != old_state->fb)) {
		struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);

		DRM_DEBUG_KMS("Flush framebuffer [FB:%d]\n", fb->base.id);
		pipe->plane.fb = fb;
		schedule_work(&tdev->dirty_work);
	}
}

static const struct drm_simple_display_pipe_funcs tinydrm_display_pipe_funcs = {
	.enable = tinydrm_display_pipe_enable,
	.disable = tinydrm_display_pipe_disable,
	.update = tinydrm_display_pipe_update,
};

/**
 * tinydrm_display_pipe_init - initialize tinydrm display pipe
 * @tdev: tinydrm device
 * @formats: array of supported formats (%DRM_FORMAT_*)
 * @format_count: number of elements in @formats
 *
 * Sets up a display pipeline which consist of one &drm_plane, one &drm_crtc,
 * one &drm_encoder and one &drm_connector with one static &drm_display_mode.
 */
int tinydrm_display_pipe_init(struct tinydrm_device *tdev,
			      const uint32_t *formats,
			      unsigned int format_count)
{
	struct drm_device *dev = tdev->base;
	struct drm_connector *connector = &tdev->connector;
	int ret;

	mutex_init(&tdev->dev_lock);
	INIT_WORK(&tdev->dirty_work, tinydrm_dirty_work);

	drm_connector_helper_add(connector, &tinydrm_connector_hfuncs);
	ret = drm_connector_init(dev, connector, &tinydrm_connector_funcs,
				 DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret)
		return ret;

	connector->status = connector_status_connected;

	return drm_simple_display_pipe_init(dev, &tdev->pipe,
					    &tinydrm_display_pipe_funcs,
					    formats, format_count,
					    connector);
}
EXPORT_SYMBOL(tinydrm_display_pipe_init);
