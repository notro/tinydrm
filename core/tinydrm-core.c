//#define DEBUG
/*
 * Copyright (C) 2015 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/device.h>

#include <drm/drmP.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_plane_helper.h>
#include <drm/tinydrm/tinydrm.h>

#include "internal.h"

void tinydrm_merge_clips(struct drm_clip_rect *dst, struct drm_clip_rect *clips, unsigned num_clips, unsigned flags, u32 width, u32 height)
{
	struct drm_clip_rect full_clip = {
		.x1 = 0,
		.x2 = width - 1,
		.y1 = 0,
		.y2 = height - 1,
	};
	int i;

	if (!clips) {
		clips = &full_clip;
		num_clips = 1;
	}

	for (i = 0; i < num_clips; i++) {
		dst->x1 = min(dst->x1, clips[i].x1);
		dst->x2 = max(dst->x2, clips[i].x2);
		dst->y1 = min(dst->y1, clips[i].y1);
		dst->y2 = max(dst->y2, clips[i].y2);

		if (flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY) {
			i++;
			dst->x2 = max(dst->x2, clips[i].x2);
			dst->y2 = max(dst->y2, clips[i].y2);
		}
	}

	dst->x2 = min_t(u32, dst->x2, width - 1);
	dst->y2 = min_t(u32, dst->y2, height - 1);
}
EXPORT_SYMBOL(tinydrm_merge_clips);

/******************************************************************************
 *
 *  Connector
 *
 */


static inline struct tinydrm_device *connector_to_tinydrm(struct drm_connector *connector)
{
	return container_of(connector, struct tinydrm_device, connector);
}

static int tinydrm_connector_get_modes(struct drm_connector *connector)
{
	struct tinydrm_device *tdev = connector_to_tinydrm(connector);
	struct drm_display_mode *mode;
	int ret;

	DRM_DEBUG_KMS("\n");
	ret = drm_panel_get_modes(&tdev->panel);
	if (ret > 0)
		return ret;

	// this is destroyed by tinydrm_connector_destroy -> drm_connector_cleanup -> drm_mode_remove -> drm_mode_destroy
	mode = drm_cvt_mode(connector->dev, tdev->width, tdev->height, 60, false, false, false);
	if (!mode)
		return 0;

	mode->type |= DRM_MODE_TYPE_PREFERRED;
//	mode->width_mm = tdev->width_mm;
//	mode->height_mm = tdev->height_mm;

	drm_mode_probed_add(connector, mode);

	return 1; // number of modes
}

struct drm_encoder *tinydrm_connector_best_encoder(struct drm_connector *connector)
{
	struct tinydrm_device *tdev = connector_to_tinydrm(connector);

	return &tdev->encoder;
}

static const struct drm_connector_helper_funcs tinydrm_connector_helper_funcs = {
	.get_modes = tinydrm_connector_get_modes,
//	.mode_valid = tinydrm_connector_mode_valid, // optional
	.best_encoder = tinydrm_connector_best_encoder,
};

static enum drm_connector_status
tinydrm_connector_detect(struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}

static void tinydrm_connector_destroy(struct drm_connector *connector)
{
	DRM_DEBUG_KMS("\n");
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static const struct drm_connector_funcs tinydrm_connector_funcs = {
	.dpms = drm_atomic_helper_connector_dpms,
	.reset = drm_atomic_helper_connector_reset,
	.detect = tinydrm_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = tinydrm_connector_destroy,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};




/******************************************************************************
 *
 *  Encoder
 *
 */


static void tinydrm_encoder_disable(struct drm_encoder *encoder)
{
}

static void tinydrm_encoder_enable(struct drm_encoder *encoder)
{
}

static int tinydrm_encoder_atomic_check(struct drm_encoder *encoder,
					struct drm_crtc_state *crtc_state,
					struct drm_connector_state *conn_state)
{
	struct drm_display_mode *adjusted_mode = &crtc_state->adjusted_mode;
	const struct drm_display_mode *mode = &crtc_state->mode;
	struct drm_connector *connector = conn_state->connector;
	const struct drm_display_mode *panel_mode;
	struct drm_device *ddev = encoder->dev;

	if (list_empty(&connector->modes)) {
		dev_dbg(ddev->dev, "encoder: empty modes list\n");
		return -EINVAL;
	}

	panel_mode = list_first_entry(&connector->modes,
				      struct drm_display_mode, head);

	/* We're not allowed to modify the resolution. */
	if (mode->hdisplay != panel_mode->hdisplay ||
	    mode->vdisplay != panel_mode->vdisplay)
		return -EINVAL;

	/* The flat panel mode is fixed, just copy it to the adjusted mode. */
	drm_mode_copy(adjusted_mode, panel_mode);

	return 0;
}


static const struct drm_encoder_helper_funcs tinydrm_encoder_helper_funcs = {
//	.mode_set = tinydrm_encoder_mode_set, // optional, not sure if we need it. Called from crtc_set_mode()

// either dpms/commit or enable/disable
	.disable = tinydrm_encoder_disable,
	.enable = tinydrm_encoder_enable,

// atomic_check() or mode_fixup()
// maybe not: http://lxr.free-electrons.com/source/drivers/gpu/drm/drm_atomic_helper.c#L310
// ->atomic_check(
	.atomic_check = tinydrm_encoder_atomic_check,
};

static const struct drm_encoder_funcs tinydrm_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};




