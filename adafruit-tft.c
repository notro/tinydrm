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

#include <drm/tinydrm/ili9340.h>
#include <drm/tinydrm/lcdreg-spi.h>
#include <drm/tinydrm/mipi-dbi.h>
#include <drm/tinydrm/tinydrm.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <video/mipi_display.h>

enum adafruit_tft_displays {
	ADAFRUIT_1601 = 1601,
	ADAFRUIT_797 = 797,
	ADAFRUIT_358 = 358,
};

static u32 adafruit_tft_get_rotation(struct device *dev)
{
	u32 rotation = 0;

	device_property_read_u32(dev, "rotation", &rotation);

	return rotation;
}

static int adafruit_tft_1601_prepare(struct tinydrm_device *tdev)
{
	struct lcdreg *reg = tdev->lcdreg;
	u8 addr_mode;
	int ret;

	dev_dbg(tdev->base->dev, "%s\n", __func__);

	if (tdev->regulator) {
		ret = regulator_enable(tdev->regulator);
		if (ret) {
			dev_err(tdev->base->dev,
				"Failed to enable regulator %d\n", ret);
			return ret;
		}
	}

	mipi_dbi_debug_dump_regs(reg);

	/* Avoid flicker by skipping setup if the bootloader has done it */
	if (mipi_dbi_display_is_on(reg))
		return 0;

	lcdreg_reset(reg);
	ret = lcdreg_writereg(reg, MIPI_DCS_SOFT_RESET);
	if (ret) {
		dev_err(tdev->base->dev, "Error writing lcdreg %d\n", ret);
		return ret;
	}

	msleep(20);

	/* Undocumented registers */
	lcdreg_writereg(reg, 0xEF, 0x03, 0x80, 0x02);
	lcdreg_writereg(reg, 0xCF, 0x00, 0xC1, 0x30);
	lcdreg_writereg(reg, 0xED, 0x64, 0x03, 0x12, 0x81);
	lcdreg_writereg(reg, 0xE8, 0x85, 0x00, 0x78);
	lcdreg_writereg(reg, 0xCB, 0x39, 0x2C, 0x00, 0x34, 0x02);
	lcdreg_writereg(reg, 0xF7, 0x20);
	lcdreg_writereg(reg, 0xEA, 0x00, 0x00);

	lcdreg_writereg(reg, ILI9340_PWCTRL1, 0x23);
	lcdreg_writereg(reg, ILI9340_PWCTRL2, 0x10);
	lcdreg_writereg(reg, ILI9340_VMCTRL1, 0x3e, 0x28);
	lcdreg_writereg(reg, ILI9340_VMCTRL2, 0x86);

	lcdreg_writereg(reg, MIPI_DCS_SET_PIXEL_FORMAT, 0x55);
	lcdreg_writereg(reg, ILI9340_FRMCTR1, 0x00, 0x18);
	lcdreg_writereg(reg, ILI9340_DISCTRL, 0x08, 0x82, 0x27);

	/* 3Gamma Function Disable */
	lcdreg_writereg(reg, 0xF2, 0x00);

	lcdreg_writereg(reg, MIPI_DCS_SET_GAMMA_CURVE, 0x01);
	lcdreg_writereg(reg, ILI9340_PGAMCTRL,
			0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
			0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00);
	lcdreg_writereg(reg, ILI9340_NGAMCTRL,
			0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
			0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F);

	switch (adafruit_tft_get_rotation(reg->dev)) {
	default:
		addr_mode = ILI9340_MADCTL_MV | ILI9340_MADCTL_MY |
			    ILI9340_MADCTL_MX;
		break;
	case 90:
		addr_mode = ILI9340_MADCTL_MY;
		break;
	case 180:
		addr_mode = ILI9340_MADCTL_MV;
		break;
	case 270:
		addr_mode = ILI9340_MADCTL_MX;
		break;
	}
	addr_mode |= ILI9340_MADCTL_BGR;
	lcdreg_writereg(reg, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);

	lcdreg_writereg(reg, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(120);
	lcdreg_writereg(reg, MIPI_DCS_SET_DISPLAY_ON);

	mipi_dbi_debug_dump_regs(reg);

	return 0;
}

static const struct tinydrm_funcs adafruit_tft_1601_funcs = {
	.prepare = adafruit_tft_1601_prepare,
	.unprepare = mipi_dbi_unprepare,
	.enable = tinydrm_enable_backlight,
	.disable = tinydrm_disable_backlight,
	.dirty = mipi_dbi_dirty,
};

static const struct of_device_id adafruit_tft_of_match[] = {
	{ .compatible = "adafruit,tft1601", .data = (void *)ADAFRUIT_1601 },
	{ .compatible = "adafruit,tft797",  .data = (void *)ADAFRUIT_797 },
	{ .compatible = "adafruit,tft358",  .data = (void *)ADAFRUIT_358 },
	{},
};
MODULE_DEVICE_TABLE(of, adafruit_tft_of_match);

static const struct spi_device_id adafruit_tft_id[] = {
	{ "tft1601", ADAFRUIT_1601 },
	{ "tft797",  ADAFRUIT_797 },
	{ "tft358",  ADAFRUIT_358 },
	{ },
};
MODULE_DEVICE_TABLE(spi, adafruit_tft_id);

TINYDRM_DRM_DRIVER(adafruit_tft, "adafruit-tft", "Adafruit TFT", "20160317");

static int adafruit_tft_probe(struct spi_device *spi)
{
	const struct of_device_id *of_id;
	struct lcdreg_spi_config cfg = {
		.mode = LCDREG_SPI_4WIRE,
	};
	struct device *dev = &spi->dev;
	struct tinydrm_device *tdev;
	bool readable = false;
	struct lcdreg *reg;
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

	tdev = devm_kzalloc(dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;

	tdev->backlight = tinydrm_of_find_backlight(dev);
	if (IS_ERR(tdev->backlight))
		return PTR_ERR(tdev->backlight);

	tdev->regulator = devm_regulator_get_optional(dev, "power");
	if (IS_ERR(tdev->regulator)) {
		if (PTR_ERR(tdev->regulator) != -ENODEV)
			return PTR_ERR(tdev->regulator);
		tdev->regulator = NULL;
	}

	switch (id) {
	case ADAFRUIT_1601:
		readable = true;
		cfg.mode = LCDREG_SPI_4WIRE;
		tdev->width = 320;
		tdev->height = 240;
		tdev->width_mm = 58;
		tdev->height_mm = 43;
		tdev->funcs = &adafruit_tft_1601_funcs;
		break;
	case ADAFRUIT_797:
		cfg.mode = LCDREG_SPI_3WIRE;
		tdev->width = 176;
		tdev->height = 220;
		/* TODO: tdev->funcs = &adafruit_tft_797_funcs*/
		break;
	case ADAFRUIT_358:
		tdev->width = 128;
		tdev->height = 160;
		/* TODO: tdev->funcs = &adafruit_tft_358_funcs */
		break;
	default:
		return -EINVAL;
	}

	DRM_DEBUG_DRIVER("rotation = %u\n", adafruit_tft_get_rotation(dev));
	switch (adafruit_tft_get_rotation(dev)) {
	case 90:
	case 270:
		swap(tdev->width, tdev->height);
		break;
	}

	reg = devm_lcdreg_spi_init(spi, &cfg);
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	reg->readable = readable;
	tdev->lcdreg = reg;
	ret = mipi_dbi_init(dev, tdev);
	if (ret)
		return ret;

	/* TODO: Make configurable */
	tdev->fbdefio_delay_ms = 40;

	ret = devm_tinydrm_init(dev, tdev, &adafruit_tft);
	if (ret)
		return ret;

	ret = tinydrm_modeset_init(tdev);
	if (ret)
		return ret;

	ret = devm_tinydrm_register(tdev);
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
