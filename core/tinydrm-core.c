#define DEBUG
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



#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/spi/spi.h>

#include "internal.h"

/*
 *
 *  Connector
 *
 */

// http://lxr.free-electrons.com/source/drivers/gpu/drm/rcar-du/tinydrm_hdmicon.c#L55

static inline struct tinydrm_device *connector_to_tinydrm(struct drm_connector *connector)
{
	return container_of(connector, struct tinydrm_device, connector);
}


static int tinydrm_connector_get_modes(struct drm_connector *connector)
{
	struct tinydrm_device *tdev = connector_to_tinydrm(connector);
	struct drm_display_mode *mode;

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




/*
 *
 *  Encoder
 *
 */

// http://lxr.free-electrons.com/source/drivers/gpu/drm/rcar-du/tinydrm_encoder.c

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

// either dpms or enable/disable
	.disable = tinydrm_encoder_disable,
	.enable = tinydrm_encoder_enable,

// OR mode_fixup()
	.atomic_check = tinydrm_encoder_atomic_check,
};

static const struct drm_encoder_funcs tinydrm_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};




/*
 *
 *  Crtc
 *
 */

// http://lxr.free-electrons.com/source/drivers/gpu/drm/rcar-du/tinydrm_crtc.c

static void tinydrm_crtc_enable(struct drm_crtc *crtc)
{
	struct drm_device *ddev = crtc->dev;
	struct tinydrm_device *tdev = ddev->dev_private;

	dev_info(ddev->dev, "%s: enabled=%u\n", __func__, tdev->enabled);
	if (tdev->enabled)
		return;

	if (tdev->enable) {
		int ret = tdev->enable(tdev);

		if (ret)
			dev_err(ddev->dev, "enable() failed %d\n", ret);
	}

	tdev->enabled = true;
}

static void tinydrm_crtc_disable(struct drm_crtc *crtc)
{
	struct drm_device *ddev = crtc->dev;
	struct tinydrm_device *tdev = ddev->dev_private;

	dev_info(ddev->dev, "%s: enabled=%u\n", __func__, tdev->enabled);
	if (!tdev->enabled)
		return;

	if (tdev->disable) {
		int ret = tdev->disable(tdev);

		if (ret)
			dev_err(ddev->dev, "disable() failed %d\n", ret);
	}

	tdev->enabled = false;
}

static bool tinydrm_crtc_mode_fixup(struct drm_crtc *crtc,
				    const struct drm_display_mode *mode,
				    struct drm_display_mode *adjusted_mode)
{
	return true;
}

static const struct drm_crtc_helper_funcs tinydrm_crtc_helper_funcs = {
	.mode_fixup = tinydrm_crtc_mode_fixup,
	.disable = tinydrm_crtc_disable,
	.enable = tinydrm_crtc_enable,
};

static const struct drm_crtc_funcs tinydrm_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};


/*
 *
 *  Framebuffer
 *
 */


//#define to_tinydrm_framebuffer(x) container_of(x, struct tinydrm_framebuffer, base)
static inline struct tinydrm_framebuffer *to_tinydrm_framebuffer(struct drm_framebuffer *fb)
{
	return container_of(fb, struct tinydrm_framebuffer, base);
}

static void tinydrm_framebuffer_destroy(struct drm_framebuffer *fb)
{
	struct tinydrm_framebuffer *tinydrm_fb = to_tinydrm_framebuffer(fb);

// With this, running ~/docs/drm-howto/modeset whacks everything and results in oopses that renders the system unusable
//	if (tinydrm_fb->cma_obj)
//		drm_gem_cma_free_object(&tinydrm_fb->cma_obj->base);

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

	return tdev->dirty(fb, tfb->cma_obj, flags, color, clips, num_clips);
}

static const struct drm_framebuffer_funcs tinydrm_fb_funcs = {
	.destroy = tinydrm_framebuffer_destroy,
	.dirty = tinydrm_framebuffer_dirty,
/*	TODO?
 *	.create_handle = tinydrm_framebuffer_create_handle, */
};