/******************************************************************************
 *
 *  Crtc
 *
 */


static void tinydrm_crtc_enable(struct drm_crtc *crtc)
{
	struct drm_device *ddev = crtc->dev;
	struct tinydrm_device *tdev = ddev->dev_private;

	dev_info(ddev->dev, "%s: enabled=%u\n", __func__, tdev->enabled);
	if (tdev->enabled)
		return;

	drm_panel_prepare(&tdev->panel);
	drm_panel_enable(&tdev->panel);
	tdev->enabled = true;
}

static void tinydrm_crtc_disable(struct drm_crtc *crtc)
{
	struct drm_device *ddev = crtc->dev;
	struct tinydrm_device *tdev = ddev->dev_private;

	dev_info(ddev->dev, "%s: enabled=%u\n", __func__, tdev->enabled);
	if (!tdev->enabled)
		return;

	drm_panel_disable(&tdev->panel);
	drm_panel_unprepare(&tdev->panel);
	tdev->enabled = false;
}

static const struct drm_crtc_helper_funcs tinydrm_crtc_helper_funcs = {
	.disable = tinydrm_crtc_disable,
	.enable = tinydrm_crtc_enable, // enable or commit: http://lxr.free-electrons.com/ident?i=drm_atomic_helper_commit_modeset_enables
};

static const struct drm_crtc_funcs tinydrm_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};




/******************************************************************************
 *
 *  Framebuffer
 *
 */


static inline struct tinydrm_framebuffer *to_tinydrm_framebuffer(struct drm_framebuffer *fb)
{
	return container_of(fb, struct tinydrm_framebuffer, base);
}

static void tinydrm_framebuffer_destroy(struct drm_framebuffer *fb)
{
	struct tinydrm_framebuffer *tinydrm_fb = to_tinydrm_framebuffer(fb);
	struct tinydrm_device *tdev = fb->dev->dev_private;

	DRM_DEBUG_KMS("drm_framebuffer = %p, tinydrm_fb->obj = %p\n", fb, tinydrm_fb->obj);

	flush_delayed_work(&tdev->dirty.deferred_work);

	if (tinydrm_fb->obj)
		drm_gem_object_unreference_unlocked(tinydrm_fb->obj);

	drm_framebuffer_cleanup(fb);
	kfree(tinydrm_fb);
}

static void tinydrm_deferred_dirty_work(struct work_struct *work)
{
	struct tinydrm_device *tdev = container_of(work, struct tinydrm_device, dirty.deferred_work.work);

	dev_dbg(tdev->base->dev, "%s\n", __func__);

	tdev->update(tdev);
}

int tinydrm_schedule_dirty(struct drm_framebuffer *fb,
			   struct drm_gem_cma_object *cma_obj, unsigned flags,
			   unsigned color, struct drm_clip_rect *clips,
			   unsigned num_clips, bool run_now)
{
	struct tinydrm_device *tdev = fb->dev->dev_private;
	unsigned long delay;

	dev_dbg(tdev->base->dev, "%s(fb = %p, cma_obj = %p, clips = %p, num_clips = %u, run_now = %u)\n", __func__, fb, cma_obj, clips, num_clips, run_now);

	spin_lock(&tdev->dirty.lock);

	tdev->dirty.fb = fb;
	tdev->dirty.cma_obj = cma_obj;
	tinydrm_merge_clips(&tdev->dirty.clip, clips, num_clips, flags,
			    fb->width, fb->height);
	if (tinydrm_is_full_clip(&tdev->dirty.clip, fb->width, fb->height))
		run_now = true;

	spin_unlock(&tdev->dirty.lock);

	delay = run_now ? 0 : msecs_to_jiffies(tdev->dirty.defer_ms);

{
bool ret;
	ret = schedule_delayed_work(&tdev->dirty.deferred_work, delay);

	if (ret)
		dev_dbg(tdev->base->dev, "%s: %s\n", __func__, ret ? "queued" : "already on queue");

}
	return 0;
}

