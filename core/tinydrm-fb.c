/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drm_fb_cma_helper.h>
#include <drm/tinydrm/tinydrm.h>

struct drm_framebuffer *
tinydrm_fb_create(struct drm_device *drm, struct drm_file *file_priv,
		  const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct tinydrm_device *tdev = drm->dev_private;

	return drm_fb_cma_create_with_funcs(drm, file_priv, mode_cmd,
					    tdev->fb_funcs);
}

int tinydrm_fbdev_init(struct tinydrm_device *tdev)
{
	struct drm_device *drm = tdev->drm;
	struct drm_fbdev_cma *fbdev;
	int bpp;

	DRM_DEBUG_KMS("\n");

	bpp = drm->mode_config.preferred_depth;
	fbdev = drm_fbdev_cma_init_with_funcs(drm, bpp ? bpp : 32,
					      drm->mode_config.num_crtc,
					      drm->mode_config.num_connector,
					      tdev->fb_funcs);
	if (IS_ERR(fbdev))
		return PTR_ERR(fbdev);

	tdev->fbdev_cma = fbdev;

	return 0;
}

void tinydrm_fbdev_fini(struct tinydrm_device *tdev)
{
	if (!tdev->fbdev_cma)
		return;

	drm_fbdev_cma_fini(tdev->fbdev_cma);
	tdev->fbdev_cma = NULL;
}
