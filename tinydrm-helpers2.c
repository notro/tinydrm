/*
 * Copyright 2017 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>

#include <drm/drm_device.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_fourcc.h>
#include <drm/tinydrm/tinydrm-helpers2.h>

/*

	This should be added to tinydrm-helpers.c

*/


/**
 * tinydrm_rgb565_buf_copy - Copy RGB565/XRGB8888 to RGB565 clip buffer
 * @dst: RGB565 destination buffer
 * @fb: DRM framebuffer
 * @clip: Clip rectangle area to copy
 * @swap: Swap bytes
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int tinydrm_rgb565_buf_copy(void *dst, struct drm_framebuffer *fb,
			    struct drm_rect *clip, bool swap)
{
	struct drm_gem_cma_object *cma_obj = drm_fb_cma_get_gem_obj(fb, 0);
	struct dma_buf_attachment *import_attach = cma_obj->base.import_attach;
	struct drm_format_name_buf format_name;
	void *src = cma_obj->vaddr;
	int ret = 0;

	if (import_attach) {
		ret = dma_buf_begin_cpu_access(import_attach->dmabuf,
					       DMA_FROM_DEVICE);
		if (ret)
			return ret;
	}

	switch (fb->format->format) {
	case DRM_FORMAT_RGB565:
		if (swap)
			tinydrm_swab16(dst, src, fb, clip);
		else
			tinydrm_memcpy(dst, src, fb, clip);
		break;
	case DRM_FORMAT_XRGB8888:
		tinydrm_xrgb8888_to_rgb565(dst, src, fb, clip, swap);
		break;
	default:
		dev_err_once(fb->dev->dev, "Format is not supported: %s\n",
			     drm_get_format_name(fb->format->format,
						 &format_name));
		return -EINVAL;
	}

	if (import_attach)
		ret = dma_buf_end_cpu_access(import_attach->dmabuf,
					     DMA_FROM_DEVICE);
	return ret;
}
EXPORT_SYMBOL(tinydrm_rgb565_buf_copy);

/**
 * tinydrm_hw_reset - Hardware reset of controller
 * @reset: GPIO connected to reset pin. Can be NULL.
 * @assert_ms: Time in milliseconds to assert reset. Can be zero.
 * @settle_ms: Time in milliseconds to wait for controller to become ready.
 *             Can be zero.
 *
 * Reset controller by pulling down the reset gpio for @assert_ms then pull up
 * and wait for @settle_ms.
 */
void tinydrm_hw_reset(struct gpio_desc *reset, unsigned int assert_ms,
		      unsigned int settle_ms)
{
	if (IS_ERR_OR_NULL(reset))
		return;

	gpiod_set_value_cansleep(reset, 0);
	if (assert_ms)
		msleep(assert_ms);
	gpiod_set_value_cansleep(reset, 1);
	if (settle_ms)
		msleep(settle_ms);
}
EXPORT_SYMBOL(tinydrm_hw_reset);

MODULE_LICENSE("GPL");
