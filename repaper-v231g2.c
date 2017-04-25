/*
 * Copyright 2013-2017 Pervasive Displays, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/pwm.h>
#include <linux/spi/spi.h>

#include <drm/tinydrm/tinydrm.h>
#include <drm/tinydrm/tinydrm-helpers2.h>

#include "repaper.h"

#define BORDER_BYTE_BLACK 0xff
#define BORDER_BYTE_WHITE 0xaa
#define BORDER_BYTE_NULL  0x00


typedef enum {           // Image pixel -> Display pixel
	EPD_compensate,  // B -> W, W -> B (Current Image)
	EPD_white,       // B -> N, W -> W (Current Image)
	EPD_inverse,     // B -> N, W -> B (New Image)
	EPD_normal       // B -> B, W -> W (New Image)
} EPD_stage;


static int repaper_epd_write_buf(struct spi_device *spi, u8 reg, const u8 *buf, size_t len)
{
	u8 idx_buf[2] = { 0x70, reg };
	struct spi_transfer tr_idx = {
		.tx_buf  = idx_buf,
		.len = 2,
		.delay_usecs = 2,
	};
	u8 pre_data = 0x72;
	struct spi_transfer tr_data[2] = {
		{
			.tx_buf  = &pre_data,
			.len = 1,
		}, {
			.tx_buf  = buf,
			.len = len,
			.delay_usecs = 2,
		},
	};
	int ret;

	udelay(10);
	ret = spi_sync_transfer(spi, &tr_idx, 1);
	if (ret)
		return ret;

	udelay(10);
	return spi_sync_transfer(spi, tr_data, 2);
}

static int repaper_epd_write_val(struct spi_device *spi, u8 reg, u8 val)
{
	return repaper_epd_write_buf(spi, reg, &val, 1);
}

static int repaper_epd_read_val(struct spi_device *spi, u8 reg)
{
	u8 idx_buf[2] = { 0x70, reg };
	u8 tx_data[2] = { 0x73, 0x00 };
	u8 rx_data[2];
	struct spi_transfer tr_data = {
		.tx_buf  = tx_data,
		.rx_buf  = rx_data,
		.len = 2,
	};
	int ret;

	ret = spi_write(spi, idx_buf, 2);
	if (ret)
		return ret;

	ret = spi_sync_transfer(spi, &tr_data, 1);

	return ret ? ret : rx_data[1];
}

static int repaper_epd_read_id(struct spi_device *spi)
{
	u8 tx_data[2] = { 0x71, 0x00 };
	u8 rx_data[2];
	struct spi_transfer tr_data = {
		.tx_buf  = tx_data,
		.rx_buf  = rx_data,
		.len = 2,
	};
	int ret;

	ret = spi_sync_transfer(spi, &tr_data, 1);

	return ret ? ret : rx_data[1];
}

/*
 * FIXME
 * Is this needed, couldn't find ref to it in the datasheet?
 * Called SPI_on/SPI_off in userspace driver.
 */
static void repaper_epd_spi_mosi_low(struct spi_device *spi)
{
	const u8 buf[1] = { 0 };

	spi_write(spi, buf, 1);
}

