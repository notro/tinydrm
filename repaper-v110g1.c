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

typedef enum {           // Image pixel -> Display pixel
	EPD_compensate,  // B -> W, W -> B (Current Image)
	EPD_white,       // B -> N, W -> W (Current Image)
	EPD_inverse,     // B -> N, W -> B (New Image)
	EPD_normal       // B -> B, W -> W (New Image)
} EPD_stage;

// function prototypes
static void power_off(struct repaper_epd *epd);
static int temperature_to_factor_10x(int temperature);
static void frame_fixed(struct repaper_epd *epd, u8 fixed_value, EPD_stage stage);
static void frame_data(struct repaper_epd *epd, const u8 *image, const u8 *mask, EPD_stage stage);
static void frame_fixed_repeat(struct repaper_epd *epd, u8 fixed_value, EPD_stage stage);
static void frame_data_repeat(struct repaper_epd *epd, const u8 *image, const u8 *mask, EPD_stage stage);
static void line(struct repaper_epd *epd, uint16_t line, const u8 *data, u8 fixed_value, const u8 *mask, EPD_stage stage);

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

/*
 * Is this needed, couldn't find ref to it in the datasheet?
 * Called SPI_on/SPI_off in userspace driver.
 */
static void repaper_epd_spi_mosi_low(struct spi_device *spi)
{
	const u8 buf[1] = { 0 };

	spi_write(spi, buf, 1);
}

static void EPD_begin(struct repaper_epd *epd)
{
	struct spi_device *spi = epd->spi;
	u8 vcom_config[2] = { 0xd0, 0x00 };
	int t;

	// power up sequence
	gpiod_set_value_cansleep(epd->reset, 0);
	gpiod_set_value_cansleep(epd->panel_on, 0);
	gpiod_set_value_cansleep(epd->discharge, 0);
	gpiod_set_value_cansleep(epd->border, 0);

	repaper_epd_spi_mosi_low(spi);

	pwm_enable(epd->pwm);
	msleep(25);
	gpiod_set_value_cansleep(epd->panel_on, 1);
	msleep(10);

	gpiod_set_value_cansleep(epd->reset, 1);
	gpiod_set_value_cansleep(epd->border, 1);
	msleep(5);

	gpiod_set_value_cansleep(epd->reset, 0);
	msleep(5);

	gpiod_set_value_cansleep(epd->reset, 1);
	msleep(5);

	// wait for COG to become ready
	for (t = 100; t > 0; t--) {
		if (!gpiod_get_value_cansleep(epd->busy))
			break;

		udelay(10);
	}

	if (!t) {
		pr_err("timeout waiting for panel to become ready.\n");
		return;// -ETIMEDOUT;
	}

	// channel select
	repaper_epd_write_buf(spi, 0x01, epd->channel_select, epd->channel_select_length);

	// DC/DC frequency
	repaper_epd_write_val(spi, 0x06, 0xff);

	// high power mode osc
	repaper_epd_write_val(spi, 0x07, 0x9d);

	// disable ADC
	repaper_epd_write_val(spi, 0x08, 0x00);

	// Vcom level
	repaper_epd_write_buf(spi, 0x09, vcom_config, sizeof(vcom_config));

	// gate and source voltage levels
	repaper_epd_write_buf(spi, 0x04, epd->gate_source, epd->gate_source_length);
	msleep(5);  //???

	// driver latch on
	repaper_epd_write_val(spi, 0x03, 0x01);

	// driver latch off
	repaper_epd_write_val(spi, 0x03, 0x00);
	msleep(5);

	// charge pump positive voltage on
	repaper_epd_write_val(spi, 0x05, 0x01);

	// final delay before PWM off
	msleep(30);
	pwm_disable(epd->pwm);

	// charge pump negative voltage on
	repaper_epd_write_val(spi, 0x05, 0x03);
	msleep(30);

	// Vcom driver on
	repaper_epd_write_val(spi, 0x05, 0x0f);
	msleep(30);

	// output enable to disable
	repaper_epd_write_val(spi, 0x02, 0x24);

	repaper_epd_spi_mosi_low(spi);
}

