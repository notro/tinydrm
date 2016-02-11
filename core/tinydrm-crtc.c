/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drmP.h>
#include <drm/drm_crtc_helper.h>
#include <drm/tinydrm/tinydrm.h>

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

static struct drm_encoder *tinydrm_connector_best_encoder(struct drm_connector *connector)
{
	return drm_encoder_find(connector->dev, connector->encoder_ids[0]);
}

static const struct drm_connector_helper_funcs tinydrm_connector_helper_funcs = {
	.get_modes = tinydrm_connector_get_modes,
	.best_encoder = tinydrm_connector_best_encoder,
};

static void tinydrm_crtc_enable(struct drm_crtc *crtc)
{
	struct tinydrm_device *tdev = crtc->dev->dev_private;

	DRM_DEBUG_KMS("prepared=%u, enabled=%u\n", tdev->prepared, tdev->enabled);

	if (!tdev->prepared) {
		drm_panel_prepare(&tdev->panel);
		tdev->prepared = true;
	}

	/* The panel is enabled after the first display update */
}

static void tinydrm_crtc_disable(struct drm_crtc *crtc)
{
	struct tinydrm_device *tdev = crtc->dev->dev_private;

	DRM_DEBUG_KMS("prepared=%u, enabled=%u\n", tdev->prepared, tdev->enabled);

	if (tdev->enabled) {
		drm_panel_disable(&tdev->panel);
		tdev->enabled = false;
	}

	if (tdev->prepared) {
		drm_panel_unprepare(&tdev->panel);
		tdev->prepared = false;
	}
}

static const struct drm_crtc_helper_funcs tinydrm_crtc_helper_funcs = {
	.disable = tinydrm_crtc_disable,
	.enable = tinydrm_crtc_enable,
};

int tinydrm_crtc_create(struct tinydrm_device *tdev)
{
	return tinydrm_simple_crtc_create(tdev->base, &tdev->plane, NULL,
		&tinydrm_crtc_helper_funcs, &tinydrm_connector_helper_funcs);
}
