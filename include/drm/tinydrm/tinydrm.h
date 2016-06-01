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

struct tinydrm_debugfs_dirty;
struct tinydrm_device;
struct spi_device;
struct regulator;
struct lcdreg;

/**
 * struct tinydrm_funcs - tinydrm device operations
 * @prepare: power on display and perform set up (optional)
 * @unprepare: power off display (optional)
 * @enable: enable display (optional)
 * @disable: disable display (optional)
 * @dirty: flush the framebuffer (optional, but not very useful without it)
 *
 * The .prepare() function is called when the display pipeline is enabled.
 * This is typically used to power on and initialize the display controller
 * to a state where it can receive framebuffer updates.
 *
 * After the first time the framebuffer has been flushed and the display has
 * been updated, the .enable() function is called. This will typically enable
 * backlight or by other means switch on display output.
 *
 * The .disable() function will typically disable backlight and .unprepare()
 * will power off the display. They are called in that order when the display
 * pipeline is disabled.
 *
 * The .dirty() function is called when the &drm_framebuffer is flushed, either
 * by userspace or by the fbdev emulation layer.
 * It has a one-to-one mapping to the &drm_framebuffer_funcs ->dirty callback
 * except that it also provides the &drm_gem_cma_object buffer object that is
 * backing the framebuffer. The entire framebuffer should be flushed if clips
 * is NULL. See &drm_mode_fb_dirty_cmd for more information about the
 * arguments. When the pipeline detects that the current framebuffer has
 * changed, it schedules a worker to flush this new framebuffer ensuring that
 * the display is in sync.
 */
struct tinydrm_funcs {
	int (*prepare)(struct tinydrm_device *tdev);
	void (*unprepare)(struct tinydrm_device *tdev);
	int (*enable)(struct tinydrm_device *tdev);
	void (*disable)(struct tinydrm_device *tdev);
	int (*dirty)(struct drm_framebuffer *fb,
		     struct drm_gem_cma_object *cma_obj,
		     unsigned flags, unsigned color,
		     struct drm_clip_rect *clips, unsigned num_clips);
};

/**
 * struct tinydrm_device - tinydrm device
 * @backlight: backlight device (optional)
 * @regulator: regulator source (optional)
 * @lcdreg: LCD register structure (optional)
 * @dev_private: device private data (optional)
 * @base: DRM device
 * @pipe: Display pipe structure
 * @fbdev_cma: fbdev CMA structure (optional)
 * @fbdev_helper: fbdev helper (optional)
 * @suspend_state: atomic state when suspended
 * @dirty_work: framebuffer flusher
 * @dev_lock: serializes &tinydrm_funcs operations and protects
 *            prepared/enabled state changes
 * @prepared: device prepared state
 * @enabled: device enabled state
 * @debugfs_dirty: debugfs dirty file control structure
 * @funcs: tinydrm device operations (optional)
 */
struct tinydrm_device {
	struct backlight_device *backlight;
	struct regulator *regulator;
	struct lcdreg *lcdreg;
	void *dev_private;
	struct drm_device *base;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct drm_fbdev_cma *fbdev_cma;
	struct drm_fb_helper *fbdev_helper;
	struct drm_atomic_state *suspend_state;
	struct work_struct dirty_work;
	struct mutex dev_lock;
	bool prepared;
	bool enabled;
	struct tinydrm_debugfs_dirty *debugfs_dirty;
	const struct tinydrm_funcs *funcs;
};

/*
 * TINYDRM_DRM_DRIVER - default tinydrm driver structure
 * @name_struct: structure name
 * @name_str: driver name
 * @desc_str: driver description
 * @date_str: driver date
 *
 * This macro provides a default &drm_driver structure for drivers.
 */
#define TINYDRM_DRM_DRIVER(name_struct, name_str, desc_str, date_str) \
static struct drm_driver name_struct = { \
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME \
				| DRIVER_ATOMIC, \
	.lastclose		= tinydrm_lastclose, \
	.gem_free_object	= tinydrm_gem_cma_free_object, \
	.gem_vm_ops		= &drm_gem_cma_vm_ops, \
	.prime_handle_to_fd	= drm_gem_prime_handle_to_fd, \
	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle, \
	.gem_prime_import	= drm_gem_prime_import, \
	.gem_prime_export	= drm_gem_prime_export, \
	.gem_prime_get_sg_table	= drm_gem_cma_prime_get_sg_table, \
	.gem_prime_import_sg_table = tinydrm_gem_cma_prime_import_sg_table, \
	.gem_prime_vmap		= drm_gem_cma_prime_vmap, \
	.gem_prime_vunmap	= drm_gem_cma_prime_vunmap, \
	.gem_prime_mmap		= drm_gem_cma_prime_mmap, \
	.dumb_create		= drm_gem_cma_dumb_create, \
	.dumb_map_offset	= drm_gem_cma_dumb_map_offset, \
	.dumb_destroy		= drm_gem_dumb_destroy, \
	.debugfs_init		= tinydrm_debugfs_init, \
	.debugfs_cleanup	= tinydrm_debugfs_cleanup, \
	.fops			= &tinydrm_fops, \
	.name			= name_str, \
	.desc			= desc_str, \
	.date			= date_str, \
	.major			= 1, \
	.minor			= 0, \
}