static int tinydrm_framebuffer_dirty(struct drm_framebuffer *fb,
				     struct drm_file *file_priv,
				     unsigned flags, unsigned color,
				     struct drm_clip_rect *clips,
				     unsigned num_clips)
{
	struct tinydrm_framebuffer *tfb = to_tinydrm_framebuffer(fb);

	dev_dbg(fb->dev->dev, "%s\n", __func__);

	return tinydrm_schedule_dirty(fb, tfb->cma_obj, flags, color,
				      clips, num_clips, false);
}

static const struct drm_framebuffer_funcs tinydrm_fb_funcs = {
	.destroy = tinydrm_framebuffer_destroy,
	.dirty = tinydrm_framebuffer_dirty,
/*	TODO?
 *	.create_handle = tinydrm_framebuffer_create_handle, */
};




/******************************************************************************
 *
 *  Mode
 *
 */


static struct drm_framebuffer *
tinydrm_fb_create(struct drm_device *ddev, struct drm_file *file_priv,
		  struct drm_mode_fb_cmd2 *mode_cmd)
{
//	struct tinydrm_device *tdev = ddev->dev_private;
	struct tinydrm_framebuffer *tinydrm_fb;
	struct drm_gem_object *obj;
	int ret;

	/* Validate the pixel format, size and pitches */
//	mode_cmd->pixel_format
//	mode_cmd->width
//	mode_cmd->height
//	mode_cmd->pitches
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




/******************************************************************************
 *
 * Plane
 *
 */

static const uint32_t tinydrm_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,

/*
	DRM_FORMAT_BGRX8888,
	DRM_FORMAT_BGRA8888,
	DRM_FORMAT_RGBX8888,
	DRM_FORMAT_RGBA8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_ABGR8888,
*/
};

static void tinydrm_plane_atomic_update(struct drm_plane *plane,
                                           struct drm_plane_state *old_state)
{
//	struct drm_device *ddev = plane->dev;

/*
        if (plane->fb) {
                vgfb = to_tinydrm_framebuffer(plane->fb);
                bo = gem_to_tinydrm_obj(vgfb->obj);
                handle = bo->hw_res_handle;
        } else {
                handle = 0;
        }
*/
	DRM_DEBUG("handle 0x%x, crtc %dx%d+%d+%d\n", 0,
		  plane->state->crtc_w, plane->state->crtc_h,
		  plane->state->crtc_x, plane->state->crtc_y);
}

static const struct drm_plane_helper_funcs tinydrm_plane_helper_funcs = {
	.atomic_update = tinydrm_plane_atomic_update,
};

static const struct drm_plane_funcs tinydrm_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= drm_primary_helper_destroy,
	.reset			= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state	= drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
};




/******************************************************************************
 *
 * DRM driver
 *
 */


static int tinydrm_load(struct drm_device *ddev, unsigned long flags)
{
	struct tinydrm_device *tdev = ddev->dev_private;
	int ret;

	DRM_DEBUG_KMS("\n");

	INIT_DELAYED_WORK(&tdev->dirty.deferred_work, tinydrm_deferred_dirty_work);
	tinydrm_reset_clip(&tdev->dirty.clip);

	drm_mode_config_init(ddev);

	ddev->mode_config.min_width = tdev->width;
	ddev->mode_config.min_height = tdev->height;
	ddev->mode_config.max_width = tdev->width;
	ddev->mode_config.max_height = tdev->height;
	ddev->mode_config.funcs = &tinydrm_mode_config_funcs;

	drm_plane_helper_add(&tdev->plane, &tinydrm_plane_helper_funcs);
	ret = drm_universal_plane_init(ddev, &tdev->plane, 0,
				       &tinydrm_plane_funcs, tinydrm_formats,
				       ARRAY_SIZE(tinydrm_formats),
				       DRM_PLANE_TYPE_PRIMARY);
	if (ret)
		return ret;

	drm_crtc_helper_add(&tdev->crtc, &tinydrm_crtc_helper_funcs);
	ret = drm_crtc_init_with_planes(ddev, &tdev->crtc, &tdev->plane, NULL,
					&tinydrm_crtc_funcs);
	if (ret)
		return ret;

//	drm_mode_crtc_set_gamma_size(&tdev->crtc, 256);

	tdev->encoder.possible_crtcs = 1 << drm_crtc_index(&tdev->crtc);
	drm_encoder_helper_add(&tdev->encoder, &tinydrm_encoder_helper_funcs);
	ret = drm_encoder_init(ddev, &tdev->encoder, &tinydrm_encoder_funcs, DRM_MODE_ENCODER_VIRTUAL);
	if (ret)
		return ret;

	tdev->connector.status = connector_status_connected;
	drm_connector_helper_add(&tdev->connector, &tinydrm_connector_helper_funcs);
	ret = drm_connector_init(ddev, &tdev->connector, &tinydrm_connector_funcs, DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret)
		return ret;

	ret = drm_mode_connector_attach_encoder(&tdev->connector, &tdev->encoder);
	if (ret)
		return ret;

	ret = drm_connector_register(&tdev->connector);
	if (ret)
		return ret;

	drm_panel_init(&tdev->panel);
	drm_panel_add(&tdev->panel);
	drm_panel_attach(&tdev->panel, &tdev->connector);

	drm_mode_config_reset(ddev);

	ret = tinydrm_fbdev_init(tdev);
	if (ret)
		return ret;

	return 0;
}