/* pixels on display are numbered from 1 so even is actually bits 1,3,5,... */
static void even_pixels(struct repaper_epd *epd, u8 **pp, const u8 *data,
			u8 fixed_value, const u8 *mask, EPD_stage stage)
{
	unsigned b;

	for (b = 0; b < epd->bytes_per_line; ++b) {
		if (data) {
			u8 pixels = data[b] & 0xaa;
			u8 pixel_mask = 0xff;
			u8 p1, p2, p3, p4;

			if (mask) {
				pixel_mask = (mask[b] ^ pixels) & 0xaa;
				pixel_mask |= pixel_mask >> 1;
			}

			switch(stage) {
			case EPD_compensate:  // B -> W, W -> B (Current Image)
				pixels = 0xaa | ((pixels ^ 0xaa) >> 1);
				break;
			case EPD_white:       // B -> N, W -> W (Current Image)
				pixels = 0x55 + ((pixels ^ 0xaa) >> 1);
				break;
			case EPD_inverse:     // B -> N, W -> B (New Image)
				pixels = 0x55 | (pixels ^ 0xaa);
				break;
			case EPD_normal:       // B -> B, W -> W (New Image)
				pixels = 0xaa | (pixels >> 1);
				break;
			}

			pixels = (pixels & pixel_mask) | (~pixel_mask & 0x55);
			p1 = (pixels >> 6) & 0x03;
			p2 = (pixels >> 4) & 0x03;
			p3 = (pixels >> 2) & 0x03;
			p4 = (pixels >> 0) & 0x03;
			pixels = (p1 << 0) | (p2 << 2) | (p3 << 4) | (p4 << 6);
			*(*pp)++ = pixels;
		} else {
			*(*pp)++ = fixed_value;
		}
	}
}

/* pixels on display are numbered from 1 so odd is actually bits 0,2,4,... */
static void odd_pixels(struct repaper_epd *epd, u8 **pp, const u8 *data,
		       u8 fixed_value, const u8 *mask, EPD_stage stage)
{
	unsigned b;

	for (b = epd->bytes_per_line; b > 0; --b) {
		if (data) {
			u8 pixels = data[b - 1] & 0x55;
			u8 pixel_mask = 0xff;

			if (mask) {
				pixel_mask = (mask[b - 1] ^ pixels) & 0x55;
				pixel_mask |= pixel_mask << 1;
			}

			switch(stage) {
			case EPD_compensate:  /* B -> W, W -> B (Current Image) */
				pixels = 0xaa | (pixels ^ 0x55);
				break;
			case EPD_white:       /* B -> N, W -> W (Current Image) */
				pixels = 0x55 + (pixels ^ 0x55);
				break;
			case EPD_inverse:     /* B -> N, W -> B (New Image) */
				pixels = 0x55 | ((pixels ^ 0x55) << 1);
				break;
			case EPD_normal:       /* B -> B, W -> W (New Image) */
				pixels = 0xaa | pixels;
				break;
			}

			pixels = (pixels & pixel_mask) | (~pixel_mask & 0x55);
			*(*pp)++ = pixels;
		} else {
			*(*pp)++ = fixed_value;
		}
	}
}

/* interleave bits: (byte)76543210 -> (16 bit).7.6.5.4.3.2.1 */
static inline uint16_t interleave_bits(uint16_t value)
{
	value = (value | (value << 4)) & 0x0f0f;
	value = (value | (value << 2)) & 0x3333;
	value = (value | (value << 1)) & 0x5555;

	return value;
}

/* pixels on display are numbered from 1 */
static void all_pixels(struct repaper_epd *epd, u8 **pp, const u8 *data,
		       u8 fixed_value, const u8 *mask, EPD_stage stage)
{
	unsigned b;

	for (b = epd->bytes_per_line; b > 0; --b) {
		if (data) {
			u16 pixels = interleave_bits(data[b - 1]);
			u16 pixel_mask = 0xffff;

			if (mask) {
				u16 pixel_mask = interleave_bits(mask[b - 1]);

				pixel_mask = (pixel_mask ^ pixels) & 0x5555;
				pixel_mask |= pixel_mask << 1;
			}

			switch(stage) {
			case EPD_compensate:  /* B -> W, W -> B (Current Image) */
				pixels = 0xaaaa | (pixels ^ 0x5555);
				break;
			case EPD_white:       /* B -> N, W -> W (Current Image) */
				pixels = 0x5555 + (pixels ^ 0x5555);
				break;
			case EPD_inverse:     /* B -> N, W -> B (New Image) */
				pixels = 0x5555 | ((pixels ^ 0x5555) << 1);
				break;
			case EPD_normal:       /* B -> B, W -> W (New Image) */
				pixels = 0xaaaa | pixels;
				break;
			}

			pixels = (pixels & pixel_mask) | (~pixel_mask & 0x5555);
			*(*pp)++ = pixels >> 8;
			*(*pp)++ = pixels;
		} else {
			*(*pp)++ = fixed_value;
			*(*pp)++ = fixed_value;
		}
	}
}

