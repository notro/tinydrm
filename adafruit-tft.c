#define DEBUG

/*
 * DRM driver for Adafruit MIPI compatible SPI TFT displays
 *
 * Copyright 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/tinydrm/hx8340.h>
#include <drm/tinydrm/lcdreg-spi.h>
#include <drm/tinydrm/mipi-dbi.h>
#include <drm/tinydrm/st7735r.h>
#include <drm/tinydrm/tinydrm.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <video/mipi_display.h>

struct adafruit_tft_display {
	const struct tinydrm_funcs funcs;
	const struct drm_display_mode mode;
	enum lcdreg_spi_mode spi_mode;
};

enum adafruit_tft_display_ids {
	ADAFRUIT_797,
	ADAFRUIT_358,
};

/* Init sequence taken from the Adafruit-HX8340B library */
static int adafruit_tft_797_prepare(struct tinydrm_device *tdev)
{
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);
	struct lcdreg *reg = mipi->reg;
	u8 addr_mode;
	int ret;

	dev_dbg(tdev->base->dev, "%s\n", __func__);

	if (mipi->prepared_once)
		return 0;

	if (mipi->regulator) {
		ret = regulator_enable(mipi->regulator);
		if (ret) {
			dev_err(tdev->base->dev,
				"Failed to enable regulator %d\n", ret);
			return ret;
		}
	}

	lcdreg_reset(reg);
	ret = lcdreg_writereg(reg, HX8340_SETEXTCMD, 0xFF, 0x83, 0x40);
	if (ret) {
		dev_err(reg->dev, "lcdreg_writereg failed: %d\n", ret);
		return ret;
	}

	lcdreg_writereg(reg, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(150);

	/* Undocumented register */
	lcdreg_writereg(reg, 0xCA, 0x70, 0x00, 0xD9);

	lcdreg_writereg(reg, HX8340_SETOSC, 0x01, 0x11);

	/* Undocumented register */
	lcdreg_writereg(reg, 0xC9, 0x90, 0x49, 0x10, 0x28,
			           0x28, 0x10, 0x00, 0x06);
	msleep(20);

	lcdreg_writereg(reg, HX8340_SETGAMMAP, 0x60, 0x71, 0x01, 0x0E, 0x05,
					       0x02, 0x09, 0x31, 0x0A);
	lcdreg_writereg(reg, HX8340_SETGAMMAN, 0x67, 0x30, 0x61, 0x17, 0x48,
					       0x07, 0x05, 0x33);
	msleep(10);

	lcdreg_writereg(reg, HX8340_SETPWCTR5, 0x35, 0x20, 0x45);
	lcdreg_writereg(reg, HX8340_SETPWCTR4, 0x33, 0x25, 0x4C);
	msleep(10);

	lcdreg_writereg(reg, MIPI_DCS_SET_PIXEL_FORMAT, 0x05);

	switch (mipi->rotation) {
	default:
		addr_mode = 0;
		break;
	case 90:
		addr_mode = HX8340_MADCTL_MV | HX8340_MADCTL_MY;
		break;
	case 180:
		addr_mode = HX8340_MADCTL_MY;
		break;
	case 270:
		addr_mode = HX8340_MADCTL_MX | HX8340_MADCTL_MV;
		break;
	}
	addr_mode |= HX8340_MADCTL_BGR;
	lcdreg_writereg(reg, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);

	lcdreg_writereg(reg, MIPI_DCS_SET_DISPLAY_ON);
	msleep(50);

	return 0;
}

/* Init sequence taken from the Adafruit-ST7735-Library (Black Tab) */
static int adafruit_tft_358_prepare(struct tinydrm_device *tdev)
{
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);
	struct lcdreg *reg = mipi->reg;
	u8 addr_mode;
	int ret;

	dev_dbg(tdev->base->dev, "%s\n", __func__);

	if (mipi->prepared_once)
		return 0;

	if (mipi->regulator) {
		ret = regulator_enable(mipi->regulator);
		if (ret) {
			dev_err(tdev->base->dev,
				"Failed to enable regulator %d\n", ret);
			return ret;
		}
	}

	lcdreg_reset(reg);
	ret = lcdreg_writereg(reg, MIPI_DCS_SOFT_RESET);
	if (ret) {
		dev_err(tdev->base->dev, "Error writing lcdreg %d\n", ret);
		return ret;
	}

	msleep(150);

	lcdreg_writereg(reg, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(500);

	lcdreg_writereg(reg, ST7735R_FRMCTR1, 0x01, 0x2C, 0x2D);
	lcdreg_writereg(reg, ST7735R_FRMCTR2, 0x01, 0x2C, 0x2D);
	lcdreg_writereg(reg, ST7735R_FRMCTR3,
			0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D);
	lcdreg_writereg(reg, ST7735R_INVCTR, 0x07);

	lcdreg_writereg(reg, ST7735R_PWCTR1, 0xA2, 0x02, 0x84);
	lcdreg_writereg(reg, ST7735R_PWCTR2, 0xC5);
	lcdreg_writereg(reg, ST7735R_PWCTR3, 0x0A, 0x00);
	lcdreg_writereg(reg, ST7735R_PWCTR4, 0x8A, 0x2A);
	lcdreg_writereg(reg, ST7735R_PWCTR5, 0x8A, 0xEE);

	lcdreg_writereg(reg, ST7735R_VMCTR1, 0x0E);
	lcdreg_writereg(reg, MIPI_DCS_EXIT_INVERT_MODE);

	switch (mipi->rotation) {
	default:
		addr_mode = ST7735R_MADCTL_MX | ST7735R_MADCTL_MY;
		break;
	case 90:
		addr_mode = ST7735R_MADCTL_MX | ST7735R_MADCTL_MV;
		break;
	case 180:
		addr_mode = 0;
		break;
	case 270:
		addr_mode = ST7735R_MADCTL_MY | ST7735R_MADCTL_MV;
		break;
	}
	lcdreg_writereg(reg, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);

	lcdreg_writereg(reg, MIPI_DCS_SET_PIXEL_FORMAT, 0x05);

	lcdreg_writereg(reg, ST7735R_GAMCTRP1,
			0x02, 0x1c, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2d,
			0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10);
	lcdreg_writereg(reg, ST7735R_GAMCTRN1,
			0x03, 0x1d, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
			0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10);

	lcdreg_writereg(reg, MIPI_DCS_ENTER_NORMAL_MODE);
	msleep(20);

	lcdreg_writereg(reg, MIPI_DCS_SET_DISPLAY_ON);
	msleep(100);

	return 0;
}

