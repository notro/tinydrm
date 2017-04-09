/*
 * Copyright 2017 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/gpio/consumer.h>

#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_fb_cma_helper.h>
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
			    struct drm_clip_rect *clip, bool swap)
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

/*
 * RGB565 to monochrome conversion code is taken from fb_agm1264k-fl.c
 * Copyright (C) 2014 ololoshka2871
 */

static const s8 tinydrm_diffusing_matrix[2][2] = {
	{-1, 3},
	{3, 2},
};

static const u8 tinydrm_gray8_gamma_table[256] = {
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
	1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,
	3,3,3,3,3,4,4,4,4,5,5,5,5,6,6,6,
	6,7,7,7,8,8,8,9,9,9,10,10,11,11,11,12,
	12,13,13,13,14,14,15,15,16,16,17,17,18,18,19,19,
	20,20,21,22,22,23,23,24,25,25,26,26,27,28,28,29,
	30,30,31,32,33,33,34,35,35,36,37,38,39,39,40,41,
	42,43,43,44,45,46,47,48,49,49,50,51,52,53,54,55,
	56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,
	73,74,75,76,77,78,79,81,82,83,84,85,87,88,89,90,
	91,93,94,95,97,98,99,100,102,103,105,106,107,109,110,111,
	113,114,116,117,119,120,121,123,124,126,127,129,130,132,133,135,
	137,138,140,141,143,145,146,148,149,151,153,154,156,158,159,161,
	163,165,166,168,170,172,173,175,177,179,181,182,184,186,188,190,
	192,194,196,197,199,201,203,205,207,209,211,213,215,217,219,221,
	223,225,227,229,231,234,236,238,240,242,244,246,248,251,253,255
};

/*
 * tinydrm_rgb565_to_gray8 - Convert RGB565 to 8-bit grayscale
 * @gray8: Destination 8-bit grayscale buffer
 * @vmem16: Source RGB565 buffer
 * @width: Framebuffer width
 * @heigth: Framebuffer heigth
 * @table: Optional conversion table
 */
static void tinydrm_rgb565_to_gray8(u8 *gray8, u16 *vmem16, u32 width,
				    u32 height, const u8 *table)
{
	int x,y;

	for (x = 0; x < width; ++x)
		for (y = 0; y < height; ++y) {
			u16 pixel = vmem16[y *  width + x];
			u16 r = (pixel & (0x1f << 11)) >> 11;
			u16 g = (pixel & (0x3f << 5)) >> 5;
			u16 b = pixel & 0x1f;

			/* ITU-R BT.601: Y = 0.299R + 0.587G + 0.114B */
			pixel = (299 * r + 587 * g + 114 * b) / 200;

			if (table)
				gray8[y * width + x] = table[pixel];
			else
				gray8[y * width + x] = pixel;
		}
}

#define WHITE		0xff
#define BLACK		0

/*
 * tinydrm_gray8_to_mono8 - Convert 8-bit grayscale to monochrome
 * @vmem8: Source and destination buffer
 * @width: Framebuffer width
 * @heigth: Framebuffer heigth
 *
 * Uses Floyd–Steinberg dithering
 *
 */
static void tinydrm_gray8_to_mono8(u8 *vmem8, u32 width, u32 height)
{
	int x,y;

	for (x = 0; x < width; ++x)
		for (y = 0; y < height; ++y) {
			u8 pixel = vmem8[y * width + x];
			s16 error_b = pixel - BLACK;
			s16 error_w = pixel - WHITE;
			s16 error;
			u16 i, j;

			/* which color is close? */
			if (abs(error_b) >= abs(error_w)) {
				error = error_w;
				pixel = WHITE;
			} else {
				error = error_b;
				pixel = BLACK;
			}

			error /= 8;

			/* diffusion matrix row */
			for (i = 0; i < 2; ++i)
				/* diffusion matrix column */
				for (j = 0; j < 2; ++j) {
					u8 *val;
					s8 coeff;

					/* skip pixels out of zone */
					if (x + i < 0 ||
						x + i >= width
						|| y + j >= height)
						continue;

					val = &vmem8[(y + j) * width + x + i];
					coeff = tinydrm_diffusing_matrix[i][j];
					if (coeff == -1) {
						*val = pixel;
					} else {
						s16 p = *val + error * coeff;

						if (p > WHITE)
							p = WHITE;
						if (p < BLACK)
							p = BLACK;
						*val = (u8)p;
					}
				}
		}
}

void tinydrm_rgb565_to_mono8(u8 *mono8, u16 *vmem16, u32 width, u32 height)
{
	tinydrm_rgb565_to_gray8(mono8, vmem16, width, height,
				tinydrm_gray8_gamma_table);
	tinydrm_gray8_to_mono8(mono8, width, height);
}
EXPORT_SYMBOL(tinydrm_rgb565_to_mono8);


void tinydrm_mono8_to_mono(u8 *mono, u8 *mono8, u32 width, u32 height)
{
	int y, xb, i;

	for (y = 0; y < height; y++)
		for (xb = 0; xb < width / 8; xb++) {
			*mono = 0x00;
			for (i = 0; i < 8; i++) {
				int x = xb * 8 + i;

				*mono <<= 1;
				if (mono8[y * width + x])
					*mono |= 1;
			}
			mono++;
		}
}
EXPORT_SYMBOL(tinydrm_mono8_to_mono);

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
