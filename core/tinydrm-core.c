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
#include <drm/drm_fb_cma_helper.h>
#include <drm/tinydrm/tinydrm.h>
#include <linux/device.h>
#include <linux/dma-buf.h>

static const uint32_t tinydrm_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

static const struct drm_mode_config_funcs tinydrm_mode_config_funcs = {
	.fb_create = tinydrm_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

void tinydrm_lastclose(struct drm_device *dev)
{
	struct tinydrm_device *tdev = dev->dev_private;

	DRM_DEBUG_KMS("\n");
	drm_fbdev_cma_restore_mode(tdev->fbdev_cma);
}
EXPORT_SYMBOL(tinydrm_lastclose);

/**
 * tinydrm_gem_cma_free_object - free resources associated with a CMA GEM
 *                               object
 * @gem_obj: GEM object to free
 *
 * This function frees the backing memory of the CMA GEM object, cleans up the
 * GEM object state and frees the memory used to store the object itself using
 * drm_gem_cma_free_object(). It also handles PRIME buffers with the kernel
 * virtual address set through the use of
 * tinydrm_gem_cma_prime_import_sg_table(). tinydrm drivers should set this as
 * their &drm_driver ->gem_free_object callback.
 */
void tinydrm_gem_cma_free_object(struct drm_gem_object *gem_obj)
{
	if (gem_obj->import_attach) {
		struct drm_gem_cma_object *cma_obj;

		cma_obj = to_drm_gem_cma_obj(gem_obj);
		dma_buf_vunmap(gem_obj->import_attach->dmabuf, cma_obj->vaddr);
		cma_obj->vaddr = NULL;
	}

	drm_gem_cma_free_object(gem_obj);
}
EXPORT_SYMBOL_GPL(tinydrm_gem_cma_free_object);

/**
 * tinydrm_gem_cma_prime_import_sg_table - produce a CMA GEM object from
 *     another driver's scatter/gather table of pinned pages
 * @dev: device to import into
 * @attach: DMA-BUF attachment
 * @sgt: scatter/gather table of pinned pages
 *
 * This function imports a scatter/gather table exported via DMA-BUF by
 * another driver using drm_gem_cma_prime_import_sg_table(). It also sets the
 * kernel virtual address on the CMA object. tinydrm drivers should use this
 * as their &drm_driver ->gem_prime_import_sg_table callback.
 *
 * Returns:
 * A pointer to a newly created GEM object or an ERR_PTR-encoded negative
 * error code on failure.
 */
struct drm_gem_object *
tinydrm_gem_cma_prime_import_sg_table(struct drm_device *dev,
				      struct dma_buf_attachment *attach,
				      struct sg_table *sgt)
{
	struct drm_gem_cma_object *cma_obj;
	struct drm_gem_object *obj;
	void *vaddr;

	vaddr = dma_buf_vmap(attach->dmabuf);
	if (!vaddr) {
		DRM_ERROR("Failed to vmap PRIME buffer\n");
		return ERR_PTR(-ENOMEM);
	}

	obj = drm_gem_cma_prime_import_sg_table(dev, attach, sgt);
	if (IS_ERR(obj)) {
		dma_buf_vunmap(attach->dmabuf, vaddr);
		return obj;
	}

	cma_obj = to_drm_gem_cma_obj(obj);
	cma_obj->vaddr = vaddr;

	return obj;
}
EXPORT_SYMBOL(tinydrm_gem_cma_prime_import_sg_table);

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

	tinydrm_fbdev_fini(tdev);

	drm_mode_config_cleanup(tdev->base);
	drm_dev_unregister(tdev->base);
	drm_dev_unref(tdev->base);
}

static int tinydrm_register(struct device *parent, struct tinydrm_device *tdev,
			    struct drm_driver *driver)
{
	struct drm_device *dev;
	int ret;

	DRM_DEBUG_KMS("\n");

	if (!parent->coherent_dma_mask) {
		ret = dma_set_coherent_mask(parent, DMA_BIT_MASK(32));
		if (ret) {
			DRM_ERROR("Failed to set coherent_dma_mask\n");
			return ret;
		}
	}

	dev = drm_dev_alloc(driver, parent);
	if (!dev)
		return -ENOMEM;

	tdev->base = dev;
	dev->dev_private = tdev;

	ret = drm_dev_set_unique(dev, dev_name(dev->dev));
	if (ret)
		goto err_free;

	ret = drm_dev_register(dev, 0);
	if (ret)
		goto err_free;

	drm_mode_config_init(dev);
	dev->mode_config.min_width = tdev->width;
	dev->mode_config.min_height = tdev->height;
	dev->mode_config.max_width = tdev->width;
	dev->mode_config.max_height = tdev->height;
	dev->mode_config.funcs = &tinydrm_mode_config_funcs;

	ret = tinydrm_display_pipe_init(tdev, tinydrm_formats,
					ARRAY_SIZE(tinydrm_formats));
	if (ret)
		goto err_free;

	drm_mode_config_reset(dev);
	devm_tinydrm_debugfs_init(tdev);

	ret = tinydrm_fbdev_init(tdev);
	if (ret)
		DRM_ERROR("Failed to initialize fbdev: %d\n", ret);

	DRM_INFO("Device: %s\n", dev_name(dev->dev));
	DRM_INFO("Initialized %s %d.%d.%d on minor %d\n",
		 driver->name, driver->major, driver->minor, driver->patchlevel,
		 dev->primary->index);

	return 0;

err_free:
	drm_dev_unref(dev);

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