static void EPD_end(struct repaper_epd *epd)
{
	struct spi_device *spi = epd->spi;

	// dummy frame
	frame_fixed(epd, 0x55, EPD_normal);

	// dummy line and border
	if (epd->dots_per_line == 128) {
		// only for 1.44" EPD
		line(epd, 0x7fffu, 0, 0x55, NULL, EPD_normal);

		msleep(250);

	} else {
		// all other display sizes
		line(epd, 0x7fffu, 0, 0x55, NULL, EPD_normal);
		msleep(25);

		gpiod_set_value_cansleep(epd->border, 0);
		msleep(250);
		gpiod_set_value_cansleep(epd->border, 1);
	}

	repaper_epd_spi_mosi_low(spi);

	// latch reset turn on
	repaper_epd_write_val(spi, 0x03, 0x01);

	// output enable off
	repaper_epd_write_val(spi, 0x02, 0x05);

	// Vcom power off
	repaper_epd_write_val(spi, 0x05, 0x0e);

	// power off negative charge pump
	repaper_epd_write_val(spi, 0x05, 0x02);

	// discharge
	repaper_epd_write_val(spi, 0x04, 0x0c);
	msleep(120);

	// all charge pumps off
	repaper_epd_write_val(spi, 0x05, 0x00);

	// turn of osc
	repaper_epd_write_val(spi, 0x07, 0x0d);

	// discharge internal - 1
	repaper_epd_write_val(spi, 0x04, 0x50);
	msleep(40);

	// discharge internal - 2
	repaper_epd_write_val(spi, 0x04, 0xa0);
	msleep(40);

	// discharge internal - 3
	repaper_epd_write_val(spi, 0x04, 0x00);

	power_off(epd);
}

static void power_off(struct repaper_epd *epd)
{
	// turn of power and all signals
	gpiod_set_value_cansleep(epd->reset, 0);
	gpiod_set_value_cansleep(epd->panel_on, 0);
	gpiod_set_value_cansleep(epd->border, 0);

	// ensure SPI MOSI and CLOCK are Low before CS Low
	repaper_epd_spi_mosi_low(epd->spi);

	// discharge pulse
	gpiod_set_value_cansleep(epd->discharge, 1);
	msleep(150);
	gpiod_set_value_cansleep(epd->discharge, 0);
}

static void EPD_set_temperature(struct repaper_epd *epd, int temperature)
{
	epd->factored_stage_time = epd->stage_time * temperature_to_factor_10x(temperature) / 10;
}

// clear display (anything -> white)
static void EPD_clear(struct repaper_epd *epd)
{
	frame_fixed_repeat(epd, 0xff, EPD_compensate);
	frame_fixed_repeat(epd, 0xff, EPD_white);
	frame_fixed_repeat(epd, 0xaa, EPD_inverse);
	frame_fixed_repeat(epd, 0xaa, EPD_normal);
}

// assuming a clear (white) screen output an image
static void EPD_image_0(struct repaper_epd *epd, const u8 *image)
{
	frame_fixed_repeat(epd, 0xaa, EPD_compensate);
	frame_fixed_repeat(epd, 0xaa, EPD_white);
	frame_data_repeat(epd, image, NULL, EPD_inverse);
	frame_data_repeat(epd, image, NULL, EPD_normal);
}

// change from old image to new image
static void EPD_image(struct repaper_epd *epd, const u8 *old_image, const u8 *new_image)
{
	frame_data_repeat(epd, old_image, NULL, EPD_compensate);
	frame_data_repeat(epd, old_image, NULL, EPD_white);
	frame_data_repeat(epd, new_image, NULL, EPD_inverse);
	frame_data_repeat(epd, new_image, NULL, EPD_normal);
}

// change from old image to new image
static void EPD_partial_image(struct repaper_epd *epd, const u8 *old_image, const u8 *new_image)
{

	frame_data_repeat(epd, old_image, new_image, EPD_compensate);
	frame_data_repeat(epd, old_image, new_image, EPD_white);
	frame_data_repeat(epd, new_image, old_image, EPD_inverse);
	frame_data_repeat(epd, new_image, old_image, EPD_normal);
}

