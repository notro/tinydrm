/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/tinydrm/tinydrm.h>
#include <linux/console.h>
#include <linux/device.h>
#include <linux/dma-buf.h>

/**
 * DOC: Overview
 *
 * This library provides helpers for displays with onboard graphics memory
 * connected through a slow interface.
 *
 * In order for the display to turn off at shutdown, the device driver shutdown
 * callback has to be set. This function should call tinydrm_shutdown().
 *
 */

static const uint32_t tinydrm_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

static const struct drm_mode_config_funcs tinydrm_mode_config_funcs = {
	.fb_create = tinydrm_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/**
 * tinydrm_lastclose - DRM .lastclose() helper
 * @dev: DRM device
 *
 * This function ensures that fbdev is restored when drm_lastclose() is called
 * on the last drm_release(). tinydrm drivers should use this as their
 * &drm_driver ->lastclose callback.
 */
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
 * drm_gem_cma_free_object(). It also handles PRIME buffers which has the kernel
 * virtual address set by tinydrm_gem_cma_prime_import_sg_table(). tinydrm
 * drivers should set this as their &drm_driver ->gem_free_object callback.
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
	struct drm_device *dev = tdev->base;

	DRM_DEBUG_KMS("\n");

	tinydrm_shutdown(tdev);
	tinydrm_fbdev_fini(tdev);
	drm_mode_config_cleanup(dev);
	drm_dev_unregister(dev);
	drm_dev_unref(dev);
}

static int tinydrm_register(struct device *parent, struct tinydrm_device *tdev,
			    struct drm_driver *driver)
{
	struct drm_device *dev;
	int ret;

	dev = drm_dev_alloc(driver, parent);
	if (!dev)
		return -ENOMEM;

	ret = drm_dev_set_unique(dev, dev_name(dev->dev));
	if (ret)
		goto err_free;

	ret = drm_dev_register(dev, 0);
	if (ret)
		goto err_free;

	tdev->base = dev;
	dev->dev_private = tdev;

	return 0;

err_free:
	drm_dev_unref(dev);

	return ret;
}

static void devm_tinydrm_release(struct device *dev, void *res)
{
	tinydrm_unregister(*(struct tinydrm_device **)res);
}

/**
 * devm_tinydrm_register - Register tinydrm device
 * @parent: Parent device object
 * @tdev: tinydrm device
 * @driver: DRM driver
 *
 * This function registers a tinydrm device.
 * Resources will be automatically freed on driver detach (devres).
 */
int devm_tinydrm_register(struct device *parent, struct tinydrm_device *tdev,
			  struct drm_driver *driver)
{
	struct tinydrm_device **ptr;
	int ret;

	ptr = devres_alloc(devm_tinydrm_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return -ENOMEM;

	ret = tinydrm_register(parent, tdev, driver);
	if (ret) {
		devres_free(ptr);
		return ret;
	}

	*ptr = tdev;
	devres_add(parent, ptr);

	return 0;
}
EXPORT_SYMBOL(devm_tinydrm_register);

int tinydrm_modeset_init(struct tinydrm_device *tdev)
{
	struct drm_device *dev = tdev->base;
	int ret;

	drm_mode_config_init(dev);
	dev->mode_config.min_width = tdev->width;
	dev->mode_config.min_height = tdev->height;
	dev->mode_config.max_width = tdev->width;
	dev->mode_config.max_height = tdev->height;
	dev->mode_config.funcs = &tinydrm_mode_config_funcs;

	ret = tinydrm_display_pipe_init(tdev, tinydrm_formats,
					ARRAY_SIZE(tinydrm_formats));
	if (ret)
		return ret;

	drm_mode_config_reset(dev);

	ret = tinydrm_fbdev_init(tdev);
	if (ret)
		DRM_ERROR("Failed to initialize fbdev: %d\n", ret);

	return 0;
}
EXPORT_SYMBOL(tinydrm_modeset_init);

/**
 * tinydrm_shutdown - Shutdown tinydrm
 * @tdev: tinydrm device
 *
 * This function makes sure that tinydrm is disabled and unprepared.
 * Used by drivers in their shutdown callback to turn off the display
 * on machine shutdown and reboot.
 */
void tinydrm_shutdown(struct tinydrm_device *tdev)
{
	/* TODO Is there a drm function to disable output? */
	if (tdev->pipe.funcs)
		tdev->pipe.funcs->disable(&tdev->pipe);
}
EXPORT_SYMBOL(tinydrm_shutdown);

static void tinydrm_fbdev_suspend(struct tinydrm_device *tdev)
{
	if (!tdev->fbdev_helper)
		return;

	console_lock();
	drm_fb_helper_set_suspend(tdev->fbdev_helper, 1);
	console_unlock();
}

static void tinydrm_fbdev_resume(struct tinydrm_device *tdev)
{
	if (!tdev->fbdev_helper)
		return;

        console_lock();
        drm_fb_helper_set_suspend(tdev->fbdev_helper, 0);
        console_unlock();
}

/**
 * tinydrm_suspend - Suspend tinydrm
 * @tdev: tinydrm device
 *
 * Used in driver PM operations to suspend tinydrm.
 * Suspends fbdev and DRM.
 * Resume with tinydrm_resume().
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int tinydrm_suspend(struct tinydrm_device *tdev)
{
	struct drm_device *dev = tdev->base;
	struct drm_atomic_state *state;

	if (tdev->suspend_state) {
		DRM_ERROR("Failed to suspend: state already set\n");
		return -EINVAL;
	}

	tinydrm_fbdev_suspend(tdev);
	state = drm_atomic_helper_suspend(dev);
	if (IS_ERR(state)) {
		tinydrm_fbdev_resume(tdev);
		return PTR_ERR(state);
	}

	tdev->suspend_state = state;

	return 0;
}
EXPORT_SYMBOL(tinydrm_suspend);

/**
 * tinydrm_resume - Resume tinydrm
 * @tdev: tinydrm device
 *
 * Used in driver PM operations to resume tinydrm.
 * Suspend with tinydrm_suspend().
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int tinydrm_resume(struct tinydrm_device *tdev)
{
	struct drm_atomic_state *state = tdev->suspend_state;
	struct drm_device *dev = tdev->base;
	int ret;

	if (!state) {
		DRM_ERROR("Failed to resume: state is not set\n");
		return -EINVAL;
	}

	tdev->suspend_state = NULL;

	ret = drm_atomic_helper_resume(dev, state);
	if (ret) {
		DRM_ERROR("Error resuming state: %d\n", ret);
		drm_atomic_state_free(state);
		return ret;
	}

	tinydrm_fbdev_resume(tdev);

	return 0;
}
EXPORT_SYMBOL(tinydrm_resume);

MODULE_LICENSE("GPL");