/* output one line of scan and data bytes to the display */
static void one_line(struct repaper_epd *epd, unsigned int line, const u8 *data,
		     u8 fixed_value, const u8 *mask, EPD_stage stage)
{
	u8 *p = epd->line_buffer;
	u16 b;

	repaper_epd_spi_mosi_low(epd->spi);

	if (epd->pre_border_byte) {
		*p++ = 0x00;
	}

	if (epd->middle_scan) {
		/* data bytes */
		odd_pixels(epd, &p, data, fixed_value, mask, stage);

		/* scan line */
		for (b = epd->bytes_per_scan; b > 0; --b) {
			if (line / 4 == b - 1)
				*p++ = 0x03 << (2 * (line & 0x03));
			else
				*p++ = 0x00;
		}

		/* data bytes */
		even_pixels(epd, &p, data, fixed_value, mask, stage);

	} else {
		/*
		 * even scan line, but as lines on display are numbered from 1,
		 * line: 1,3,5,...
		 */
		for (b = 0; b < epd->bytes_per_scan; ++b) {
			if (0 != (line & 0x01) && line / 8 == b)
				*p++ = 0xc0 >> (line & 0x06);
			else
				*p++ = 0x00;
		}

		/* data bytes */
		all_pixels(epd, &p, data, fixed_value, mask, stage);

		/*
		 * odd scan line, but as lines on display are numbered from 1,
		 * line: 0,2,4,6,...
		 */
		for (b = epd->bytes_per_scan; b > 0; --b) {
			if (0 == (line & 0x01) && line / 8 == b - 1)
				*p++ = 0x03 << (line & 0x06);
			else
				*p++ = 0x00;
		}
	}

	switch (epd->border_byte) {
	case EPD_BORDER_BYTE_NONE:
		break;

	case EPD_BORDER_BYTE_ZERO:
		*p++ = 0x00;
		break;

	case EPD_BORDER_BYTE_SET:
		switch(stage) {
		case EPD_compensate:
		case EPD_white:
		case EPD_inverse:
			*p++ = 0x00;
			break;
		case EPD_normal:
			*p++ = 0xaa;
			break;
		}
		break;
	}

	repaper_epd_write_buf(epd->spi, 0x0a, epd->line_buffer,
			      p - epd->line_buffer);

	/* Output data to panel */
	repaper_epd_write_val(epd->spi, 0x02, 0x07);

	repaper_epd_spi_mosi_low(epd->spi);
}

static void nothing_frame(struct repaper_epd *epd)
{
	unsigned int line;

	for (line = 0; line < epd->lines_per_display; ++line)
		one_line(epd, 0x7fffu, NULL, 0x00, NULL, EPD_compensate);
}

static void dummy_line(struct repaper_epd *epd)
{
	one_line(epd, 0x7fffu, NULL, 0x00, NULL, EPD_compensate);
}

static void border_dummy_line(struct repaper_epd *epd)
{
	one_line(epd, 0x7fffu, NULL, 0x00, NULL, EPD_normal);
}

static void EPD_set_temperature(struct repaper_epd *epd, int temperature)
{
	unsigned int factor10x;

	if (temperature <= -10)
		factor10x = 170;
	else if (temperature <= -5)
		factor10x = 120;
	else if (temperature <= 5)
		factor10x = 80;
	else if (temperature <= 10)
		factor10x = 40;
	else if (temperature <= 15)
		factor10x = 30;
	else if (temperature <= 20)
		factor10x = 20;
	else if (temperature <= 40)
		factor10x = 10;
	else
		factor10x = 7;

	epd->factored_stage_time = epd->stage_time * factor10x / 10;
}

