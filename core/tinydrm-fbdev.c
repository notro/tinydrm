/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drmP.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/tinydrm/tinydrm.h>

static int tinydrm_fbdev_fb_dirty(struct drm_framebuffer *fb,
				  struct drm_file *file_priv,
				  unsigned flags, unsigned color,
				  struct drm_clip_rect *clips,
				  unsigned num_clips)
{
	struct drm_gem_cma_object *cma = drm_fb_cma_get_gem_obj(fb, 0);
	struct tinydrm_device *tdev = fb->dev->dev_private;

	if (tdev->pipe.plane.fb != fb)
		return 0;

	return tdev->dirtyfb(fb, cma->vaddr, flags, color, clips, num_clips);
}

static struct drm_framebuffer_funcs tinydrm_fbdev_fb_funcs = {
	.destroy	= drm_fb_cma_destroy,
	.create_handle	= drm_fb_cma_create_handle,
	.dirty		= tinydrm_fbdev_fb_dirty,
};

static int tinydrm_fbdev_create(struct drm_fb_helper *helper,
				struct drm_fb_helper_surface_size *sizes)
{
	struct tinydrm_device *tdev = helper->dev->dev_private;
	int ret;

	ret = drm_fbdev_cma_create_with_funcs(helper, sizes,
					      &tinydrm_fbdev_fb_funcs);
	if (ret)
		return ret;

	if (tdev->fbdefio_delay_ms) {
		unsigned long delay;

		delay = msecs_to_jiffies(tdev->fbdefio_delay_ms);
		helper->fbdev->fbdefio->delay = delay ? delay : 1;
	}

	return 0;
}

static const struct drm_fb_helper_funcs tinydrm_fb_helper_funcs = {
	.fb_probe = tinydrm_fbdev_create,
};

int tinydrm_fbdev_init(struct tinydrm_device *tdev)
{
	struct drm_device *dev = tdev->base;
	struct drm_fbdev_cma *fbdev;

	DRM_DEBUG_KMS("IN\n");

	fbdev = drm_fbdev_cma_init_with_funcs(dev, 16,
					      dev->mode_config.num_crtc,
					      dev->mode_config.num_connector,
					      &tinydrm_fb_helper_funcs);
	if (IS_ERR(fbdev))
		return PTR_ERR(fbdev);

	tdev->fbdev_cma = fbdev;

	DRM_DEBUG_KMS("OUT\n");

	return 0;
}
EXPORT_SYMBOL(tinydrm_fbdev_init);

void tinydrm_fbdev_fini(struct tinydrm_device *tdev)
{
	drm_fbdev_cma_fini(tdev->fbdev_cma);
	tdev->fbdev_cma = NULL;
}
EXPORT_SYMBOL(tinydrm_fbdev_fini);

void tinydrm_fbdev_restore_mode(struct tinydrm_device *tdev)
{
	drm_fbdev_cma_restore_mode(tdev->fbdev_cma);
}
EXPORT_SYMBOL(tinydrm_fbdev_restore_mode);
