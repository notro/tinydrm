//#define DEBUG
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
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/tinydrm/tinydrm.h>

#define DEFAULT_DEFIO_DELAY HZ/30

struct tinydrm_fbdev {
	struct drm_fb_helper fb_helper;
	struct drm_framebuffer fb;
	void *vmem;
};

static inline struct tinydrm_fbdev *helper_to_fbdev(struct drm_fb_helper *helper)
{
	return container_of(helper, struct tinydrm_fbdev, fb_helper);
}

static inline struct tinydrm_fbdev *fb_to_fbdev(struct drm_framebuffer *fb)
{
	return container_of(fb, struct tinydrm_fbdev, fb);
}

static void tinydrm_fbdev_dirty(struct fb_info *info,
				struct drm_clip_rect *clip, bool run_now)
{
	struct drm_fb_helper *helper = info->par;
	struct tinydrm_device *tdev = helper->dev->dev_private;
	struct drm_framebuffer *fb = helper->fb;

	if (tdev->pipe.plane.fb != fb)
		return;

	if (tdev->deferred)
		tdev->deferred->no_delay = run_now;
	tdev->dirtyfb(fb, info->screen_buffer, 0, 0, clip, 1);
}

static void tinydrm_fbdev_deferred_io(struct fb_info *info,
				      struct list_head *pagelist)
{
	unsigned long start, end, next, min, max;
	struct drm_clip_rect clip;
	struct page *page;
int count = 0;

	min = ULONG_MAX;
	max = 0;
	next = 0;
	list_for_each_entry(page, pagelist, lru) {
		start = page->index << PAGE_SHIFT;
		end = start + PAGE_SIZE - 1;
		min = min(min, start);
		max = max(max, end);
count++;
	}

	if (min < max) {
		clip.x1 = 0;
		clip.x2 = info->var.xres - 1;
		clip.y1 = min / info->fix.line_length;
		clip.y2 = min_t(u32, max / info->fix.line_length,
				    info->var.yres - 1);
		pr_debug("%s: x1=%u, x2=%u, y1=%u, y2=%u, count=%d\n", __func__, clip.x1, clip.x2, clip.y1, clip.y2, count);
		tinydrm_fbdev_dirty(info, &clip, true);
	}
}

static void tinydrm_fbdev_fb_fillrect(struct fb_info *info,
				      const struct fb_fillrect *rect)
{
	struct drm_clip_rect clip = {
		.x1 = rect->dx,
		.x2 = rect->dx + rect->width - 1,
		.y1 = rect->dy,
		.y2 = rect->dy + rect->height - 1,
	};

	dev_dbg(info->dev, "%s: dx=%d, dy=%d, width=%d, height=%d\n",
		__func__, rect->dx, rect->dy, rect->width, rect->height);
	sys_fillrect(info, rect);
	tinydrm_fbdev_dirty(info, &clip, false);
}

static void tinydrm_fbdev_fb_copyarea(struct fb_info *info,
				      const struct fb_copyarea *area)
{
	struct drm_clip_rect clip = {
		.x1 = area->dx,
		.x2 = area->dx + area->width - 1,
		.y1 = area->dy,
		.y2 = area->dy + area->height - 1,
	};

	dev_dbg(info->dev, "%s: dx=%d, dy=%d, width=%d, height=%d\n",
		__func__,  area->dx, area->dy, area->width, area->height);
	sys_copyarea(info, area);
	tinydrm_fbdev_dirty(info, &clip, false);
}

static void tinydrm_fbdev_fb_imageblit(struct fb_info *info,
				       const struct fb_image *image)
{
	struct drm_clip_rect clip = {
		.x1 = image->dx,
		.x2 = image->dx + image->width - 1,
		.y1 = image->dy,
		.y2 = image->dy + image->height - 1,
	};

	dev_dbg(info->dev, "%s: dx=%d, dy=%d, width=%d, height=%d\n",
		__func__,  image->dx, image->dy, image->width, image->height);
	sys_imageblit(info, image);
	tinydrm_fbdev_dirty(info, &clip, false);
}