/*

drm_fb_cma_helper.c

doesn't have .dirty

http://lxr.free-electrons.com/ident?i=drm_fb_cma_funcs

 * Userspace may annotate the updates, the annotates are a
 * promise made by the caller that the change is either a copy
 * of pixels or a fill of a single color in the region specified.
 *
 * If the DRM_MODE_FB_DIRTY_ANNOTATE_COPY flag is given then
 * the number of updated regions are half of num_clips given,
 * where the clip rects are paired in src and dst. The width and
 * height of each one of the pairs must match.
 *
 * If the DRM_MODE_FB_DIRTY_ANNOTATE_FILL flag is given the caller
 * promises that the region specified of the clip rects is filled
 * completely with a single color as given in the color argument.
struct drm_mode_fb_dirty_cmd {
        __u32 flags;
        __u32 color;

*/

//static struct drm_framebuffer_funcs drm_fb_cma_funcs = {
//	.destroy	= drm_fb_cma_destroy,
//	.create_handle	= drm_fb_cma_create_handle,
//};
//
//static const struct drm_framebuffer_funcs qxl_fb_funcs = {
//        .destroy = qxl_user_framebuffer_destroy,
//        .dirty = qxl_framebuffer_surface_dirty, // no FILL
///*      TODO?
// *      .create_handle = qxl_user_framebuffer_create_handle, */
//};
//
//static struct drm_framebuffer_funcs vmw_framebuffer_surface_funcs = {
//        .destroy = vmw_framebuffer_surface_destroy,
//        .dirty = vmw_framebuffer_surface_dirty, // same as qxl
//};
//
//static const struct drm_framebuffer_funcs virtio_gpu_fb_funcs = {
//        .destroy = virtio_gpu_user_framebuffer_destroy,
//        .dirty = virtio_gpu_framebuffer_surface_dirty, // no FILL/COPY support
//};
//
//static const struct drm_framebuffer_funcs udlfb_funcs = {
//        .destroy = udl_user_framebuffer_destroy,
//        .dirty = udl_user_framebuffer_dirty, // no FILL/COPY support
//};
//
//static struct drm_framebuffer_funcs exynos_drm_fb_funcs = {
//        .destroy        = exynos_drm_fb_destroy,
//        .create_handle  = exynos_drm_fb_create_handle,
//        .dirty          = exynos_drm_fb_dirty, // empty
//};
//
//static const struct drm_framebuffer_funcs omap_framebuffer_funcs = {
//        .create_handle = omap_framebuffer_create_handle,
//        .destroy = omap_framebuffer_destroy,
//        .dirty = omap_framebuffer_dirty, // empty
//};
//
//static const struct drm_framebuffer_funcs msm_framebuffer_funcs = {
//        .create_handle = msm_framebuffer_create_handle,
//        .destroy = msm_framebuffer_destroy,
//        .dirty = msm_framebuffer_dirty, // empty
//};





/*
 *
 *  Mode
 *
 */

// http://lxr.free-electrons.com/source/drivers/gpu/drm/rcar-du/tinydrm_kms.c



static struct drm_framebuffer *
tinydrm_fb_create(struct drm_device *ddev, struct drm_file *file_priv,
		  struct drm_mode_fb_cmd2 *mode_cmd)
{
//	struct tinydrm_device *tdev = ddev->dev_private;
	struct tinydrm_framebuffer *tinydrm_fb;
	struct drm_gem_object *obj;
	int ret;


	dev_dbg(ddev->dev, "%s\n", __func__);