static void frame_fixed(struct repaper_epd *epd, u8 fixed_value, EPD_stage stage)
{
	unsigned int line;

	for (line = 0; line < epd->lines_per_display ; ++line)
		one_line(epd, line, NULL, fixed_value, NULL, stage);
}

static void frame_data(struct repaper_epd *epd, const u8 *image, const u8 *mask, EPD_stage stage)
{
	unsigned int line;

	if (!mask) {
		for (line = 0; line < epd->lines_per_display ; ++line) {
			one_line(epd, line, &image[line * epd->bytes_per_line],
				 0, NULL, stage);
		}
	} else {
		for (line = 0; line < epd->lines_per_display ; ++line) {
			size_t n = line * epd->bytes_per_line;

			one_line(epd, line, &image[n], 0, &mask[n], stage);
		}
	}
}

static void frame_fixed_repeat(struct repaper_epd *epd, u8 fixed_value, EPD_stage stage)
{
	u64 start = local_clock();
	u64 end = start + (epd->factored_stage_time * 1000 * 1000);
	int i = 0;

	do {
		frame_fixed(epd, fixed_value, stage);
		i++;
	} while (local_clock() < end);
//	printk("%s: iterations=%d\n", __func__, i);
}

static void frame_data_repeat(struct repaper_epd *epd, const u8 *image, const u8 *mask, EPD_stage stage)
{
	u64 start = local_clock();
	u64 end = start + (epd->factored_stage_time * 1000 * 1000);
	int i;

	do {
		frame_data(epd, image, mask, stage);
		i++;
	} while (local_clock() < end);
//	printk("%s: iterations=%d\n", __func__, i);
}







// bit reversed table
static const char repaper_reversed[256] = {
//	__00____01____02____03____04____05____06____07____08____09____0a____0b____0c____0d____0e____0f
	0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0, 0x10, 0x90, 0x50, 0xd0, 0x30, 0xb0, 0x70, 0xf0,
//	__10____11____12____13____14____15____16____17____18____19____1a____1b____1c____1d____1e____1f
	0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8, 0x68, 0xe8, 0x18, 0x98, 0x58, 0xd8, 0x38, 0xb8, 0x78, 0xf8,
//	__20____21____22____23____24____25____26____27____28____29____2a____2b____2c____2d____2e____2f
	0x04, 0x84, 0x44, 0xc4, 0x24, 0xa4, 0x64, 0xe4, 0x14, 0x94, 0x54, 0xd4, 0x34, 0xb4, 0x74, 0xf4,
//	__30____31____32____33____34____35____36____37____38____39____3a____3b____3c____3d____3e____3f
	0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec, 0x1c, 0x9c, 0x5c, 0xdc, 0x3c, 0xbc, 0x7c, 0xfc,
//	__40____41____42____43____44____45____46____47____48____49____4a____4b____4c____4d____4e____4f
	0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2, 0x12, 0x92, 0x52, 0xd2, 0x32, 0xb2, 0x72, 0xf2,
//	__50____51____52____53____54____55____56____57____58____59____5a____5b____5c____5d____5e____5f
	0x0a, 0x8a, 0x4a, 0xca, 0x2a, 0xaa, 0x6a, 0xea, 0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa,
//	__60____61____62____63____64____65____66____67____68____69____6a____6b____6c____6d____6e____6f
	0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6, 0x16, 0x96, 0x56, 0xd6, 0x36, 0xb6, 0x76, 0xf6,
//	__70____71____72____73____74____75____76____77____78____79____7a____7b____7c____7d____7e____7f
	0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee, 0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe,
//	__80____81____82____83____84____85____86____87____88____89____8a____8b____8c____8d____8e____8f
	0x01, 0x81, 0x41, 0xc1, 0x21, 0xa1, 0x61, 0xe1, 0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1,
//	__90____91____92____93____94____95____96____97____98____99____9a____9b____9c____9d____9e____9f
	0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9, 0x69, 0xe9, 0x19, 0x99, 0x59, 0xd9, 0x39, 0xb9, 0x79, 0xf9,
//	__a0____a1____a2____a3____a4____a5____a6____a7____a8____a9____aa____ab____ac____ad____ae____af
	0x05, 0x85, 0x45, 0xc5, 0x25, 0xa5, 0x65, 0xe5, 0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5,
//	__b0____b1____b2____b3____b4____b5____b6____b7____b8____b9____ba____bb____bc____bd____be____bf
	0x0d, 0x8d, 0x4d, 0xcd, 0x2d, 0xad, 0x6d, 0xed, 0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd,
//	__c0____c1____c2____c3____c4____c5____c6____c7____c8____c9____ca____cb____cc____cd____ce____cf
	0x03, 0x83, 0x43, 0xc3, 0x23, 0xa3, 0x63, 0xe3, 0x13, 0x93, 0x53, 0xd3, 0x33, 0xb3, 0x73, 0xf3,
//	__d0____d1____d2____d3____d4____d5____d6____d7____d8____d9____da____db____dc____dd____de____df
	0x0b, 0x8b, 0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb, 0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb,
//	__e0____e1____e2____e3____e4____e5____e6____e7____e8____e9____ea____eb____ec____ed____ee____ef
	0x07, 0x87, 0x47, 0xc7, 0x27, 0xa7, 0x67, 0xe7, 0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7,
//	__f0____f1____f2____f3____f4____f5____f6____f7____f8____f9____fa____fb____fc____fd____fe____ff
	0x0f, 0x8f, 0x4f, 0xcf, 0x2f, 0xaf, 0x6f, 0xef, 0x1f, 0x9f, 0x5f, 0xdf, 0x3f, 0xbf, 0x7f, 0xff
};