extern const struct file_operations tinydrm_fops;
void tinydrm_lastclose(struct drm_device *drm);
void tinydrm_gem_cma_free_object(struct drm_gem_object *gem_obj);
struct drm_gem_object *
tinydrm_gem_cma_prime_import_sg_table(struct drm_device *drm,
				      struct dma_buf_attachment *attach,
				      struct sg_table *sgt);
struct drm_framebuffer *
tinydrm_fb_create(struct drm_device *drm, struct drm_file *file_priv,
		  const struct drm_mode_fb_cmd2 *mode_cmd);
int tinydrm_display_pipe_init(struct tinydrm_device *tdev,
			      const uint32_t *formats,
			      unsigned int format_count,
			      const struct drm_display_mode *mode);
int devm_tinydrm_init(struct device *parent, struct tinydrm_device *tdev,
		      struct drm_driver *driver);
int devm_tinydrm_register(struct tinydrm_device *tdev);
void tinydrm_shutdown(struct tinydrm_device *tdev);
int tinydrm_suspend(struct tinydrm_device *tdev);
int tinydrm_resume(struct tinydrm_device *tdev);

int tinydrm_fbdev_init(struct tinydrm_device *tdev);
void tinydrm_fbdev_fini(struct tinydrm_device *tdev);

#ifdef CONFIG_DEBUG_FS
int tinydrm_debugfs_init(struct drm_minor *minor);
void tinydrm_debugfs_dirty_begin(struct tinydrm_device *tdev,
				 struct drm_framebuffer *fb,
				 const struct drm_clip_rect *clip);
void tinydrm_debugfs_dirty_end(struct tinydrm_device *tdev, size_t len,
			       unsigned bits_per_pixel);
void tinydrm_debugfs_cleanup(struct drm_minor *minor);
int tinydrm_debugfs_dirty_init(struct tinydrm_device *tdev);
#else
int tinydrm_debugfs_dirty_init(struct tinydrm_device *tdev)
{
	return 0;
}

void tinydrm_debugfs_dirty_begin(struct tinydrm_device *tdev,
				 struct drm_framebuffer *fb,
				 const struct drm_clip_rect *clip)
{
}

void tinydrm_debugfs_dirty_end(struct tinydrm_device *tdev, size_t len,
			       unsigned bits_per_pixel)
{
}

#define tinydrm_debugfs_init	NULL
#define tinydrm_debugfs_cleanup	NULL
#endif

void tinydrm_merge_clips(struct drm_clip_rect *dst,
			 struct drm_clip_rect *src, unsigned num_clips,
			 unsigned flags, u32 max_width, u32 max_height);
void tinydrm_xrgb8888_to_rgb565(u32 *src, u16 *dst, unsigned num_pixels,
				bool swap_bytes);
int tinydrm_lcdreg_flush_rgb565(struct lcdreg *reg, u32 regnr,
				struct drm_framebuffer *fb, void *vmem,
				struct drm_clip_rect *clip);

#ifdef CONFIG_BACKLIGHT_CLASS_DEVICE
struct backlight_device *tinydrm_of_find_backlight(struct device *dev);
int tinydrm_enable_backlight(struct tinydrm_device *tdev);
void tinydrm_disable_backlight(struct tinydrm_device *tdev);
#else
static inline struct backlight_device *
tinydrm_of_find_backlight(struct device *dev)
{
	return NULL;
}

static inline int tinydrm_enable_backlight(struct tinydrm_device *tdev)
{
	return 0;
}

static inline int tinydrm_disable_backlight(struct tinydrm_device *tdev)
{
	return 0;
}
#endif

extern const struct dev_pm_ops tinydrm_simple_pm_ops;
void tinydrm_spi_shutdown(struct spi_device *spi);

#endif /* __LINUX_TINYDRM_H */
