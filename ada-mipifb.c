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

#include <drm/drm_gem_cma_helper.h>
#include <drm/tinydrm/tinydrm.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/spi/spi.h>


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

static int ada_mipi_update(struct tinydrm_device *tdev)
{
	struct drm_gem_cma_object *cma_obj = tdev->dirty.cma_obj;
	struct drm_clip_rect clip;

	spin_lock(&tdev->dirty.lock);
	clip = tdev->dirty.clip;
	tinydrm_reset_clip(&tdev->dirty.clip);
	spin_unlock(&tdev->dirty.lock);

	dev_dbg(tdev->base->dev, "%s: cma_obj=%p, vaddr=%p, paddr=%pad\n", __func__, cma_obj, cma_obj->vaddr, &cma_obj->paddr);
	dev_dbg(tdev->base->dev, "%s: x1=%u, x2=%u, y1=%u, y2=%u\n", __func__, clip.x1, clip.x2, clip.y1, clip.y2);
	dev_dbg(tdev->base->dev, "\n");

	return 0;
}

static int ada_mipi_panel_disable(struct drm_panel *panel)
{
	struct tinydrm_device *tdev = tinydrm_from_panel(panel);

	dev_dbg(tdev->base->dev, "%s\n", __func__);

	return 0;
}

static int ada_mipi_panel_unprepare(struct drm_panel *panel)
{
	struct tinydrm_device *tdev = tinydrm_from_panel(panel);

	dev_dbg(tdev->base->dev, "%s\n", __func__);

	return 0;
}

static int ada_mipi_panel_prepare(struct drm_panel *panel)
{
	struct tinydrm_device *tdev = tinydrm_from_panel(panel);

	dev_dbg(tdev->base->dev, "%s\n", __func__);

	return 0;
}

static int ada_mipi_panel_enable(struct drm_panel *panel)
{
	struct tinydrm_device *tdev = tinydrm_from_panel(panel);

	dev_dbg(tdev->base->dev, "%s\n", __func__);

	return 0;
}

struct drm_panel_funcs ada_mipi_drm_panel_funcs = {
	.disable = ada_mipi_panel_disable,
	.unprepare = ada_mipi_panel_unprepare,
	.prepare = ada_mipi_panel_prepare,
	.enable = ada_mipi_panel_enable,
};

enum adafruit_displays {
	ADAFRUIT_358 = 358,
	ADAFRUIT_797 = 797,
	ADAFRUIT_1480 = 1480,
	ADAFRUIT_1601 = 1601,
};

static const struct of_device_id ada_mipi_ids[] = {
	{ .compatible = "adafruit,ada358",  .data = (void *)ADAFRUIT_358 },
	{ .compatible = "adafruit,ada797",  .data = (void *)ADAFRUIT_797 },
	{ .compatible = "adafruit,ada1480", .data = (void *)ADAFRUIT_1480 },
	{ .compatible = "adafruit,ada1601", .data = (void *)ADAFRUIT_1601 },
{ .compatible = "sainsmart18", .data = (void *)ADAFRUIT_358 },
	{},
};
MODULE_DEVICE_TABLE(of, ada_mipi_ids);

static int ada_mipi_probe(struct spi_device *spi)
{
	struct tinydrm_device *tdev;
	struct device *dev = &spi->dev;
	const struct of_device_id *of_id;

	of_id = of_match_device(ada_mipi_ids, dev);
	if (!of_id)
		return -EINVAL;

	tdev = devm_kzalloc(dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;

	tdev->panel.funcs = &ada_mipi_drm_panel_funcs;
	tdev->update = ada_mipi_update;
	tdev->dirty.defer_ms = 40;

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

static int ada_mipi_remove(struct spi_device *spi)
{
	struct tinydrm_device *tdev = spi_get_drvdata(spi);

	tinydrm_release(tdev);

	return 0;
}

static struct spi_driver ada_mipi_spi_driver = {
	.driver = {
		.name   = "ada-mipifb",
		.owner  = THIS_MODULE,
		.of_match_table = ada_mipi_ids,
	},
	.probe  = ada_mipi_probe,
	.remove = ada_mipi_remove,
};
module_spi_driver(ada_mipi_spi_driver);

//MODULE_ALIAS("spi:ada358");
//MODULE_ALIAS("spi:ada797");
//MODULE_ALIAS("spi:ada1480");
//MODULE_ALIAS("spi:ada1601");


MODULE_LICENSE("GPL");