static const struct adafruit_tft_display adafruit_tft_displays[] = {
	/* 2.2" Color TFT LCD display - HX8340BN, 9-bit mode (#797) */
	[ADAFRUIT_797] = {
		.mode = {
			TINYDRM_MODE(176, 220, 34, 43),
		},
		.funcs = {
			.prepare = adafruit_tft_797_prepare,
			.unprepare = mipi_dbi_unprepare,
			.enable = mipi_dbi_enable_backlight,
			.disable = mipi_dbi_disable_backlight,
			.dirty = mipi_dbi_dirty,
		},
		.spi_mode = LCDREG_SPI_3WIRE,
	},
	/* 1.8" Color TFT LCD display - ST7735R (#358) */
	[ADAFRUIT_358] = {
		.mode = {
			TINYDRM_MODE(128, 160, 28, 35),
		},
		.funcs = {
			.prepare = adafruit_tft_358_prepare,
			.unprepare = mipi_dbi_unprepare,
			.enable = mipi_dbi_enable_backlight,
			.disable = mipi_dbi_disable_backlight,
			.dirty = mipi_dbi_dirty,
		},
		.spi_mode = LCDREG_SPI_4WIRE,
	},
};

static const struct of_device_id adafruit_tft_of_match[] = {
	{ .compatible = "adafruit,tft797",  .data = (void *)ADAFRUIT_797 },
	{ .compatible = "adafruit,tft358",  .data = (void *)ADAFRUIT_358 },
	{},
};
MODULE_DEVICE_TABLE(of, adafruit_tft_of_match);

static const struct spi_device_id adafruit_tft_id[] = {
	{ "tft797",  ADAFRUIT_797 },
	{ "tft358",  ADAFRUIT_358 },
	{ },
};
MODULE_DEVICE_TABLE(spi, adafruit_tft_id);

TINYDRM_DRM_DRIVER(adafruit_tft_driver, "adafruit-tft", "Adafruit TFT",
		   "20160317");

static int adafruit_tft_probe(struct spi_device *spi)
{
	struct lcdreg_spi_config cfg = { 0, };
	const struct of_device_id *of_id;
	struct device *dev = &spi->dev;
	struct tinydrm_device *tdev;
	struct mipi_dbi *mipi;
	struct lcdreg *reg;
	u32 rotation = 0;
	int id, ret;

	of_id = of_match_device(adafruit_tft_of_match, dev);
	if (of_id) {
		id = (int)of_id->data;
	} else {
		const struct spi_device_id *spi_id = spi_get_device_id(spi);

		if (!spi_id)
			return -EINVAL;

		id = spi_id->driver_data;
	}

	if (!dev->coherent_dma_mask) {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret)
			dev_warn(dev, "Failed to set dma mask %d\n", ret);
	}

	mipi = devm_kzalloc(dev, sizeof(*mipi), GFP_KERNEL);
	if (!mipi)
		return -ENOMEM;

	tdev = &mipi->tinydrm;

	mipi->backlight = tinydrm_of_find_backlight(dev);
	if (IS_ERR(mipi->backlight))
		return PTR_ERR(mipi->backlight);

	mipi->regulator = devm_regulator_get_optional(dev, "power");
	if (IS_ERR(mipi->regulator)) {
		ret = PTR_ERR(mipi->regulator);
		if (ret != -ENODEV)
			return ret;

		mipi->regulator = NULL;
	}

	cfg.mode = adafruit_tft_displays[id].spi_mode;
	cfg.readable = device_property_read_bool(dev, "readable");
	reg = devm_lcdreg_spi_init(spi, &cfg);
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	device_property_read_u32(dev, "rotation", &rotation);

	ret = mipi_dbi_init(dev, mipi, reg, &adafruit_tft_driver,
			    &adafruit_tft_displays[id].mode, rotation);
	if (ret)
		return ret;

	ret = devm_tinydrm_register(tdev, &adafruit_tft_displays[id].funcs);
	if (ret)
		return ret;

	spi_set_drvdata(spi, tdev);

	DRM_DEBUG_DRIVER("Initialized %s:%s on minor %d\n",
			 tdev->base->driver->name, dev_name(dev),
			 tdev->base->primary->index);

	return 0;
}

static struct spi_driver adafruit_tft_spi_driver = {
	.driver = {
		.name = "adafruit-tft",
		.owner = THIS_MODULE,
		.of_match_table = adafruit_tft_of_match,
		.pm = &tinydrm_simple_pm_ops,
	},
	.id_table = adafruit_tft_id,
	.probe = adafruit_tft_probe,
	.shutdown = tinydrm_spi_shutdown,
};
module_spi_driver(adafruit_tft_spi_driver);

MODULE_DESCRIPTION("Adafruit MIPI compatible SPI displays");
MODULE_AUTHOR("Noralf Trønnes");
MODULE_LICENSE("GPL");
