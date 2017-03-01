/*
 * Copyright (C) 2017 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include <asm/unaligned.h>

#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/tinydrm/tinydrm-ili9325.h>


int tinydrm_ili9325_flush(struct tinydrm_panel *panel,
			  struct drm_framebuffer *fb,
			  struct drm_clip_rect *rect)
{
	u16 ac_low, ac_high;
	void *tr;

	rect->x1 = 0;
	rect->x2 = fb->width;

	tr = tinydrm_panel_rgb565_buf(panel, fb, rect);
	if (IS_ERR(tr))
		return PTR_ERR(tr);

	/*
	 * FIXME
	 * This should support clips that are not full width,
	 * and it looks like only 240x320 panels are supported.
	 */
#define WIDTH 240
#define HEIGHT 320

	switch (panel->rotation) {
	case 0:
		ac_low = 0;
		ac_high = rect->y1;
		break;
	case 180:
		ac_low = WIDTH - 1 - 0;
		ac_high = HEIGHT - 1 - rect->y1;
		break;
	case 270:
		ac_low = WIDTH - 1 - rect->y1;
		ac_high = 0;
		break;
	case 90:
		ac_low = rect->y1;
		ac_high = HEIGHT - 1 - 0;
		break;
	};

	regmap_write(panel->reg, 0x0020, ac_low);
	regmap_write(panel->reg, 0x0021, ac_high);

	return regmap_raw_write(panel->reg, 0x0022, tr,
			       (rect->x2 - rect->x1) * (rect->y2 - rect->y1) * 2);
}
EXPORT_SYMBOL(tinydrm_ili9325_flush);

static const uint32_t tinydrm_ili9325_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

/**
 * tinydrm_ili9325_init - Initialize &tinydrm_simple for use with ili9325
 * @dev: Parent device
 * @panel: &tinydrm_panel structure to initialize
 * @funcs: Callbacks for the panel (optional)
 * @reg: Register map
 * @driver: DRM driver
 * @mode: Display mode
 * @rotation: Initial rotation in degrees Counter Clock Wise
 *
 * This function initializes a &tinydrm_panel structure and it's underlying
 * @tinydrm_device. It also sets up the display pipeline.
 *
 * Supported formats: Native RGB565 and emulated XRGB8888.
 *
 * Objects created by this function will be automatically freed on driver
 * detach (devres).
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int tinydrm_ili9325_init(struct device *dev, struct tinydrm_panel *panel,
			 const struct tinydrm_panel_funcs *funcs,
			 struct regmap *reg, struct drm_driver *driver,
			 const struct drm_display_mode *mode,
			 unsigned int rotation)
{
	panel->reg = reg;
	panel->swap_bytes = tinydrm_regmap_raw_swap_bytes(reg);

	return tinydrm_panel_init(dev, panel, funcs, tinydrm_ili9325_formats,
				  ARRAY_SIZE(tinydrm_ili9325_formats),
				  driver, mode, rotation);
}
EXPORT_SYMBOL(tinydrm_ili9325_init);

#if IS_ENABLED(CONFIG_SPI)

struct tinydrm_ili9325_spi {
	struct spi_device *spi;
	struct regmap *reg;
	unsigned int bpw;
	unsigned int id;
};

/* Startbyte: | 0 | 1 | 1 | 1 | 0 | ID | RS | RW | */
static u8 tinydrm_ili9325_spi_get_startbyte(bool id, bool rs, bool read)
{
	return 0x70 | (id << 2) | (rs << 1) | read;
}

static int tinydrm_ili9325_spi_gather_write(void *context, const void *reg,
					    size_t reg_len, const void *val,
					    size_t val_len)
{
	struct tinydrm_ili9325_spi *spih = context;
	/* For reliability only run pixel data above spec */
	u32 norm_speed_hz = min_t(u32, 10000000, spih->spi->max_speed_hz);
	struct spi_transfer header = {
		.speed_hz = norm_speed_hz,
		.bits_per_word = 8,
		.len = 1,
	};
	u8 *startbyte;
	int ret;

	startbyte = kmalloc(1, GFP_KERNEL);
	if (!startbyte)
		return -ENOMEM;

	header.tx_buf = startbyte;
	*startbyte = tinydrm_ili9325_spi_get_startbyte(spih->id, 0, false);
	ret = tinydrm_spi_transfer(spih->spi, norm_speed_hz, &header,
				   spih->bpw, reg, reg_len);
	if (ret)
		goto err_free;

	*startbyte = tinydrm_ili9325_spi_get_startbyte(spih->id, 1, false);
	ret = tinydrm_spi_transfer(spih->spi, val_len > 64 ? 0 : norm_speed_hz,
				   &header, spih->bpw, val, val_len);

err_free:
	kfree(startbyte);

	return ret;
}

