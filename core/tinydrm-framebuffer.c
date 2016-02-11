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
#include <drm/drm_gem_cma_helper.h>
#include <drm/tinydrm/tinydrm.h>

static inline struct tinydrm_framebuffer *to_tinydrm_framebuffer(struct drm_framebuffer *fb)
{
	return container_of(fb, struct tinydrm_framebuffer, base);
}

static void tinydrm_framebuffer_destroy(struct drm_framebuffer *fb)
{
	struct tinydrm_framebuffer *tinydrm_fb = to_tinydrm_framebuffer(fb);
	struct tinydrm_device *tdev = fb->dev->dev_private;

	DRM_DEBUG_KMS("drm_framebuffer = %p, tinydrm_fb->obj = %p\n", fb, tinydrm_fb->obj);

	if (tdev->deferred)
		flush_delayed_work(&tdev->deferred->dwork);

	if (tinydrm_fb->obj)
		drm_gem_object_unreference_unlocked(tinydrm_fb->obj);

	drm_framebuffer_cleanup(fb);
	kfree(tinydrm_fb);
}

static int tinydrm_framebuffer_dirty(struct drm_framebuffer *fb,
				     struct drm_file *file_priv,
				     unsigned flags, unsigned color,
				     struct drm_clip_rect *clips,
				     unsigned num_clips)
{
	struct tinydrm_framebuffer *tfb = to_tinydrm_framebuffer(fb);
	struct tinydrm_device *tdev = fb->dev->dev_private;

	dev_dbg(fb->dev->dev, "%s\n", __func__);

	return tdev->fb_dirty(fb, tfb->cma_obj, flags, color, clips, num_clips);
}

static const struct drm_framebuffer_funcs tinydrm_fb_funcs = {
	.destroy = tinydrm_framebuffer_destroy,
	.dirty = tinydrm_framebuffer_dirty,
/*	TODO?
 *	.create_handle = tinydrm_framebuffer_create_handle, */
};

static struct drm_framebuffer *
tinydrm_fb_create(struct drm_device *ddev, struct drm_file *file_priv,
		  struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct tinydrm_framebuffer *tinydrm_fb;
	struct drm_gem_object *obj;
	int ret;

	/* TODO? Validate the pixel format, size and pitches */
	DRM_DEBUG_KMS("pixel_format=%s\n", drm_get_format_name(mode_cmd->pixel_format));
	DRM_DEBUG_KMS("width=%u\n", mode_cmd->width);
	DRM_DEBUG_KMS("height=%u\n", mode_cmd->height);
	DRM_DEBUG_KMS("pitches[0]=%u\n", mode_cmd->pitches[0]);

	obj = drm_gem_object_lookup(ddev, file_priv, mode_cmd->handles[0]);
	if (!obj)
		return NULL;

	tinydrm_fb = kzalloc(sizeof(*tinydrm_fb), GFP_KERNEL);
	if (!tinydrm_fb)
		return NULL;

	tinydrm_fb->obj = obj;
	tinydrm_fb->cma_obj = to_drm_gem_cma_obj(obj);

	ret = drm_framebuffer_init(ddev, &tinydrm_fb->base, &tinydrm_fb_funcs);
	if (ret) {
		kfree(tinydrm_fb);
		drm_gem_object_unreference_unlocked(obj);
		return NULL;
	}

	drm_helper_mode_fill_fb_struct(&tinydrm_fb->base, mode_cmd);

	return &tinydrm_fb->base;
}

static const struct drm_mode_config_funcs tinydrm_mode_config_funcs = {
	.fb_create = tinydrm_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

void tinydrm_mode_config_init(struct tinydrm_device *tdev)
{
	struct drm_device *ddev = tdev->base;

	drm_mode_config_init(ddev);

	ddev->mode_config.min_width = tdev->width;
	ddev->mode_config.min_height = tdev->height;
	ddev->mode_config.max_width = tdev->width;
	ddev->mode_config.max_height = tdev->height;
	ddev->mode_config.funcs = &tinydrm_mode_config_funcs;
}
