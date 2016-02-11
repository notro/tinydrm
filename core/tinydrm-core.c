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
#include <drm/drm_atomic_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_plane_helper.h>
#include <drm/tinydrm/tinydrm.h>
#include <linux/device.h>

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

	tinydrm_mode_config_init(tdev);

	drm_plane_helper_add(&tdev->plane, &tinydrm_plane_helper_funcs);
	ret = drm_universal_plane_init(ddev, &tdev->plane, 0,
				       &tinydrm_plane_funcs, tinydrm_formats,
				       ARRAY_SIZE(tinydrm_formats),
				       DRM_PLANE_TYPE_PRIMARY);
	if (ret)
		return ret;

	ret = tinydrm_crtc_create(tdev);
	if (ret)
		return ret;

	tinydrm_get_first_connector(ddev)->status = connector_status_connected;
	drm_panel_init(&tdev->panel);
	drm_panel_add(&tdev->panel);
	drm_panel_attach(&tdev->panel, tinydrm_get_first_connector(ddev));

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