static void repaper_reverse_bits(char *buf, size_t len)
{
	int i;

	for (i = 0; i < len; i++)
		buf[i] = repaper_reversed[(int)buf[i]];
}



static int repaper_v231g2_fb_dirty(struct drm_framebuffer *fb,
				   struct drm_file *file_priv,
				   unsigned int flags, unsigned int color,
				   struct drm_clip_rect *clips,
				   unsigned int num_clips)
{
	struct tinydrm_device *tdev = fb->dev->dev_private;
	struct repaper_epd *epd = epd_from_tinydrm(tdev);
	struct drm_clip_rect clip;
	u8 *mono8 = NULL;
	int ret = 0;

	mutex_lock(&tdev->dirty_lock);

	if (!epd->enabled)
		goto out_unlock;

	/* fbdev can flush even when we're not interested */
	if (tdev->pipe.plane.fb != fb)
		goto out_unlock;

//	tinydrm_merge_clips(&clip, clips, num_clips, flags,
//				   fb->width, fb->height);

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

	ret = tinydrm_rgb565_buf_copy(epd->buf, fb, &clip, false);
	if (ret)
		goto out_unlock;

	tinydrm_rgb565_to_mono8(mono8, epd->buf, fb->width, fb->height);
	tinydrm_mono8_to_mono(epd->buf, mono8, fb->width, fb->height);

	repaper_reverse_bits(epd->buf, fb->width * fb->height / 8);

	/* FIXME: make it dynamic somehow */
	EPD_set_temperature(epd, 25);

	if (epd->partial) {
		frame_data_repeat(epd, epd->buf, epd->current_buffer, EPD_normal);
	} else if (epd->cleared) {
		/* Change from old image to new image */
		frame_data_repeat(epd, epd->current_buffer, NULL, EPD_compensate);
		frame_data_repeat(epd, epd->current_buffer, NULL, EPD_white);
		frame_data_repeat(epd, epd->buf, NULL, EPD_inverse);
		frame_data_repeat(epd, epd->buf, NULL, EPD_normal);

		epd->partial = true;
	} else {
		/* Clear display (anything -> white) */
		frame_fixed_repeat(epd, 0xff, EPD_compensate);
		frame_fixed_repeat(epd, 0xff, EPD_white);
		frame_fixed_repeat(epd, 0xaa, EPD_inverse);
		frame_fixed_repeat(epd, 0xaa, EPD_normal);

		/* Assuming a clear (white) screen output an image */
		frame_fixed_repeat(epd, 0xaa, EPD_compensate);
		frame_fixed_repeat(epd, 0xaa, EPD_white);
		frame_data_repeat(epd, epd->buf, NULL, EPD_inverse);
		frame_data_repeat(epd, epd->buf, NULL, EPD_normal);

		epd->cleared = true;
	}

	memcpy(epd->current_buffer, epd->buf, fb->width * fb->height / 8);

	DRM_DEBUG("End Flushing [FB:%d]\n", fb->base.id);

out_unlock:
	mutex_unlock(&tdev->dirty_lock);

	if (ret)
		dev_err_once(fb->dev->dev, "Failed to update display %d\n",
			     ret);
	kfree(mono8);

	return ret;
}