static ssize_t tinydrm_fbdev_fb_write(struct fb_info *info,
				      const char __user *buf, size_t count,
				      loff_t *ppos)
{
	struct drm_clip_rect clip = {
		.x1 = 0,
		.x2 = info->var.xres - 1,
		.y1 = 0,
		.y2 = info->var.yres - 1,
	};
	ssize_t ret;

	dev_dbg(info->dev, "%s:\n", __func__);
	ret = fb_sys_write(info, buf, count, ppos);
	tinydrm_fbdev_dirty(info, &clip, false);

	return ret;
}

static void tinydrm_fbdev_fb_destroy(struct drm_framebuffer *fb)
{
}

static struct drm_framebuffer_funcs tinydrm_fbdev_fb_funcs = {
	.destroy = tinydrm_fbdev_fb_destroy,
};

static int tinydrm_fbdev_create(struct drm_fb_helper *helper,
				struct drm_fb_helper_surface_size *sizes)
{
	struct tinydrm_fbdev *fbdev = helper_to_fbdev(helper);
	struct drm_mode_fb_cmd2 mode_cmd = { 0 };
	struct drm_device *dev = helper->dev;
	struct tinydrm_device *tdev = dev->dev_private;
	struct fb_deferred_io *fbdefio;
	struct drm_framebuffer *fb;
	unsigned int bytes_per_pixel = DIV_ROUND_UP(sizes->surface_bpp, 8);
	struct fb_ops *fbops;
	struct fb_info *fbi;
	size_t size;
	char *screen_buffer;
	int ret;

	mode_cmd.width = sizes->surface_width;
	mode_cmd.height = sizes->surface_height;
	mode_cmd.pitches[0] = sizes->surface_width * bytes_per_pixel;
	mode_cmd.pixel_format = drm_mode_legacy_fb_format(sizes->surface_bpp,
							sizes->surface_depth);
	size = mode_cmd.pitches[0] * mode_cmd.height;

	/*
	 * A per device fbops structure is needed because
	 * fb_deferred_io_cleanup() clears fbops.fb_mmap
	 */
	fbops = devm_kzalloc(dev->dev, sizeof(*fbops), GFP_KERNEL);
	if (!fbops) {
		dev_err(dev->dev, "Failed to allocate fbops\n");
		return -ENOMEM;
	}

	/* A per device structure is needed for individual delays */
	fbdefio = devm_kzalloc(dev->dev, sizeof(*fbdefio), GFP_KERNEL);
	if (!fbdefio) {
		dev_err(dev->dev, "Could not allocate fbdefio\n");
		return -ENOMEM;
	}

	fbi = drm_fb_helper_alloc_fbi(helper);
	if (IS_ERR(fbi)) {
		dev_err(dev->dev, "Could not allocate fbi\n");
		return PTR_ERR(fbi);
	}

	screen_buffer = vzalloc(size);
	if (!screen_buffer) {
		dev_err(dev->dev, "Failed to allocate fbdev screen buffer.\n");
		ret = -ENOMEM;
		goto err_fb_info_destroy;
	}

	fb = &fbdev->fb;
	helper->fb = fb;
	drm_helper_mode_fill_fb_struct(fb, &mode_cmd);
	ret = drm_framebuffer_init(dev, fb, &tinydrm_fbdev_fb_funcs);
	if (ret) {
		dev_err(dev->dev, "failed to init framebuffer: %d\n", ret);
		vfree(screen_buffer);
		goto err_fb_info_destroy;
	}

	DRM_DEBUG_KMS("fbdev FB ID: %d, vmem = %p\n", fb->base.id, fbdev->vmem);

	fbi->par = helper;
	fbi->flags = FBINFO_FLAG_DEFAULT | FBINFO_VIRTFB;
	strcpy(fbi->fix.id, "tinydrm");

	fbops->owner          = THIS_MODULE,
	fbops->fb_fillrect    = tinydrm_fbdev_fb_fillrect,
	fbops->fb_copyarea    = tinydrm_fbdev_fb_copyarea,
	fbops->fb_imageblit   = tinydrm_fbdev_fb_imageblit,
	fbops->fb_write       = tinydrm_fbdev_fb_write,
	fbops->fb_check_var   = drm_fb_helper_check_var,
	fbops->fb_set_par     = drm_fb_helper_set_par,
	fbops->fb_blank       = drm_fb_helper_blank,
	fbops->fb_setcmap     = drm_fb_helper_setcmap,
	fbi->fbops = fbops;

