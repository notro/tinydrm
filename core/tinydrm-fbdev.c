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
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/tinydrm/tinydrm.h>

#include "internal.h"

#define DEFAULT_DEFIO_DELAY HZ/30

static void tinydrm_fbdev_dirty(struct fb_info *info,
				struct drm_clip_rect *clip, bool run_now)
{
	struct drm_fb_helper *helper = info->par;
	struct tinydrm_device *tdev = helper->dev->dev_private;
	struct drm_framebuffer *fb = helper->fb;
	struct drm_gem_cma_object *cma_obj;

	if (tdev->plane.fb != fb)
		return;

	cma_obj = drm_fb_cma_get_gem_obj(fb, 0);
	if (!cma_obj) {
		dev_err_once(info->dev, "Can't get cma_obj\n");
		return;
	}

	if (tdev->deferred)
		tdev->deferred->no_delay = run_now;
	tdev->fb_dirty(fb, cma_obj, 0, 0, clip, 1);
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

pr_debug("%s: count=%d\n", __func__, count);

	if (min < max) {
		clip.x1 = 0;
		clip.x2 = info->var.xres - 1;
		clip.y1 = min / info->fix.line_length;
		clip.y2 = min_t(u32, max / info->fix.line_length,
				    info->var.yres - 1);
//		pr_debug("%s: x1=%u, x2=%u, y1=%u, y2=%u\n", __func__, clips[i].x1, clips[i].x2, clips[i].y1, clips[i].y2);
		tinydrm_fbdev_dirty(info, &clip, true);
	}
}

static void tinydrm_fbdev_fillrect(struct fb_info *info,
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

static void tinydrm_fbdev_copyarea(struct fb_info *info,
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

static void tinydrm_fbdev_imageblit(struct fb_info *info,
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

static struct fb_ops tinydrm_fbdev_cma_ops = {
	.owner          = THIS_MODULE,
	.fb_fillrect    = tinydrm_fbdev_fillrect,
	.fb_copyarea    = tinydrm_fbdev_copyarea,
	.fb_imageblit   = tinydrm_fbdev_imageblit,

	.fb_check_var   = drm_fb_helper_check_var,
	.fb_set_par     = drm_fb_helper_set_par,
	.fb_blank       = drm_fb_helper_blank,
	.fb_pan_display = drm_fb_helper_pan_display,
	.fb_setcmap     = drm_fb_helper_setcmap,
};

static int tinydrm_fbdev_event_notify(struct notifier_block *self,
				      unsigned long action, void *data)
{
	struct fb_event *event = data;
	struct fb_info *info = event->info;
	struct drm_fb_helper *helper = info->par;
	struct tinydrm_device *tdev = helper->dev->dev_private;
	unsigned long delay = DEFAULT_DEFIO_DELAY;
	struct fb_ops *fbops;

	DRM_DEBUG("info=%p\n", info);
	DRM_DEBUG("helper->fbdev=%p\n", helper->fbdev);
	DRM_DEBUG("helper->dev=%p\n", helper->dev);
	DRM_DEBUG("tdev->base=%p\n", tdev->base);

	/* Make sure it's a tinydrm fbdev */
	if (!(info == helper->fbdev && helper->dev == tdev->base))
		return NOTIFY_DONE;

	DRM_DEBUG("xres=%u, yres=%u\n", info->var.xres, info->var.yres);
	DRM_DEBUG("action=%lu, info=%p\n", action, info);

	switch (action) {
	case FB_EVENT_FB_REGISTERED:
		/* console and fb_info are locked at this point */

		/* This notifier can be registered multiple times */
		if (info->fbdefio)
			return NOTIFY_DONE;

		info->flags |= FBINFO_VIRTFB;
		strcpy(info->fix.id, "tinydrm");

		/*
		 * Without this change, the get_page() call in
		 * fb_deferred_io_fault() results in an oops:
		 *   Unable to handle kernel paging request at virtual address
		 */
		info->fix.smem_start = __pa(info->screen_base);

		/*
		 * A per device fbops structure is needed because
		 * fb_deferred_io_cleanup() clears fbops.fb_mmap
		 */
		fbops = devm_kzalloc(info->device, sizeof(*fbops), GFP_KERNEL);
		if (!fbops) {
			dev_err(info->device, "Could not allocate fbops\n");
			return notifier_from_errno(-ENOMEM);
		}

		*fbops = tinydrm_fbdev_cma_ops;
		info->fbops = fbops;

		/* A per device structure is needed for individual delays */
		info->fbdefio = devm_kzalloc(info->device,
					     sizeof(*info->fbdefio),
					     GFP_KERNEL);
		if (!info->fbdefio) {
			dev_err(info->device, "Could not allocate fbdefio\n");
			return notifier_from_errno(-ENOMEM);
		}

		if (tdev->deferred)
			delay = msecs_to_jiffies(tdev->deferred->defer_ms);
		/* delay=0 is turned into delay=HZ, so use 1 as a minimum */
		info->fbdefio->delay = delay ? : 1;
		info->fbdefio->deferred_io = tinydrm_fbdev_deferred_io;
		fb_deferred_io_init(info);
		break;
	case FB_EVENT_FB_UNREGISTERED:
		if (info->fbdefio) {
			fb_deferred_io_cleanup(info);
			info->fbdefio = NULL;
		}
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block tinydrm_fbdev_event_notifier = {
	.notifier_call  = tinydrm_fbdev_event_notify,
	.priority = 100, /* place before the fbcon notifier which is 0 */
};

int tinydrm_fbdev_init(struct tinydrm_device *tdev)
{
	struct drm_device *ddev = tdev->base;
	struct drm_fbdev_cma *fbdev;

	/*
	 * There's no race problem with 2 devices being here at the same
	 * time since the same notifier can be registered multiple times.
	 */
	fb_register_client(&tinydrm_fbdev_event_notifier);
	fbdev = drm_fbdev_cma_init(ddev, 16, ddev->mode_config.num_crtc,
				   ddev->mode_config.num_connector);
	fb_unregister_client(&tinydrm_fbdev_event_notifier);
	if (IS_ERR(fbdev))
		return PTR_ERR(fbdev);

	tdev->fbdev_cma = fbdev;

	return 0;
}

void tinydrm_fbdev_fini(struct tinydrm_device *tdev)
{
	if (!tdev->fbdev_cma)
		return;

	fb_register_client(&tinydrm_fbdev_event_notifier);
	drm_fbdev_cma_fini(tdev->fbdev_cma);
	tdev->fbdev_cma = NULL;
	fb_unregister_client(&tinydrm_fbdev_event_notifier);
}