static int temperature_to_factor_10x(int temperature)
{
	if (temperature <= -10) {
		return 170;
	} else if (temperature <= -5) {
		return 120;
	} else if (temperature <= 5) {
		return 80;
	} else if (temperature <= 10) {
		return 40;
	} else if (temperature <= 15) {
		return 30;
	} else if (temperature <= 20) {
		return 20;
	} else if (temperature <= 40) {
		return 10;
	}
	return 7;
}

/*
 * One frame of data is the number of lines * rows. For example:
 * The 1.44” frame of data is 96 lines * 128 dots.
 * The 2” frame of data is 96 lines * 200 dots.
 * The 2.7” frame of data is 176 lines * 264 dots.
 *
 * the image is arranged by line which matches the display size
 * so smallest would have 96 * 32 bytes
 */

static void frame_fixed(struct repaper_epd *epd, u8 fixed_value, EPD_stage stage)
{
	u8 l;

	for (l = 0; l < epd->lines_per_display ; ++l) {
		line(epd, l, NULL, fixed_value, NULL, stage);
	}
}

static void frame_data(struct repaper_epd *epd, const u8 *image, const u8 *mask, EPD_stage stage)
{
	u8 l;

	if (NULL == mask) {
		for (l = 0; l < epd->lines_per_display ; ++l) {
			line(epd, l, &image[l * epd->bytes_per_line], 0, NULL, stage);
		}
	} else {
		for (l = 0; l < epd->lines_per_display ; ++l) {
			size_t n = l * epd->bytes_per_line;

			line(epd, l, &image[n], 0, &mask[n], stage);
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


static void line(struct repaper_epd *epd, uint16_t line, const u8 *data, u8 fixed_value, const u8 *mask, EPD_stage stage)
{
	u8 *p = epd->line_buffer;
	u16 b;

	repaper_epd_spi_mosi_low(epd->spi);

	// charge pump voltage levels
	repaper_epd_write_buf(epd->spi, 0x04, epd->gate_source, epd->gate_source_length);

	// border byte only necessary for 1.44" EPD
	if (epd->dots_per_line == 128) {
		*p++ = 0x00;
	}

	// even pixels
	for (b = epd->bytes_per_line; b > 0; --b) {
		if (NULL != data) {
			u8 pixels = data[b - 1] & 0xaa;
			u8 pixel_mask = 0xff;

			if (NULL != mask) {
				pixel_mask = (mask[b - 1] ^ pixels) & 0xaa;
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
			*p++ = (pixels & pixel_mask) | (~pixel_mask & 0x55);
		} else {
			*p++ = fixed_value;
		}
	}

	// scan line
	for (b = 0; b < epd->bytes_per_scan; ++b) {
		if (line / 4 == b) {
			*p++ = 0xc0 >> (2 * (line & 0x03));
		} else {
			*p++ = 0x00;
		}
	}

	// odd pixels
	for (b = 0; b < epd->bytes_per_line; ++b) {
		if (NULL != data) {
			u8 pixels = data[b] & 0x55;
			u8 pixel_mask = 0xff;
			u8 p1, p2, p3, p4;

			if (NULL != mask) {
				pixel_mask = (mask[b] ^ pixels) & 0x55;
				pixel_mask |= pixel_mask << 1;
			}

			switch(stage) {
			case EPD_compensate:  // B -> W, W -> B (Current Image)
				pixels = 0xaa | (pixels ^ 0x55);
				break;
			case EPD_white:       // B -> N, W -> W (Current Image)
				pixels = 0x55 + (pixels ^ 0x55);
				break;
			case EPD_inverse:     // B -> N, W -> B (New Image)
				pixels = 0x55 | ((pixels ^ 0x55) << 1);
				break;
			case EPD_normal:       // B -> B, W -> W (New Image)
				pixels = 0xaa | pixels;
				break;
			}
			pixels = (pixels & pixel_mask) | (~pixel_mask & 0x55);

			p1 = (pixels >> 6) & 0x03;
			p2 = (pixels >> 4) & 0x03;
			p3 = (pixels >> 2) & 0x03;
			p4 = (pixels >> 0) & 0x03;
			pixels = (p1 << 0) | (p2 << 2) | (p3 << 4) | (p4 << 6);
			*p++ = pixels;
		} else {
			*p++ = fixed_value;
		}
	}

	if (epd->filler) {
		*p++ = 0x00;
	}

	// send the accumulated line buffer
	repaper_epd_write_buf(epd->spi, 0x0a, epd->line_buffer, p - epd->line_buffer);

	// output data to panel
	repaper_epd_write_val(epd->spi, 0x02, 0x2f);

	repaper_epd_spi_mosi_low(epd->spi);
}







// need to sync size with above (max of all sizes)
// this will be the next display
static char display_buffer[264 * 176 / 8] = { };

// this is the current display
static char current_buffer[sizeof(display_buffer)] = { };

// bit reversed table
static const char reverse[256] = {
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


// copy buffer
static void special_memcpy(char *d, const char *s, size_t size, bool bit_reversed, bool inverted)
{
	size_t n;

	if (bit_reversed) {
		if (inverted) {
			for (n = 0; n < size; ++n) {
				*d++ = reverse[(unsigned)(*s++)] ^ 0xff;
			}
		} else {
			for (n = 0; n < size; ++n) {
				*d++ = reverse[(unsigned)(*s++)];
			}
		}
	} else if (inverted) {
		for (n = 0; n < size; ++n) {
			*d++ = *s++ ^ 0xff;
		}
	} else {
		memcpy(d, s, size);
	}
}




static int repaper_v110g1_fb_dirty(struct drm_framebuffer *fb,
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

	special_memcpy(display_buffer, epd->buf, fb->width * fb->height / 8, true, false);

	/* FIXME: make it dynamic somehow */
	EPD_set_temperature(epd, 25);

	if (epd->cleared) {
		EPD_image(epd, (const u8 *)current_buffer, (const u8 *)display_buffer);
	} else {
		EPD_clear(epd);
		EPD_image_0(epd, (const u8 *)display_buffer);
		epd->cleared = true;
	}

	memcpy(current_buffer, display_buffer, sizeof(display_buffer));

	DRM_DEBUG("End Flushing [FB:%d]\n", fb->base.id);

out_unlock:
	mutex_unlock(&tdev->dirty_lock);

	if (ret)
		dev_err_once(fb->dev->dev, "Failed to update display %d\n",
			     ret);
	kfree(mono8);

	return ret;
}

const struct drm_framebuffer_funcs repaper_v110g1_fb_funcs = {
	.destroy	= drm_fb_cma_destroy,
	.create_handle	= drm_fb_cma_create_handle,
	.dirty		= repaper_v110g1_fb_dirty,
};

static void repaper_v110g1_pipe_enable(struct drm_simple_display_pipe *pipe,
				       struct drm_crtc_state *crtc_state)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct repaper_epd *epd = epd_from_tinydrm(tdev);

	DRM_DEBUG_DRIVER("Enable begin\n");

	EPD_begin(epd);
	epd->enabled = true;

	DRM_DEBUG_DRIVER("Enable end\n");
}

static void repaper_v110g1_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct repaper_epd *epd = epd_from_tinydrm(tdev);

	DRM_DEBUG_DRIVER("Disable begin\n");

	mutex_lock(&tdev->dirty_lock);
	epd->enabled = false;
	mutex_unlock(&tdev->dirty_lock);

	EPD_end(epd);

	DRM_DEBUG_DRIVER("Disable end\n");
}

const struct drm_simple_display_pipe_funcs repaper_v110g1_pipe_funcs = {
	.enable = repaper_v110g1_pipe_enable,
	.disable = repaper_v110g1_pipe_disable,
	.update = tinydrm_display_pipe_update,
	.prepare_fb = tinydrm_display_pipe_prepare_fb,
};
