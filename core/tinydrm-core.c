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
#include <drm/tinydrm/tinydrm.h>
#include <linux/device.h>

#include "internal.h"

void tinydrm_lastclose(struct drm_device *dev)
{
	struct tinydrm_device *tdev = dev->dev_private;

	DRM_DEBUG_KMS("\n");
	tinydrm_fbdev_restore_mode(tdev->fbdev);
}
EXPORT_SYMBOL(tinydrm_lastclose);

const struct file_operations tinydrm_fops = {
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
EXPORT_SYMBOL(tinydrm_fops);

static void tinydrm_unregister(struct tinydrm_device *tdev)
{
	DRM_DEBUG_KMS("\n");

	if (tdev->deferred)
		cancel_delayed_work_sync(&tdev->deferred->dwork);

	tinydrm_fbdev_fini(tdev);

	drm_panel_detach(&tdev->panel);
	drm_panel_remove(&tdev->panel);

	drm_mode_config_cleanup(tdev->base);
	drm_dev_unregister(tdev->base);
	drm_dev_unref(tdev->base);
}

static int tinydrm_register(struct device *dev, struct tinydrm_device *tdev,
			    struct drm_driver *driver)
{
	struct drm_connector *connector;
	struct drm_device *ddev;
	int ret;

	DRM_DEBUG_KMS("\n");

dev->coherent_dma_mask = DMA_BIT_MASK(32);

	if (WARN_ON(!tdev->dirtyfb))
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

	tinydrm_mode_config_init(tdev);
	ret = tinydrm_plane_init(tdev);
	if (ret)
		goto err_free;

	ret = tinydrm_crtc_create(tdev);
	if (ret)
		goto err_free;

	connector = list_first_entry(&ddev->mode_config.connector_list,
				     typeof(*connector), head);
	connector->status = connector_status_connected;

	drm_panel_init(&tdev->panel);
	drm_panel_add(&tdev->panel);
	drm_panel_attach(&tdev->panel, connector);

	drm_mode_config_reset(ddev);

	ret = tinydrm_fbdev_init(tdev);
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

static void devm_tinydrm_release(struct device *dev, void *res)
{
	tinydrm_unregister(*(struct tinydrm_device **)res);
}

int devm_tinydrm_register(struct device *dev, struct tinydrm_device *tdev,
			  struct drm_driver *driver)
{
	struct tinydrm_device **ptr;
	int ret;

	ptr = devres_alloc(devm_tinydrm_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return -ENOMEM;

	ret = tinydrm_register(dev, tdev, driver);
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
