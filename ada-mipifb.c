#define DEBUG

/*
 * Framebuffer driver for Adafruit MIPI compatible SPI displays
 *
 * Copyright 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/tinydrm/tinydrm.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/spi/spi.h>

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
drm_gem_object_unreference_unlocked(&cma_obj->base);

*/

static void tinydrm_deferred_io_work(struct work_struct *work)
{
	struct tinydrm_lcdctrl *tctrl = container_of(work, struct tinydrm_lcdctrl, deferred_work.work);
	struct drm_clip_rect clip;
static bool inside = false;

	// locking
	clip = *tctrl->clips;
	tinydrm_reset_clip(tctrl->clips);

	dev_info(tctrl->tdev.base->dev, "%s(inside=%u): x1=%u, x2=%u, y1=%u, y2=%u\n\n", __func__, inside, clip.x1, clip.x2, clip.y1, clip.y2);
inside = true;
	msleep(150);
	dev_info(tctrl->tdev.base->dev, "%s: done.\n\n", __func__);
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

static int ada_mipifb_panel_disable(struct drm_panel *panel)
{
//	struct tinydrm_device *tdev = connector_to_tinydrm(panel->connector);

	DRM_DEBUG_KMS("\n");

	return 0;
}

static int ada_mipifb_panel_unprepare(struct drm_panel *panel)
{
	DRM_DEBUG_KMS("\n");

	return 0;
}

static int ada_mipifb_panel_prepare(struct drm_panel *panel)
{
	DRM_DEBUG_KMS("\n");

	return 0;
}

static int ada_mipifb_panel_enable(struct drm_panel *panel)
{
	DRM_DEBUG_KMS("\n");

	return 0;
}

struct drm_panel_funcs ada_mipifb_drm_panel_funcs = {
	.disable = ada_mipifb_panel_disable,
	.unprepare = ada_mipifb_panel_unprepare,
	.prepare = ada_mipifb_panel_prepare,
	.enable = ada_mipifb_panel_enable,
};

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
	tdev->panel.funcs = &ada_mipifb_drm_panel_funcs;
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