	drm_fb_helper_fill_fix(fbi, fb->pitches[0], fb->depth);
	drm_fb_helper_fill_var(fbi, helper, sizes->fb_width, sizes->fb_height);

	fbdev->vmem = screen_buffer;
	fbi->screen_buffer = screen_buffer;
	fbi->screen_size = size;
	fbi->fix.smem_len = size;

	if (tdev->deferred)
		fbdefio->delay = msecs_to_jiffies(tdev->deferred->defer_ms);
	else
		fbdefio->delay = DEFAULT_DEFIO_DELAY;
	/* delay=0 is turned into delay=HZ, so use 1 as a minimum */
	if (!fbdefio->delay)
		fbdefio->delay = 1;
	fbdefio->deferred_io = tinydrm_fbdev_deferred_io;
	fbi->fbdefio = fbdefio;
	fb_deferred_io_init(fbi);

	return 0;

err_fb_info_destroy:
	drm_fb_helper_release_fbi(helper);

	return ret;
}

static const struct drm_fb_helper_funcs tinydrm_fb_helper_funcs = {
	.fb_probe = tinydrm_fbdev_create,
};

int tinydrm_fbdev_init(struct tinydrm_device *tdev)
{
	struct drm_device *dev = tdev->base;
	struct drm_fb_helper *helper;
	struct tinydrm_fbdev *fbdev;
	int ret;

	DRM_DEBUG_KMS("IN\n");

	fbdev = devm_kzalloc(dev->dev, sizeof(*fbdev), GFP_KERNEL);
	if (!fbdev) {
		dev_err(dev->dev, "Failed to allocate drm fbdev.\n");
		return -ENOMEM;
	}

	helper = &fbdev->fb_helper;

	drm_fb_helper_prepare(dev, helper, &tinydrm_fb_helper_funcs);

	ret = drm_fb_helper_init(dev, helper, 1, 1);
	if (ret < 0) {
		dev_err(dev->dev, "Failed to initialize drm fb helper.\n");
		return ret;
	}

	ret = drm_fb_helper_single_add_all_connectors(helper);
	if (ret < 0) {
		dev_err(dev->dev, "Failed to add connectors.\n");
		goto err_drm_fb_helper_fini;

	}

	ret = drm_fb_helper_initial_config(helper, 16);
	if (ret < 0) {
		dev_err(dev->dev, "Failed to set initial hw configuration.\n");
		goto err_drm_fb_helper_fini;
	}

	tdev->fbdev = fbdev;
	DRM_DEBUG_KMS("OUT\n");

	return 0;

err_drm_fb_helper_fini:
	drm_fb_helper_fini(helper);

	return ret;
}
EXPORT_SYMBOL(tinydrm_fbdev_init);

void tinydrm_fbdev_fini(struct tinydrm_device *tdev)
{
	struct tinydrm_fbdev *fbdev = tdev->fbdev;
	struct drm_fb_helper *fb_helper = &fbdev->fb_helper;

	DRM_DEBUG_KMS("IN\n");

	drm_fb_helper_unregister_fbi(fb_helper);
	fb_deferred_io_cleanup(fb_helper->fbdev);
	drm_fb_helper_release_fbi(fb_helper);
	drm_fb_helper_fini(fb_helper);

	drm_framebuffer_unregister_private(&fbdev->fb);
	drm_framebuffer_cleanup(&fbdev->fb);

	vfree(fbdev->vmem);

	tdev->fbdev = NULL;
	DRM_DEBUG_KMS("OUT\n");
}
EXPORT_SYMBOL(tinydrm_fbdev_fini);

/* TODO: pass tdev instead ? */
void tinydrm_fbdev_restore_mode(struct tinydrm_fbdev *fbdev)
{
	if (fbdev)
		drm_fb_helper_restore_fbdev_mode_unlocked(&fbdev->fb_helper);
}
EXPORT_SYMBOL(tinydrm_fbdev_restore_mode);
