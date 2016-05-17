/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __LINUX_TINYDRM_H
#define __LINUX_TINYDRM_H

#include <drm/drm_crtc.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_simple_kms_helper.h>

struct tinydrm_device;
struct spi_device;
struct regulator;
struct lcdreg;

struct tinydrm_funcs {
	int (*prepare)(struct tinydrm_device *tdev);
	int (*unprepare)(struct tinydrm_device *tdev);
	int (*enable)(struct tinydrm_device *tdev);
	int (*disable)(struct tinydrm_device *tdev);
	int (*dirty)(struct drm_framebuffer *fb,
		     struct drm_gem_cma_object *cma_obj,
		     unsigned flags, unsigned color,
		     struct drm_clip_rect *clips, unsigned num_clips);
};

struct tinydrm_device {
	unsigned int width;
	unsigned int height;
	unsigned int width_mm;
	unsigned int height_mm;
	unsigned fbdefio_delay_ms;
	struct backlight_device *backlight;
	struct regulator *regulator;
	struct lcdreg *lcdreg;
	void *dev_private;
	struct drm_device *base;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct drm_fbdev_cma *fbdev_cma;
	struct work_struct dirty_work;
	struct mutex dirty_lock;
	bool prepared;
	bool enabled;
#ifdef CONFIG_DEBUG_FS
	struct dentry *debugfs;
	struct list_head update_list;
	struct mutex update_list_lock;
#endif
	const struct tinydrm_funcs *funcs;
};

extern const struct file_operations tinydrm_fops;
void tinydrm_lastclose(struct drm_device *dev);

#define TINYDRM_DRM_DRIVER(name_struct, name_str, desc_str, date_str) \
static struct drm_driver name_struct = { \
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME \
				| DRIVER_ATOMIC, \
	.lastclose		= tinydrm_lastclose, \
	.gem_free_object	= drm_gem_cma_free_object, \
	.gem_vm_ops		= &drm_gem_cma_vm_ops, \
	.prime_handle_to_fd	= drm_gem_prime_handle_to_fd, \
	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle, \
	.gem_prime_import	= drm_gem_prime_import, \
	.gem_prime_export	= drm_gem_prime_export, \
	.gem_prime_get_sg_table	= drm_gem_cma_prime_get_sg_table, \
	.gem_prime_import_sg_table = drm_gem_cma_prime_import_sg_table, \
	.gem_prime_vmap		= drm_gem_cma_prime_vmap, \
	.gem_prime_vunmap	= drm_gem_cma_prime_vunmap, \
	.gem_prime_mmap		= drm_gem_cma_prime_mmap, \
	.dumb_create		= drm_gem_cma_dumb_create, \
	.dumb_map_offset	= drm_gem_cma_dumb_map_offset, \
	.dumb_destroy		= drm_gem_dumb_destroy, \
	.fops			= &tinydrm_fops, \
	.name			= name_str, \
	.desc			= desc_str, \
	.date			= date_str, \
	.major			= 1, \
	.minor			= 0, \
}

struct drm_framebuffer *tinydrm_fb_create(struct drm_device *dev,
				struct drm_file *file_priv,
				const struct drm_mode_fb_cmd2 *mode_cmd);
int tinydrm_display_pipe_init(struct tinydrm_device *tdev,
			      const uint32_t *formats,
			      unsigned int format_count);
int devm_tinydrm_register(struct device *dev, struct tinydrm_device *tdev,
			  struct drm_driver *driver);

static inline void tinydrm_prepare(struct tinydrm_device *tdev)
{
	if (!tdev->prepared) {
		if (tdev->funcs && tdev->funcs->prepare)
			tdev->funcs->prepare(tdev);
		tdev->prepared = true;
	}
}

static inline void tinydrm_unprepare(struct tinydrm_device *tdev)
{
	if (tdev->prepared) {
		if (tdev->funcs && tdev->funcs->unprepare)
			tdev->funcs->unprepare(tdev);
		tdev->prepared = false;
	}
}

static inline void tinydrm_enable(struct tinydrm_device *tdev)
{
	if (!tdev->enabled) {
		if (tdev->funcs && tdev->funcs->enable)
			tdev->funcs->enable(tdev);
		tdev->enabled = true;
	}
}

static inline void tinydrm_disable(struct tinydrm_device *tdev)
{
	if (tdev->enabled) {
		if (tdev->funcs && tdev->funcs->disable)
			tdev->funcs->disable(tdev);
		tdev->enabled = false;
	}
}

int tinydrm_fbdev_init(struct tinydrm_device *tdev);
void tinydrm_fbdev_fini(struct tinydrm_device *tdev);

#ifdef CONFIG_DEBUG_FS
void tinydrm_debugfs_update_begin(struct tinydrm_device *tdev,
				  const struct drm_clip_rect *clip);
void tinydrm_debugfs_update_end(struct tinydrm_device *tdev, size_t len,
				unsigned bits_per_pixel);
void devm_tinydrm_debugfs_init(struct tinydrm_device *tdev);
#else
void tinydrm_debugfs_update_begin(struct tinydrm_device *tdev,
				  const struct drm_clip_rect *clip)
{
}

void tinydrm_debugfs_update_end(struct tinydrm_device *tdev, size_t len,
				unsigned bits_per_pixel)
{
}

void devm_tinydrm_debugfs_init(struct tinydrm_device *tdev)
{
}
#endif

void tinydrm_merge_clips(struct drm_clip_rect *dst,
			 struct drm_clip_rect *src, unsigned num_clips,
			 unsigned flags, u32 width, u32 height);
struct backlight_device *tinydrm_of_find_backlight(struct device *dev);
int tinydrm_enable_backlight(struct tinydrm_device *tdev);
int tinydrm_disable_backlight(struct tinydrm_device *tdev);
extern const struct dev_pm_ops tinydrm_simple_pm_ops;
void tinydrm_spi_shutdown(struct spi_device *spi);

#endif /* __LINUX_TINYDRM_H */
