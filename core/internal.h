/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

int tinydrm_crtc_create(struct tinydrm_device *tdev);

static inline bool tinydrm_active(struct tinydrm_device *tdev)
{
	struct drm_crtc *crtc;

	drm_for_each_crtc(crtc, tdev->base)
		return crtc->state && crtc->state->active;

	return false;
}

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