const struct drm_framebuffer_funcs repaper_v231g2_fb_funcs = {
	.destroy	= drm_fb_cma_destroy,
	.create_handle	= drm_fb_cma_create_handle,
	.dirty		= repaper_v231g2_fb_dirty,
};

static void power_off(struct repaper_epd *epd)
{
	/* Turn off power and all signals */
	gpiod_set_value_cansleep(epd->reset, 0);
	gpiod_set_value_cansleep(epd->panel_on, 0);
	gpiod_set_value_cansleep(epd->border, 0);

	/* Ensure SPI MOSI and CLOCK are Low before CS Low */
	repaper_epd_spi_mosi_low(epd->spi);

	/* Discharge pulse */
	gpiod_set_value_cansleep(epd->discharge, 1);
	msleep(150);
	gpiod_set_value_cansleep(epd->discharge, 0);
}

static void repaper_v231g2_pipe_enable(struct drm_simple_display_pipe *pipe,
				       struct drm_crtc_state *crtc_state)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct repaper_epd *epd = epd_from_tinydrm(tdev);
	struct spi_device *spi = epd->spi;
	struct device *dev = &spi->dev;
	bool dc_ok = false;
	int t, i, ret;

	DRM_DEBUG_DRIVER("Enable begin\n");

	/* Power up sequence */
	gpiod_set_value_cansleep(epd->reset, 0);
	gpiod_set_value_cansleep(epd->panel_on, 0);
	gpiod_set_value_cansleep(epd->discharge, 0);
	gpiod_set_value_cansleep(epd->border, 0);
	repaper_epd_spi_mosi_low(spi);
	msleep(5);

	gpiod_set_value_cansleep(epd->panel_on, 1);
	msleep(10);
	gpiod_set_value_cansleep(epd->reset, 1);
	gpiod_set_value_cansleep(epd->border, 1);
	msleep(5);
	gpiod_set_value_cansleep(epd->reset, 0);
	msleep(5);
	gpiod_set_value_cansleep(epd->reset, 1);
	msleep(5);

	/* Wait for COG to become ready */
	for (t = 100; t > 0; t--) {
		if (!gpiod_get_value_cansleep(epd->busy))
			break;

		udelay(10);
	}

	if (!t) {
		dev_err(dev, "timeout waiting for panel to become ready.\n");
		power_off(epd);
		return;
	}


