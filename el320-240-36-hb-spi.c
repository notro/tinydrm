/*
 * DRM driver for Benq EL320.240.36-HB SPI display
 *
 * Copyright 2017 Noralf Trønnes
 *
 * RGB565 to monochrome conversion code is taken from fb_agm1264k-fl.c
 * Copyright (C) 2014 ololoshka2871
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/spi/spi.h>

#include <drm/tinydrm/tinydrm.h>
#include <drm/tinydrm/tinydrm-helpers.h>

/* will be added to helpers */
#include <linux/dma-buf.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_fb_cma_helper.h>
static
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








#define WHITE		0xff
#define BLACK		0

static const s8 tinydrm_diffusing_matrix[2][2] = {
	{-1, 3},
	{3, 2},
};

const u8 tinydrm_gray8_gamma_table[256] = {
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
static void tinydrm_rgb565_to_gray8(u8 *gray8, u16 *vmem16, u32 width, u32 height, const u8 *table)
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

static void tinydrm_rgb565_to_mono8(u8 *mono8, u16 *vmem16, u32 width, u32 height)
{
	tinydrm_rgb565_to_gray8(mono8, vmem16, width, height,
				tinydrm_gray8_gamma_table);
	tinydrm_gray8_to_mono8(mono8, width, height);
}



static void tinydrm_mono8_to_mono(u8 *mono, u8 *mono8, u32 width, u32 height)
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







/* is this 01h or 80h ? datasheet says both */
#define WRITE_COMPLETE_DISPLAY_DATA	0x01

struct el320_240_36_hb {
	struct tinydrm_device tinydrm;
	struct spi_device *spi;
	void *tx_buf;
};

static int el320_240_36_hb_fb_dirty(struct drm_framebuffer *fb,
			     struct drm_file *file_priv,
			     unsigned int flags, unsigned int color,
			     struct drm_clip_rect *clips,
			     unsigned int num_clips)
{
	struct tinydrm_device *tdev = fb->dev->dev_private;
	struct el320_240_36_hb *priv = container_of(tdev, struct el320_240_36_hb, tinydrm);
	struct spi_transfer tr_data[2] = { };
	struct drm_clip_rect clip;
	int ret = 0;
	u8 *mono8;

	mutex_lock(&tdev->dirty_lock);

	/* fbdev can flush even when we're not interested */
	if (tdev->pipe.plane.fb != fb)
		goto out_unlock;

//	tinydrm_merge_clips(&clip, clips, num_clips, flags,
//				   fb->width, fb->height);

	/* try partial updates later */
	clip.x1 = 0;
	clip.x2 = fb->width;
	clip.y1 = 0;
	clip.y2 = fb->height;

	DRM_DEBUG("Flushing [FB:%d] x1=%u, x2=%u, y1=%u, y2=%u\n",
		  fb->base.id, clip.x1, clip.x2, clip.y1, clip.y2);

	mono8 = kmalloc(fb->width * fb->height, GFP_KERNEL);
	if (!mono8) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	ret = tinydrm_rgb565_buf_copy(priv->tx_buf, fb, &clip, false);
	if (ret)
		goto out_unlock;

	tinydrm_rgb565_to_mono8(mono8, priv->tx_buf, fb->width, fb->height);
	tinydrm_mono8_to_mono(priv->tx_buf, mono8, fb->width, fb->height);

	/* reuse mono8 to store command */
	tr_data[0].tx_buf = mono8;
	tr_data[0].len = 1;
	*mono8 = WRITE_COMPLETE_DISPLAY_DATA;

	tr_data[1].tx_buf = priv->tx_buf;
	tr_data[1].len = fb->width * fb->height / 8;

#if 0

/* verify that the output looks sane by printing the first half of the last 16 lines (fbcon font height) */
{
	int x, y, i;
	u8 *vmem = priv->tx_buf;
	char buf[321];

	memset(buf, 0, sizeof(buf));
	for (y = 240 - 16; y < 240; y++) {
		for (x = 0; x < 320 / 8 / 2; x++) {
			u8 b = vmem[(y * 320 / 8) + x];

			for (i = 0; i < 8; i++) {
				buf[(x * 8) + i] = (b & BIT(7)) ? '*' : ' ';
				b <<= 1;
			}
		}
		printk("%s\n", buf);
	}

}

#endif

	ret = spi_sync_transfer(priv->spi, tr_data, 2);

out_unlock:
	mutex_unlock(&tdev->dirty_lock);

	if (ret)
		dev_err_once(fb->dev->dev, "Failed to update display %d\n",
			     ret);

	return ret;
}

static const struct drm_framebuffer_funcs el320_240_36_hb_fb_funcs = {
	.destroy	= drm_fb_cma_destroy,
	.create_handle	= drm_fb_cma_create_handle,
	.dirty		= el320_240_36_hb_fb_dirty,
};

static void el320_240_36_hb_pipe_enable(struct drm_simple_display_pipe *pipe,
					struct drm_crtc_state *crtc_state)
{
//	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);

	DRM_DEBUG_KMS("\n");
}

static void el320_240_36_hb_pipe_disable(struct drm_simple_display_pipe *pipe)
{
//	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);

	DRM_DEBUG_KMS("\n");
}

static const struct drm_simple_display_pipe_funcs el320_240_36_hb_pipe_funcs = {
	.enable = el320_240_36_hb_pipe_enable,
	.disable = el320_240_36_hb_pipe_disable,
	.update = tinydrm_display_pipe_update,
	.prepare_fb = tinydrm_display_pipe_prepare_fb,
};

static const uint32_t el320_240_36_hb_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

static const struct drm_display_mode el320_240_36_hb_mode = {
	TINYDRM_MODE(320, 240, 115, 86),
};

static struct drm_driver el320_240_36_hb_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME |
				  DRIVER_ATOMIC,
	TINYDRM_GEM_DRIVER_OPS,
	.lastclose		= tinydrm_lastclose,
	.name			= "el320-240-36-hb-spi",
	.desc			= "Benq EL320.240.36-HB SPI",
	.date			= "20170221",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id el320_240_36_hb_of_match[] = {
	{ .compatible = "beneq,el320-240-36-hb-spi" },
	{},
};
MODULE_DEVICE_TABLE(of, el320_240_36_hb_of_match);

static const struct spi_device_id el320_240_36_hb_id[] = {
	{ "el320-240-36-hb-spi", 0 },
	{ },
};
MODULE_DEVICE_TABLE(spi, el320_240_36_hb_id);

static int el320_240_36_hb_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct tinydrm_device *tdev;
	struct el320_240_36_hb *priv;
	int ret;

	/* The SPI device is used to allocate dma memory */
	if (!dev->coherent_dma_mask) {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_warn(dev, "Failed to set dma mask %d\n", ret);
			return ret;
		}
	}

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->tx_buf = devm_kmalloc(dev, 320 * 240 * 2, GFP_KERNEL);
	if (!priv->tx_buf)
		return -ENOMEM;

	priv->spi = spi;
	tdev = &priv->tinydrm;

	ret = devm_tinydrm_init(dev, tdev, &el320_240_36_hb_fb_funcs,
				&el320_240_36_hb_driver);
	if (ret)
		return ret;

	ret = tinydrm_display_pipe_init(tdev, &el320_240_36_hb_pipe_funcs,
					DRM_MODE_CONNECTOR_VIRTUAL,
					el320_240_36_hb_formats,
					ARRAY_SIZE(el320_240_36_hb_formats),
					&el320_240_36_hb_mode, 0);
	if (ret)
		return ret;

	tdev->drm->mode_config.preferred_depth = 16;

	drm_mode_config_reset(tdev->drm);

	ret = devm_tinydrm_register(tdev);
	if (ret)
		return ret;

	spi_set_drvdata(spi, tdev);

	DRM_DEBUG_DRIVER("Initialized %s:%s @%uMHz on minor %d\n",
			 tdev->drm->driver->name, dev_name(dev),
			 spi->max_speed_hz / 1000000,
			 tdev->drm->primary->index);

	return 0;
}

static void el320_240_36_hb_shutdown(struct spi_device *spi)
{
//	struct tinydrm_device *tdev = spi_get_drvdata(spi);
//
//	tinydrm_shutdown(tdev);
}

static struct spi_driver el320_240_36_hb_spi_driver = {
	.driver = {
		.name = "el320-240-36-hb",
		.owner = THIS_MODULE,
		.of_match_table = el320_240_36_hb_of_match,
	},
	.id_table = el320_240_36_hb_id,
	.probe = el320_240_36_hb_probe,
	.shutdown = el320_240_36_hb_shutdown,
};
module_spi_driver(el320_240_36_hb_spi_driver);

MODULE_DESCRIPTION("Benq EL320.240.36-HB SPI DRM driver");
MODULE_AUTHOR("Noralf Trønnes");
MODULE_LICENSE("GPL");
