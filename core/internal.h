/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

int tinydrm_schedule_dirty(struct drm_framebuffer *fb,
			   struct drm_gem_cma_object *cma_obj, unsigned flags,
			   unsigned color, struct drm_clip_rect *clips,
			   unsigned num_clips, bool run_now);

#ifdef CONFIG_DRM_KMS_FB_HELPER
int tinydrm_fbdev_init(struct tinydrm_device *tdev);
void tinydrm_fbdev_fini(struct tinydrm_device *tdev);

static inline void tinydrm_fbdev_cma_restore_mode(struct drm_fbdev_cma *cma)
{
	drm_fbdev_cma_restore_mode(cma);
}

#else
static inline int tinydrm_fbdev_init(struct tinydrm_device *tdev)
{
	return 0;
}

static inline void tinydrm_fbdev_fini(struct tinydrm_device *tdev) { }

static inline void tinydrm_fbdev_cma_restore_mode(struct drm_fbdev_cma *cma)
{
}
#endif
