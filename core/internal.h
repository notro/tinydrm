/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

struct drm_connector_helper_funcs;
struct drm_crtc_helper_funcs;

struct drm_crtc *tinydrm_get_first_crtc(struct drm_device *dev);
struct drm_connector *tinydrm_get_first_connector(struct drm_device *dev);
int tinydrm_simple_crtc_create(struct drm_device *dev,
	struct drm_plane *primary, struct drm_plane *cursor,
	const struct drm_crtc_helper_funcs *crtc_helper_funcs,
	const struct drm_connector_helper_funcs *connector_helper_funcs);
int tinydrm_crtc_create(struct tinydrm_device *tdev);

static inline bool tinydrm_active(struct tinydrm_device *tdev)
{
	struct drm_crtc *crtc = tinydrm_get_first_crtc(tdev->base);

	return crtc && crtc->state && crtc->state->active;
}

void tinydrm_mode_config_init(struct tinydrm_device *tdev);

int tinydrm_plane_init(struct tinydrm_device *tdev);

#ifdef CONFIG_DRM_KMS_FB_HELPER
int tinydrm_fbdev_init(struct tinydrm_device *tdev);
void tinydrm_fbdev_fini(struct tinydrm_device *tdev);
void tinydrm_fbdev_restore_mode(struct tinydrm_fbdev *fbdev);
#else
static inline int tinydrm_fbdev_init(struct tinydrm_device *tdev)
{
	return 0;
}

static inline void tinydrm_fbdev_fini(struct tinydrm_device *tdev)
{
}

static inline void tinydrm_fbdev_restore_mode(struct tinydrm_fbdev *fbdev)
{
}
#endif