static int tinydrm_ili9325_spi_write(void *context, const void *data,
				     size_t count)
{
	struct tinydrm_ili9325_spi *spih = context;
	size_t sz = regmap_get_val_bytes(spih->reg);

	return tinydrm_ili9325_spi_gather_write(context, data, sz,
						data + sz, count - sz);
}

static int tinydrm_ili9325_spi_read(void *context, const void *reg,
				    size_t reg_len, void *val, size_t val_len)
{
	struct tinydrm_ili9325_spi *spih = context;
	struct spi_device *spi = spih->spi;
	u32 speed_hz = min_t(u32, 5000000, spi->max_speed_hz / 2);
	struct spi_transfer header = {
		.speed_hz = speed_hz,
		.bits_per_word = 8,
		.len = 1,
	};
	struct spi_transfer trrx = {
		.speed_hz = speed_hz,
		.bits_per_word = 8,
		.len = 3, /* including dummy byte */
	};
	struct spi_message m;
	u8 *startbyte;
	int ret;

	if (WARN_ON_ONCE(val_len != 2))
		return -EINVAL;

	startbyte = kmalloc(1, GFP_KERNEL);
	if (!startbyte)
		return -ENOMEM;

	trrx.rx_buf = kzalloc(trrx.len, GFP_KERNEL);
	if (!trrx.rx_buf) {
		ret = -ENOMEM;
		goto err_free;
	}

	header.tx_buf = startbyte;
	*startbyte = tinydrm_ili9325_spi_get_startbyte(spih->id, 0, false);
	ret = tinydrm_spi_transfer(spi, speed_hz, &header, spih->bpw,
				   reg, reg_len);
	if (ret)
		goto err_free;

	//tinydrm_ili9325_spi_set_header(spih, headerbuf, 1, true);
	*startbyte = tinydrm_ili9325_spi_get_startbyte(spih->id, 1, true);
	spi_message_init(&m);
	spi_message_add_tail(&header, &m);
	spi_message_add_tail(&trrx, &m);
	ret = spi_sync(spi, &m);
	if (ret)
		goto err_free;

	/* throw away dummy byte */
	if (tinydrm_regmap_raw_swap_bytes(spih->reg))
		*((u16 *)val) = get_unaligned_le16(trrx.rx_buf + 1);
	else
		*((u16 *)val) = get_unaligned_be16(trrx.rx_buf + 1);

err_free:
	kfree(startbyte);
	kfree(trrx.rx_buf);

	return ret;
}

static const struct regmap_bus tinydrm_ili9325_spi_bus = {
	.write = tinydrm_ili9325_spi_write,
	.gather_write = tinydrm_ili9325_spi_gather_write,
	.read = tinydrm_ili9325_spi_read,
	.reg_format_endian_default = REGMAP_ENDIAN_NATIVE,
	.val_format_endian_default = REGMAP_ENDIAN_NATIVE,
};

struct regmap *tinydrm_ili9325_spi_init(struct spi_device *spi,
					unsigned int id)
{
	struct tinydrm_ili9325_spi *spih;
	struct device *dev = &spi->dev;
	struct regmap_config config = {
		.reg_bits = 16,
		.val_bits = 16,
		.max_register = 0xff,
		.cache_type = REGCACHE_NONE,
	};

	spih = devm_kzalloc(dev, sizeof(*spih), GFP_KERNEL);
	if (!spih)
		return ERR_PTR(-ENOMEM);

	spih->spi = spi;
	spih->bpw = 16;
	spih->id = id;

	if (!tinydrm_spi_bpw_supported(spi, 16)) {
		config.reg_format_endian = REGMAP_ENDIAN_BIG,
		config.val_format_endian = REGMAP_ENDIAN_BIG,
		spih->bpw = 8;
	}

	spih->reg = devm_regmap_init(dev, &tinydrm_ili9325_spi_bus, spih,
				     &config);

	return spih->reg;
}
EXPORT_SYMBOL(tinydrm_ili9325_spi_init);

#endif