#define SPI_RID_G1_COG_ID	0x11
#define SPI_RID_G2_COG_ID	0x12

	repaper_epd_read_id(spi);
	ret = repaper_epd_read_id(spi);
	if (ret != SPI_RID_G2_COG_ID) {
		if (ret < 0)
			dev_err(dev, "failed to read chip (%d)\n", ret);
		else
			dev_err(dev, "wrong COG ID 0x%02x\n", ret);
		power_off(epd);
		return;
	}

	/* Disable OE */
	repaper_epd_write_val(spi, 0x02, 0x40);

	ret = repaper_epd_read_val(spi, 0x0f);
	if (ret < 0 || !(ret & 0x80)) {
		if (ret < 0)
			dev_err(dev, "failed to read chip (%d)\n", ret);
		else
			dev_err(dev, "panel is reported broken\n");
		power_off(epd);
		return;
	}

	/* Power saving mode */
	repaper_epd_write_val(spi, 0x0b, 0x02);

	/* Channel select */
	repaper_epd_write_buf(spi, 0x01, epd->channel_select, epd->channel_select_length);

	/* High power mode osc */
	repaper_epd_write_val(spi, 0x07, 0xd1);

	/* Power setting */
	repaper_epd_write_val(spi, 0x08, 0x02);

	/* Vcom level */
	repaper_epd_write_val(spi, 0x09, 0xc2);

	/* Power setting */
	repaper_epd_write_val(spi, 0x04, 0x03);

	/* Driver latch on */
	repaper_epd_write_val(spi, 0x03, 0x01);

	/* Driver latch off */
	repaper_epd_write_val(spi, 0x03, 0x00);
	msleep(5);

	/* Start chargepump */
	for (i = 0; i < 4; ++i) {
		/* Charge pump positive voltage on - VGH/VDL on */
		repaper_epd_write_val(spi, 0x05, 0x01);
		msleep(240);

		/* Charge pump negative voltage on - VGL/VDL on */
		repaper_epd_write_val(spi, 0x05, 0x03);
		msleep(40);

		/* Charge pump Vcom on - Vcom driver on */
		repaper_epd_write_val(spi, 0x05, 0x0f);
		msleep(40);

		/* check DC/DC */
		ret = repaper_epd_read_val(spi, 0x0f);
		if (ret < 0) {
			dev_err(dev, "failed to read chip (%d)\n", ret);
			power_off(epd);
			return;
		}

		if (ret & 0x40) {
			dc_ok = true;
			break;
		}

	}

	if (!dc_ok) {
		dev_err(dev, "dc/dc failed\n");
		power_off(epd);
		return;
	}

	/* Output enable to disable */
	// FIXME: datasheet says: SPI(0x02,0x06)
	repaper_epd_write_val(spi, 0x02, 0x04);

	epd->enabled = true;

	DRM_DEBUG_DRIVER("Enable end\n");
}

static void repaper_v231g2_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct repaper_epd *epd = epd_from_tinydrm(tdev);
	struct spi_device *spi = epd->spi;

	DRM_DEBUG_DRIVER("Disable begin\n");

	mutex_lock(&tdev->dirty_lock);
	epd->enabled = false;
	epd->partial = false;
	mutex_unlock(&tdev->dirty_lock);

	nothing_frame(epd);

	if (epd->dots_per_line == 264) {
		dummy_line(epd);
		/* only pulse border pin for 2.70" EPD */
		msleep(25);
		gpiod_set_value_cansleep(epd->border, 0);
		msleep(200);
		gpiod_set_value_cansleep(epd->border, 1);
	} else {
		border_dummy_line(epd);
		msleep(200);
	}

	/* not described in datasheet */
	repaper_epd_write_val(spi, 0x0b, 0x00);

	/* Latch reset turn on */
	repaper_epd_write_val(spi, 0x03, 0x01);

	/* Power off charge pump Vcom */
	repaper_epd_write_val(spi, 0x05, 0x03);

	/* Power off charge pump neg voltage */
	repaper_epd_write_val(spi, 0x05, 0x01);
	msleep(120);

	/* Discharge internal */
	repaper_epd_write_val(spi, 0x04, 0x80);

	/* turn off all charge pumps */
	repaper_epd_write_val(spi, 0x05, 0x00);

	/* Turn off osc */
	repaper_epd_write_val(spi, 0x07, 0x01);
	msleep(50);

	power_off(epd);

	DRM_DEBUG_DRIVER("Disable end\n");
}

const struct drm_simple_display_pipe_funcs repaper_v231g2_pipe_funcs = {
	.enable = repaper_v231g2_pipe_enable,
	.disable = repaper_v231g2_pipe_disable,
	.update = tinydrm_display_pipe_update,
	.prepare_fb = tinydrm_display_pipe_prepare_fb,
};
