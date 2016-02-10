//#define DEBUG
/*
 * Copyright (C) 2016 Noralf Trønnes
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

struct backlight_device *tinydrm_of_find_backlight(struct device *dev)
{
	struct backlight_device *backlight;
	struct device_node *np;

	np = of_parse_phandle(dev->of_node, "backlight", 0);
	if (!np)
		return NULL;

	backlight = of_find_backlight_by_node(np);
	of_node_put(np);

	if (!backlight)
		return ERR_PTR(-EPROBE_DEFER);

	return backlight;
}
EXPORT_SYMBOL(tinydrm_of_find_backlight);

static int tinydrm_connector_get_modes(struct drm_connector *connector)
{
	struct tinydrm_device *tdev = connector->dev->dev_private;
	struct drm_display_mode *mode;
	int ret;

	DRM_DEBUG_KMS("\n");
	ret = drm_panel_get_modes(&tdev->panel);
	if (ret > 0)
		return ret;

	mode = drm_cvt_mode(connector->dev, tdev->width, tdev->height, 60, false, false, false);
	if (!mode)
		return 0;

	mode->type |= DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	return 1;
}

struct drm_encoder *tinydrm_connector_best_encoder(struct drm_connector *connector)
{
	return drm_encoder_find(connector->dev, connector->encoder_ids[0]);
}

static const struct drm_connector_helper_funcs tinydrm_connector_helper_funcs = {
	.get_modes = tinydrm_connector_get_modes,
	.best_encoder = tinydrm_connector_best_encoder,
};

static void tinydrm_crtc_enable(struct drm_crtc *crtc)
{
	struct tinydrm_device *tdev = crtc->dev->dev_private;

	DRM_DEBUG_KMS("prepared=%u, enabled=%u\n", tdev->prepared, tdev->enabled);

	if (!tdev->prepared) {
		drm_panel_prepare(&tdev->panel);
		tdev->prepared = true;
	}

	/* The panel is enabled after the first display update */
}

static void tinydrm_crtc_disable(struct drm_crtc *crtc)
{
	struct tinydrm_device *tdev = crtc->dev->dev_private;

	DRM_DEBUG_KMS("prepared=%u, enabled=%u\n", tdev->prepared, tdev->enabled);

	if (tdev->enabled) {
		drm_panel_disable(&tdev->panel);
		tdev->enabled = false;
	}

	if (tdev->prepared) {
		drm_panel_unprepare(&tdev->panel);
		tdev->prepared = false;
	}
}

static const struct drm_crtc_helper_funcs tinydrm_crtc_helper_funcs = {
	.disable = tinydrm_crtc_disable,
	.enable = tinydrm_crtc_enable,
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

/* TODO: Configurable */
static const uint32_t tinydrm_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
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
	.destroy		= drm_plane_cleanup,
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

	ret = tinydrm_simple_crtc_create(ddev, &tdev->plane, NULL, &tinydrm_crtc_helper_funcs, &tinydrm_connector_helper_funcs);
	if (ret)
		return ret;

	drm_panel_init(&tdev->panel);
	drm_panel_add(&tdev->panel);
	drm_panel_attach(&tdev->panel, tinydrm_get_connector(ddev));

	drm_mode_config_reset(ddev);

	ret = tinydrm_fbdev_init(tdev);
	if (ret)
		return ret;

	return 0;
}

static void tinydrm_lastclose(struct drm_device *ddev)
{
	struct tinydrm_device *tdev = ddev->dev_private;

	DRM_DEBUG_KMS("\n");
	tinydrm_fbdev_cma_restore_mode(tdev->fbdev_cma);
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
	.lastclose		= tinydrm_lastclose,
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

	if (WARN_ON(!tdev->fb_dirty))
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

static void devm_tinydrm_release(struct device *dev, void *res)
{
	tinydrm_release(*(struct tinydrm_device **)res);
}

int devm_tinydrm_register(struct device *dev, struct tinydrm_device *tdev)
{
	struct tinydrm_device **ptr;
	int ret;

	ptr = devres_alloc(devm_tinydrm_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return -ENOMEM;

	ret = tinydrm_register(dev, tdev);
	if (ret) {
		devres_free(ptr);
		return ret;
	}

	*ptr = tdev;
	devres_add(dev, ptr);

	return 0;
}
EXPORT_SYMBOL(devm_tinydrm_register);

MODULE_LICENSE("GPL");
