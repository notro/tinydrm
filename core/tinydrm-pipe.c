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

struct tinydrm_connector {
	struct drm_connector base;
	struct drm_display_mode *mode;
};

static inline struct tinydrm_connector *
to_tinydrm_connector(struct drm_connector *connector)
{
	return container_of(connector, struct tinydrm_connector, base);
}

static int tinydrm_connector_get_modes(struct drm_connector *connector)
{
	struct tinydrm_connector *tconn = to_tinydrm_connector(connector);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, tconn->mode);
	if (!mode) {
		DRM_ERROR("Failed to duplicate mode\n");
		return 0;
	}

	if (mode->name[0] == '\0')
		drm_mode_set_name(mode);

	mode->type |= DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	if (mode->width_mm) {
		connector->display_info.width_mm = mode->width_mm;
		connector->display_info.height_mm = mode->height_mm;
	}

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
	struct tinydrm_connector *tconn = to_tinydrm_connector(connector);

	drm_mode_destroy(connector->dev, tconn->mode);
	drm_connector_cleanup(connector);
	kfree(tconn);
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

static struct drm_connector *
tinydrm_connector_create(struct drm_device *drm,
			 const struct drm_display_mode *mode,
			 int connector_type)
{
	struct tinydrm_connector *tconn;
	struct drm_connector *connector;
	struct drm_display_mode *mode_copy;
	int ret;

	tconn = kzalloc(sizeof(*tconn), GFP_KERNEL);
	if (!tconn)
		return ERR_PTR(-ENOMEM);

	mode_copy = drm_mode_duplicate(drm, mode);
	if (!mode_copy) {
		ret = -ENOMEM;
		goto err_free;
	}

	tconn->mode = mode_copy;
	connector = &tconn->base;

	drm_connector_helper_add(connector, &tinydrm_connector_hfuncs);
	ret = drm_connector_init(drm, connector, &tinydrm_connector_funcs,
				 connector_type);
	if (ret)
		goto err_destroy_mode;

	connector->status = connector_status_connected;

	return connector;

err_destroy_mode:
	drm_mode_destroy(drm, mode_copy);
err_free:
	kfree(tconn);

	return ERR_PTR(ret);
}

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
 * @mode: supported mode (a copy is made so this can be freed afterwards)
 *
 * Sets up a display pipeline which consist of one &drm_plane, one &drm_crtc,
 * one &drm_encoder and one &drm_connector with one &drm_display_mode.
 */
int tinydrm_display_pipe_init(struct tinydrm_device *tdev,
			      const uint32_t *formats,
			      unsigned int format_count,
			      const struct drm_display_mode *mode)
{
	struct drm_device *drm = tdev->base;
	struct drm_connector *connector;

	INIT_WORK(&tdev->dirty_work, tinydrm_dirty_work);

	connector = tinydrm_connector_create(drm, mode,
					     DRM_MODE_CONNECTOR_VIRTUAL);
	if (IS_ERR(connector))
		return PTR_ERR(connector);

	return drm_simple_display_pipe_init(drm, &tdev->pipe,
					    &tinydrm_display_pipe_funcs,
					    formats, format_count,
					    connector);
}
EXPORT_SYMBOL(tinydrm_display_pipe_init);
