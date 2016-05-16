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
#include <drm/tinydrm/tinydrm.h>

static int tinydrm_fb_dirty(struct drm_framebuffer *fb,
			    struct drm_file *file_priv,
			    unsigned flags, unsigned color,
			    struct drm_clip_rect *clips,
			    unsigned num_clips)
{
	struct drm_gem_cma_object *cma = drm_fb_cma_get_gem_obj(fb, 0);
	struct tinydrm_device *tdev = fb->dev->dev_private;
	int ret;

	if (!tdev->funcs || !tdev->funcs->dirty)
		return -ENOSYS;

	if (!tdev->prepared)
		return -EINVAL;

	if (WARN_ON_ONCE(!cma->vaddr))
		return -EINVAL;

	mutex_lock(&tdev->dirty_lock);

	/* fbdev can flush even when we're not interested */
	if (tdev->pipe.plane.fb != fb)
		goto out_unlock;

	ret = tdev->funcs->dirty(fb, cma->vaddr, flags, color, clips, num_clips);
	if (ret)
		return ret;

	tinydrm_enable(tdev);

out_unlock:
	mutex_unlock(&tdev->dirty_lock);

	return 0;
}

const struct drm_framebuffer_funcs tinydrm_fb_funcs = {
	.destroy	= drm_fb_cma_destroy,
	.create_handle	= drm_fb_cma_create_handle,
	.dirty		= tinydrm_fb_dirty,
};

struct drm_framebuffer *tinydrm_fb_create(struct drm_device *dev,
				struct drm_file *file_priv,
				const struct drm_mode_fb_cmd2 *mode_cmd)
{
	return drm_fb_cma_create_with_funcs(dev, file_priv, mode_cmd,
					    &tinydrm_fb_funcs);
}
EXPORT_SYMBOL(tinydrm_fb_create);
