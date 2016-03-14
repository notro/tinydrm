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
#include <drm/tinydrm/tinydrm.h>
#include <linux/slab.h>

#include "internal.h"

static int tinydrm_connector_get_modes(struct drm_connector *connector)
{
	struct tinydrm_device *tdev = connector->dev->dev_private;
	struct drm_display_mode *mode;
	int ret;

	DRM_DEBUG_KMS("\n");
	ret = drm_panel_get_modes(&tdev->panel);
	if (ret > 0)
		return ret;

	mode = drm_cvt_mode(connector->dev, tdev->width, tdev->height, 60, false, false, false);
	if (!mode)
		return 0;

	mode->type |= DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static struct drm_encoder *
tinydrm_connector_best_encoder(struct drm_connector *connector)
{
	return drm_encoder_find(connector->dev, connector->encoder_ids[0]);
}

static const struct drm_connector_helper_funcs tinydrm_connector_helper_funcs = {
	.get_modes = tinydrm_connector_get_modes,
	.best_encoder = tinydrm_connector_best_encoder,
};

static enum drm_connector_status
tinydrm_connector_detect(struct drm_connector *connector, bool force)
{
	DRM_DEBUG_KMS("status = %d\n", connector->status);

	if (drm_device_is_unplugged(connector->dev))
		return connector_status_disconnected;

	return connector->status;
}

static void tinydrm_connector_destroy(struct drm_connector *connector)
{
	DRM_DEBUG_KMS("\n");
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
	kfree(connector);
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

static void tinydrm_encoder_disable(struct drm_encoder *encoder)
{
}

static void tinydrm_encoder_enable(struct drm_encoder *encoder)
{
}

static int tinydrm_encoder_atomic_check(struct drm_encoder *encoder,
					struct drm_crtc_state *crtc_state,
					struct drm_connector_state *conn_state)
{
	return 0;
}

static const struct drm_encoder_helper_funcs tinydrm_encoder_helper_funcs = {
	.disable = tinydrm_encoder_disable,
	.enable = tinydrm_encoder_enable,
	.atomic_check = tinydrm_encoder_atomic_check,
};

static void tinydrm_encoder_cleanup(struct drm_encoder *encoder)
{
	DRM_DEBUG_KMS("\n");
	drm_encoder_cleanup(encoder);
	kfree(encoder);
}

static const struct drm_encoder_funcs tinydrm_encoder_funcs = {
	.destroy = tinydrm_encoder_cleanup,
};

static void tinydrm_crtc_enable(struct drm_crtc *crtc)
{
	struct tinydrm_device *tdev = crtc->dev->dev_private;

	DRM_DEBUG_KMS("prepared=%u, enabled=%u\n", tdev->prepared, tdev->enabled);

	/* The panel must be prepared on the first crtc enable after probe */
	tinydrm_prepare(tdev);
	/* The panel is enabled after the first display update */
}

static void tinydrm_crtc_disable(struct drm_crtc *crtc)
{
	struct tinydrm_device *tdev = crtc->dev->dev_private;

	DRM_DEBUG_KMS("prepared=%u, enabled=%u\n", tdev->prepared, tdev->enabled);

	tinydrm_disable(tdev);
}

static const struct drm_crtc_helper_funcs tinydrm_crtc_helper_funcs = {
	.disable = tinydrm_crtc_disable,
	.enable = tinydrm_crtc_enable,
};

static void tinydrm_crtc_cleanup(struct drm_crtc *crtc)
{
	DRM_DEBUG_KMS("\n");
	drm_crtc_cleanup(crtc);
	kfree(crtc);
}

static const struct drm_crtc_funcs tinydrm_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = tinydrm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

int tinydrm_crtc_create(struct tinydrm_device *tdev)
{
	struct drm_device *dev = tdev->base;
	struct drm_connector *connector;
	struct drm_encoder *encoder;
	struct drm_crtc *crtc;
	int ret;

	connector = kzalloc(sizeof(*connector), GFP_KERNEL);
	encoder = kzalloc(sizeof(*encoder), GFP_KERNEL);
	crtc = kzalloc(sizeof(*crtc), GFP_KERNEL);
	if (!connector || !encoder || !crtc) {
		ret = -ENOMEM;
		goto error_free;
	}

	drm_crtc_helper_add(crtc, &tinydrm_crtc_helper_funcs);
	ret = drm_crtc_init_with_planes(dev, crtc, &tdev->plane, NULL,
					&tinydrm_crtc_funcs);
	if (ret)
		goto error_free;

	encoder->possible_crtcs = 1 << drm_crtc_index(crtc);
	drm_encoder_helper_add(encoder, &tinydrm_encoder_helper_funcs);
	ret = drm_encoder_init(dev, encoder, &tinydrm_encoder_funcs,
			       DRM_MODE_ENCODER_NONE);
	if (ret)
		goto error_free;

	drm_connector_helper_add(connector, &tinydrm_connector_helper_funcs);
	ret = drm_connector_init(dev, connector, &tinydrm_connector_funcs,
				 DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret)
		goto error_free;

	ret = drm_mode_connector_attach_encoder(connector, encoder);
	if (ret)
		goto error_free;

	ret = drm_connector_register(connector);
	if (ret)
		goto error_free;

	return 0;

error_free:
	kfree(crtc);
	kfree(encoder);
	kfree(connector);

	return ret;
}