static const struct file_operations tinydrm_fops = {
	.owner		= THIS_MODULE,
	.open		= drm_open,
	.release	= drm_release,
	.unlocked_ioctl	= drm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= drm_compat_ioctl,
#endif
	.poll		= drm_poll,
	.read		= drm_read,
	.llseek		= no_llseek,
	.mmap		= drm_gem_cma_mmap,
};

static struct drm_driver tinydrm_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME
				| DRIVER_ATOMIC,
	.load			= tinydrm_load,
//	.unload			= tinydrm_unload,
	.get_vblank_counter	= drm_vblank_count,
//	.enable_vblank		= tinydrm_enable_vblank,
//	.disable_vblank		= tinydrm_disable_vblank,
	.gem_free_object	= drm_gem_cma_free_object,
	.gem_vm_ops		= &drm_gem_cma_vm_ops,
	.prime_handle_to_fd	= drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle,
	.gem_prime_import	= drm_gem_prime_import,
	.gem_prime_export	= drm_gem_prime_export,
	.gem_prime_get_sg_table	= drm_gem_cma_prime_get_sg_table,
	.gem_prime_import_sg_table = drm_gem_cma_prime_import_sg_table,
	.gem_prime_vmap		= drm_gem_cma_prime_vmap,
	.gem_prime_vunmap	= drm_gem_cma_prime_vunmap,
	.gem_prime_mmap		= drm_gem_cma_prime_mmap,
	.dumb_create		= drm_gem_cma_dumb_create,
	.dumb_map_offset	= drm_gem_cma_dumb_map_offset,
	.dumb_destroy		= drm_gem_dumb_destroy,
	.fops			= &tinydrm_fops,
	.name			= "tinydrm",
	.desc			= "tinydrm",
	.date			= "20150916",
	.major			= 1,
	.minor			= 0,
};

void tinydrm_release(struct tinydrm_device *tdev)
{
	DRM_DEBUG_KMS("\n");
//	drm_put_dev(tdev->base);

// TODO: cancel dirty deferred_work
//	cancel_delayed_work_sync(...);
	tinydrm_fbdev_fini(tdev);

	drm_panel_detach(&tdev->panel);
	drm_panel_remove(&tdev->panel);

	drm_mode_config_cleanup(tdev->base);
	drm_dev_unregister(tdev->base);
	drm_dev_unref(tdev->base);
}
EXPORT_SYMBOL(tinydrm_release);

int tinydrm_register(struct device *dev, struct tinydrm_device *tdev)
{
	struct drm_driver *driver = &tinydrm_driver;
	struct drm_device *ddev;
	int ret;

	dev_info(dev, "%s\n", __func__);

dev->coherent_dma_mask = DMA_BIT_MASK(32);

	if (WARN_ON(!tdev->update))
		return -EINVAL;

	ddev = drm_dev_alloc(driver, dev);
	if (!ddev)
		return -ENOMEM;

	tdev->base = ddev;
	ddev->dev_private = tdev;

	ret = drm_dev_set_unique(ddev, dev_name(ddev->dev));
	if (ret)
		goto err_free;

	ret = drm_dev_register(ddev, 0);
	if (ret)
		goto err_free;

	DRM_INFO("Device: %s\n", dev_name(dev));
	DRM_INFO("Initialized %s %d.%d.%d on minor %d\n",
		 driver->name, driver->major, driver->minor, driver->patchlevel,
		 ddev->primary->index);

	return 0;

err_free:
	drm_dev_unref(ddev);

	return ret;
}
EXPORT_SYMBOL(tinydrm_register);

MODULE_LICENSE("GPL");