	/* Validate the pixel format, size and pitches */
//	mode_cmd->pixel_format
//	mode_cmd->width
//	mode_cmd->height
//	mode_cmd->pitches
DRM_DEBUG_KMS("%s: pixel_format=%s\n", __func__, drm_get_format_name(mode_cmd->pixel_format));
DRM_DEBUG_KMS("%s: width=%u\n", __func__, mode_cmd->width);
DRM_DEBUG_KMS("%s: height=%u\n", __func__, mode_cmd->height);
DRM_DEBUG_KMS("%s: pitches[0]=%u\n", __func__, mode_cmd->pitches[0]);

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

/*
static int tinydrm_atomic_commit(struct drm_device *dev,
				 struct drm_atomic_state *state, bool async)
{
	int ret;

	ret = drm_atomic_helper_prepare_planes(dev, state);
	if (ret)
		return ret;

	drm_atomic_helper_swap_state(dev, state);

	return 0;
}
*/

static const struct drm_mode_config_funcs tinydrm_mode_config_funcs = {
	.fb_create = tinydrm_fb_create,
//	.output_poll_changed = tinydrm_output_poll_changed,
	.atomic_check = drm_atomic_helper_check,
//	.atomic_commit = tinydrm_atomic_commit,
	.atomic_commit = drm_atomic_helper_commit,
};


/*
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

/*
static void tinydrm_plane_destroy(struct drm_plane *plane)
{
	kfree(plane);
}
*/

static const struct drm_plane_funcs tinydrm_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
//	.destroy		= tinydrm_plane_destroy,
	.destroy		= drm_primary_helper_destroy,
	.reset			= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state	= drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
};

static int tinydrm_plane_atomic_check(struct drm_plane *plane,
                                         struct drm_plane_state *state)
{
	return 0;
}

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
	.atomic_check = tinydrm_plane_atomic_check,
	.atomic_update = tinydrm_plane_atomic_update,
};


static int tinydrm_load(struct drm_device *ddev, unsigned long flags)
{
	struct tinydrm_device *tdev = ddev->dev_private;
	int ret;

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

	drm_mode_config_reset(ddev);

	ret = tinydrm_fbdev_init(tdev);
	if (ret)
		return ret;

	return 0;
}

/*

we use drm_dev_set_unique()

int tinydrm_set_busid(struct drm_device *ddev, struct drm_master *master)
{
	int id;

//	id = dev->platformdev->id;
//	if (id < 0)
		id = 0;

	master->unique = kasprintf(GFP_KERNEL, "tinydrm:%s:%02d",
                                                dev_name(ddev->dev), id);
	if (!master->unique)
		return -ENOMEM;

	master->unique_len = strlen(master->unique);
	return 0;
}
*/

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
//	.preclose		= tinydrm_preclose,
//	.lastclose		= tinydrm_lastclose,
//	.set_busid		= tinydrm_set_busid,
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


static void tinydrm_release(struct tinydrm_device *tdev)
{
printk("%s\n", __func__);
//	drm_put_dev(tdev->base);

	tinydrm_fbdev_fini(tdev);
	drm_dev_unregister(tdev->base);
	drm_dev_unref(tdev->base);
}

