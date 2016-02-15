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
#include <linux/slab.h>

struct drm_crtc *tinydrm_get_first_crtc(struct drm_device *dev)
{
	struct drm_crtc *crtc;

	drm_for_each_crtc(crtc, dev)
		return crtc;

	return NULL;
}

struct drm_connector *tinydrm_get_first_connector(struct drm_device *dev)
{
	struct drm_connector *connector;

	list_for_each_entry(connector, &dev->mode_config.connector_list, head)
		return connector;

	return NULL;
}

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

/*
 * TODO: Check if this code is needed. Copied from another driver
 *       If needed maybe it can be moved to crtc atomic check
 */
static int tinydrm_encoder_atomic_check(struct drm_encoder *encoder,
					struct drm_crtc_state *crtc_state,
					struct drm_connector_state *conn_state)
{
	struct drm_display_mode *adjusted_mode = &crtc_state->adjusted_mode;
	const struct drm_display_mode *mode = &crtc_state->mode;
	struct drm_connector *connector = conn_state->connector;
	const struct drm_display_mode *panel_mode;
	struct drm_device *ddev = encoder->dev;

	if (list_empty(&connector->modes)) {
		dev_dbg(ddev->dev, "encoder: empty modes list\n");
		return -EINVAL;
	}

	panel_mode = list_first_entry(&connector->modes,
				      struct drm_display_mode, head);

	/* We're not allowed to modify the resolution. */
	if (mode->hdisplay != panel_mode->hdisplay ||
	    mode->vdisplay != panel_mode->vdisplay)
		return -EINVAL;

	/* The flat panel mode is fixed, just copy it to the adjusted mode. */
	drm_mode_copy(adjusted_mode, panel_mode);

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

int tinydrm_simple_crtc_create(struct drm_device *dev,
	struct drm_plane *primary, struct drm_plane *cursor,
	const struct drm_crtc_helper_funcs *crtc_helper_funcs,
	const struct drm_connector_helper_funcs *connector_helper_funcs)
{
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

	drm_crtc_helper_add(crtc, crtc_helper_funcs);
	ret = drm_crtc_init_with_planes(dev, crtc, primary, cursor,
					&tinydrm_crtc_funcs);
	if (ret)
		goto error_free;

	encoder->possible_crtcs = 1 << drm_crtc_index(crtc);
	drm_encoder_helper_add(encoder, &tinydrm_encoder_helper_funcs);
	ret = drm_encoder_init(dev, encoder, &tinydrm_encoder_funcs, DRM_MODE_ENCODER_VIRTUAL);
	if (ret)
		goto error_free;

	drm_connector_helper_add(connector, connector_helper_funcs);
	ret = drm_connector_init(dev, connector, &tinydrm_connector_funcs, DRM_MODE_CONNECTOR_VIRTUAL);
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
