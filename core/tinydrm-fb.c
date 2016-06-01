/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/tinydrm/tinydrm.h>

/**
 * DOC: Framebuffer
 *
 * The tinydrm &drm_framebuffer is backed by a &drm_gem_cma_object buffer
 * object. Userspace creates this buffer by calling the
 * DRM_IOCTL_MODE_CREATE_DUMB ioctl. To flush the buffer to the display,
 * userpace calls the DRM_IOCTL_MODE_DIRTYFB ioctl on the framebuffer which
 * in turn calls the &tinydrm_funcs ->dirty hook.
 *
 * This functionality is available by using tinydrm_fb_create() as the
 * &drm_mode_config_funcs ->fb_create callback.
 */

static unsigned int fbdefio_delay;
module_param(fbdefio_delay, uint, 0);
MODULE_PARM_DESC(fbdefio_delay, "fbdev deferred io delay in milliseconds");

static int tinydrm_fb_dirty(struct drm_framebuffer *fb,
			    struct drm_file *file_priv,
			    unsigned flags, unsigned color,
			    struct drm_clip_rect *clips,
			    unsigned num_clips)
{
	struct drm_gem_cma_object *cma_obj = drm_fb_cma_get_gem_obj(fb, 0);
	struct tinydrm_device *tdev = fb->dev->dev_private;
	int ret = 0;

	if (!tdev->funcs || !tdev->funcs->dirty)
		return -ENOSYS;

	mutex_lock(&tdev->dev_lock);

	if (!tdev->prepared) {
		ret = -EINVAL;
		goto out_unlock;
	}

	/* fbdev can flush even when we're not interested */
	if (tdev->pipe.plane.fb != fb)
		goto out_unlock;

	ret = tdev->funcs->dirty(fb, cma_obj, flags, color, clips, num_clips);
	if (ret)
		goto out_unlock;

	if (!tdev->enabled) {
		if (tdev->funcs && tdev->funcs->enable)
			if (tdev->funcs->enable(tdev))
				DRM_ERROR("Failed to enable display\n");
		tdev->enabled = true;
	}

out_unlock:
	mutex_unlock(&tdev->dev_lock);

	return ret;
}

static const struct drm_framebuffer_funcs tinydrm_fb_funcs = {
	.destroy	= drm_fb_cma_destroy,
	.create_handle	= drm_fb_cma_create_handle,
	.dirty		= tinydrm_fb_dirty,
};

/**
 * tinydrm_fb_create - tinydrm .fb_create() helper
 * @drm: DRM device
 * @file_priv: DRM file info
 * @mode_cmd: metadata from the userspace fb creation request
 *
 * Helper for the &drm_mode_config_funcs ->fb_create callback.
 * It sets up a &drm_framebuffer backed by the &drm_gem_cma_object buffer
 * object provided in @mode_cmd.
 */
struct drm_framebuffer *
tinydrm_fb_create(struct drm_device *drm, struct drm_file *file_priv,
		  const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct drm_framebuffer *fb;

	fb = drm_fb_cma_create_with_funcs(drm, file_priv, mode_cmd,
					  &tinydrm_fb_funcs);
	if (!IS_ERR(fb))
		DRM_DEBUG_KMS("[FB:%d] pixel_format: %s\n", fb->base.id,
			      drm_get_format_name(fb->pixel_format));

	return fb;
}
EXPORT_SYMBOL(tinydrm_fb_create);

/**
 * DOC: fbdev emulation
 *
 * tinydrm provides fbdev emulation using the drm_fb_cma_helper library.
 * It is backed by it's own &drm_framebuffer and CMA buffer object.
 * Framebuffer flushing is handled by the fb helper library which in turn
 * calls the &tinydrm_funcs ->dirty hook.
 *
 * fbdev support is initialized using tinydrm_fbdev_init().
 *
 * The tinydrm_lastclose() function ensures that fbdev operation is restored
 * when userspace closes the drm device.
 */

static int tinydrm_fbdev_create(struct drm_fb_helper *helper,
				struct drm_fb_helper_surface_size *sizes)
{
	struct tinydrm_device *tdev = helper->dev->dev_private;
	int ret;

	ret = drm_fbdev_cma_create_with_funcs(helper, sizes,
					      &tinydrm_fb_funcs);
	if (ret)
		return ret;

	DRM_DEBUG_KMS("[FB:%d] pixel_format: %s\n", helper->fb->base.id,
		      drm_get_format_name(helper->fb->pixel_format));
	strncpy(helper->fbdev->fix.id, helper->dev->driver->name, 16);
	tdev->fbdev_helper = helper;

	if (fbdefio_delay) {
		unsigned long delay;

		delay = msecs_to_jiffies(fbdefio_delay);
		helper->fbdev->fbdefio->delay = delay ? delay : 1;
	}

	return 0;
}

static const struct drm_fb_helper_funcs tinydrm_fb_helper_funcs = {
	.fb_probe = tinydrm_fbdev_create,
};

/**
 * tinydrm_fbdev_init - initialize tinydrm fbdev emulation
 * @tdev: tinydrm device
 *
 * Initialize tinydrm fbdev emulation. Tear down with tinydrm_fbdev_fini().
 * If &mode_config ->preferred_depth is set it is used as preferred bpp.
 */
int tinydrm_fbdev_init(struct tinydrm_device *tdev)
{
	struct drm_device *drm = tdev->base;
	struct drm_fbdev_cma *fbdev;
	int bpp;

	DRM_DEBUG_KMS("\n");

	bpp = drm->mode_config.preferred_depth;
	fbdev = drm_fbdev_cma_init_with_funcs(drm, bpp ? bpp : 32,
					      drm->mode_config.num_crtc,
					      drm->mode_config.num_connector,
					      &tinydrm_fb_helper_funcs);
	if (IS_ERR(fbdev))
		return PTR_ERR(fbdev);

	tdev->fbdev_cma = fbdev;

	return 0;
}
EXPORT_SYMBOL(tinydrm_fbdev_init);

/**
 * tinydrm_fbdev_fini - finalize tinydrm fbdev emulation
 * @tdev: tinydrm device
 *
 * This function tears down the fbdev emulation
 */
void tinydrm_fbdev_fini(struct tinydrm_device *tdev)
{
	drm_fbdev_cma_fini(tdev->fbdev_cma);
	tdev->fbdev_cma = NULL;
	tdev->fbdev_helper = NULL;
}
EXPORT_SYMBOL(tinydrm_fbdev_fini);