static int tinydrm_register(struct device *dev, struct tinydrm_device *tdev)
{
	struct drm_driver *driver = &tinydrm_driver;
	struct drm_device *ddev;
	int ret;

dev_info(dev, "%s\n", __func__);

	DRM_DEBUG("\n");

dev->coherent_dma_mask = DMA_BIT_MASK(32);

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




static int drv_enable(struct tinydrm_device *tdev)
{
	dev_info(tdev->base->dev, "%s()\n", __func__);

	return 0;
}

static int drv_disable(struct tinydrm_device *tdev)
{
	dev_info(tdev->base->dev, "%s()\n", __func__);

	return 0;
}

static inline void tinydrm_reset_clip(struct drm_clip_rect *clip)
{
	clip->x1 = ~0;
	clip->x2 = 0;
	clip->y1 = ~0;
	clip->y2 = 0;
}

void tinydrm_merge_clips(struct drm_clip_rect *dst, struct drm_clip_rect *clips, unsigned num_clips, unsigned flags)
{
	int i;

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
}

#include <linux/workqueue.h>

struct tinydrm_lcdctrl {
	struct tinydrm_device tdev;

	struct delayed_work deferred_work;
	unsigned defer_ms;

//	clips_lock
	struct drm_clip_rect *clips;
	unsigned num_clips;
};

static inline struct tinydrm_lcdctrl *to_tinydrm_lcdctrl(struct tinydrm_device *tdev)
{
	return container_of(tdev, struct tinydrm_lcdctrl, tdev);
}

#include <linux/delay.h>


// what happens if entering while running?

/*

Is it possible that the framebuffer has been released when we get here?

https://www.kernel.org/doc/htmldocs/drm/drm-mode-setting.html

The lifetime of a drm framebuffer is controlled with a reference count,
drivers can grab additional references with drm_framebuffer_reference and
drop them again with drm_framebuffer_unreference. For driver-private
framebuffers for which the last reference is never dropped (e.g. for the
fbdev framebuffer when the struct drm_framebuffer is embedded into the fbdev
helper struct) drivers can manually clean up a framebuffer at module unload
time with drm_framebuffer_unregister_private.

I need a reference on drm_gem_cma_object

136 static inline void
137 drm_gem_object_reference(struct drm_gem_object *obj)
138 {
139         kref_get(&obj->refcount);
140 }
141
142 static inline void
143 drm_gem_object_unreference(struct drm_gem_object *obj)
144 {
145         if (obj != NULL)
146                 kref_put(&obj->refcount, drm_gem_object_free);
147 }

754  **
755  * drm_gem_object_free - free a GEM object
756  * @kref: kref of the object to free
757  *
758  * Called after the last reference to the object has been lost.
759  * Must be called holding struct_ mutex
760  *
761  * Frees the object
762  *
763 void
764 drm_gem_object_free(struct kref *kref)
765 {
766         struct drm_gem_object *obj = (struct drm_gem_object *) kref;
767         struct drm_device *dev = obj->dev;
768
769         BUG_ON(!mutex_is_locked(&dev->struct_mutex));
770
771         if (dev->driver->gem_free_object != NULL)
772                 dev->driver->gem_free_object(obj);
773 }

177  **
178  * drm_gem_cma_free_object - free resources associated with a CMA GEM object
179  * @gem_obj: GEM object to free
180  *
181  * This function frees the backing memory of the CMA GEM object, cleans up the
182  * GEM object state and frees the memory used to store the object itself.
183  * Drivers using the CMA helpers should set this as their DRM driver's
184  * ->gem_free_object() callback.
185  *
186 void drm_gem_cma_free_object(struct drm_gem_object *gem_obj)
187 {
188         struct drm_gem_cma_object *cma_obj;
189
190         cma_obj = to_drm_gem_cma_obj(gem_obj);
191
192         if (cma_obj->vaddr) {
193                 dma_free_writecombine(gem_obj->dev->dev, cma_obj->base.size,
194                                       cma_obj->vaddr, cma_obj->paddr);
195         } else if (gem_obj->import_attach) {
196                 drm_prime_gem_destroy(gem_obj, cma_obj->sgt);
197         }
198
199         drm_gem_object_release(gem_obj);
200
201         kfree(cma_obj);
202 }

How can I only take one reference in dirty, so I can release it once in deferred?

drm_gem_object_reference(&cma_obj->base);
drm_gem_object_unreference(&cma_obj->base);

*/

static void tinydrm_deferred_io_work(struct work_struct *work)
{
	struct tinydrm_lcdctrl *tctrl = container_of(work, struct tinydrm_lcdctrl, deferred_work.work);
	struct drm_clip_rect clip;
static bool inside = false;

inside = true;
	clip = *tctrl->clips;
	tinydrm_reset_clip(tctrl->clips);

	dev_info(tctrl->tdev.base->dev, "%s(inside=%u): x1=%u, x2=%u, y1=%u, y2=%u\n\n", __func__, inside, clip.x1, clip.x2, clip.y1, clip.y2);
	msleep(150);
inside = false;
}

static int drv_dirty(struct drm_framebuffer *fb, struct drm_gem_cma_object *cma_obj, unsigned flags,
		     unsigned color, struct drm_clip_rect *clips, unsigned num_clips)
{
	struct tinydrm_device *tdev = fb->dev->dev_private;
	struct tinydrm_lcdctrl *tctrl = to_tinydrm_lcdctrl(tdev);
	struct drm_clip_rect full_clip = {
		.x1 = 0,
		.x2 = fb->width - 1,
		.y1 = 0,
		.y2 = fb->height - 1,
	};
	int i;

	dev_info(tdev->base->dev, "%s(cma_obj=%p, flags=0x%x, color=0x%x, clips=%p, num_clips=%u)\n", __func__, cma_obj, flags, color, clips, num_clips);

	if (num_clips == 0) {
		clips = &full_clip;
		num_clips = 1;
	}

	for (i = 0; i < num_clips; i++)
		dev_info(tdev->base->dev, "    x1=%u, x2=%u, y1=%u, y2=%u\n", clips[i].x1, clips[i].x2, clips[i].y1, clips[i].y2);

	tinydrm_merge_clips(tctrl->clips, clips, num_clips, flags);

	if (tctrl->clips[0].x1 == 0 && tctrl->clips[0].x2 == (fb->width - 1) && tctrl->clips[0].y1 == 0 && tctrl->clips[0].y2 == (fb->height - 1))
		schedule_delayed_work(&tctrl->deferred_work, 0);
	else
		schedule_delayed_work(&tctrl->deferred_work, msecs_to_jiffies(tctrl->defer_ms));

	return 0;
}




enum adafruit_displays {
	ADAFRUIT_358 = 358,
	ADAFRUIT_797 = 797,
	ADAFRUIT_1480 = 1480,
	ADAFRUIT_1601 = 1601,
};


static const struct of_device_id ada_mipifb_ids[] = {
	{ .compatible = "adafruit,ada358",  .data = (void *)ADAFRUIT_358 },
	{ .compatible = "adafruit,ada797",  .data = (void *)ADAFRUIT_797 },
	{ .compatible = "adafruit,ada1480", .data = (void *)ADAFRUIT_1480 },
	{ .compatible = "adafruit,ada1601", .data = (void *)ADAFRUIT_1601 },
{ .compatible = "sainsmart18", .data = (void *)ADAFRUIT_358 },
	{},
};
MODULE_DEVICE_TABLE(of, ada_mipifb_ids);

static int ada_mipifb_probe(struct spi_device *spi)
{
	struct tinydrm_lcdctrl *tctrl;
	struct tinydrm_device *tdev;
	struct device *dev = &spi->dev;
	const struct of_device_id *of_id;

	of_id = of_match_device(ada_mipifb_ids, dev);
	if (!of_id)
		return -EINVAL;

//	tdev = devm_kzalloc(dev, sizeof(*tdev), GFP_KERNEL);
//	if (!tdev)
//		return -ENOMEM;
	tctrl = devm_kzalloc(dev, sizeof(*tctrl), GFP_KERNEL);
	if (!tctrl)
		return -ENOMEM;

	tctrl->defer_ms = 50;
	tctrl->clips = devm_kzalloc(dev, sizeof(*tctrl->clips), GFP_KERNEL);
	if (!tctrl->clips)
		return -ENOMEM;

	INIT_DELAYED_WORK(&tctrl->deferred_work, tinydrm_deferred_io_work);
	tinydrm_reset_clip(tctrl->clips);


	tdev = &tctrl->tdev;

	tdev->enable = drv_enable;
	tdev->disable = drv_disable;
	tdev->dirty = drv_dirty;

	switch ((int)of_id->data) {
	case ADAFRUIT_358:
tdev->width = 240;
tdev->height = 240;
		break;
	case ADAFRUIT_797:
tdev->width = 320;
tdev->height = 320;
		break;
	case ADAFRUIT_1480:
	case ADAFRUIT_1601:
		tdev->width = 240;
		tdev->height = 320;
		break;
	default:
		return -EINVAL;
	}

	spi_set_drvdata(spi, tdev);

	return tinydrm_register(dev, tdev);
}

static int ada_mipifb_remove(struct spi_device *spi)
{
	struct tinydrm_device *tdev = spi_get_drvdata(spi);

	tinydrm_release(tdev);

	return 0;
}

static struct spi_driver ada_mipifb_spi_driver = {
	.driver = {
		.name   = "ada-mipifb",
		.owner  = THIS_MODULE,
		.of_match_table = ada_mipifb_ids,
	},
	.probe  = ada_mipifb_probe,
	.remove = ada_mipifb_remove,
};
module_spi_driver(ada_mipifb_spi_driver);

//MODULE_ALIAS("spi:ada358");
//MODULE_ALIAS("spi:ada797");
//MODULE_ALIAS("spi:ada1480");
//MODULE_ALIAS("spi:ada1601");


MODULE_LICENSE("GPL");
