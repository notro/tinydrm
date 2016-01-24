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

#include <drm/tinydrm/ili9340.h>
#include <drm/tinydrm/lcdreg.h>
#include <drm/tinydrm/mipi-dbi.h>
#include <drm/tinydrm/tinydrm.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/spi/spi.h>
#include <video/mipi_display.h>

static int ada_mipi_panel_disable(struct drm_panel *panel)
{
	struct tinydrm_device *tdev = tinydrm_from_panel(panel);

	dev_dbg(tdev->base->dev, "%s\n", __func__);

	return 0;
}

static int ada_mipi_1601_panel_unprepare(struct drm_panel *panel)
{
	struct tinydrm_device *tdev = tinydrm_from_panel(panel);

	dev_dbg(tdev->base->dev, "%s\n", __func__);

	return 0;
}

static int ada_mipi_1601_panel_prepare(struct drm_panel *panel)
{
	struct tinydrm_device *tdev = tinydrm_from_panel(panel);
	struct lcdreg *reg = tdev->lcdreg;
	int ret;

	dev_dbg(tdev->base->dev, "%s\n", __func__);

	lcdreg_reset(reg);
	ret = lcdreg_writereg(reg, ILI9340_SWRESET);
	if (ret) {
		dev_err(reg->dev, "lcdreg_writereg failed: %d\n", ret);
		return ret;
	}

	msleep(20);
//	mipi_dbi_check(reg, 0);

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

	lcdreg_writereg(reg, ILI9340_PIXSET, 0x55);
	lcdreg_writereg(reg, ILI9340_FRMCTR1, 0x00, 0x18);
	lcdreg_writereg(reg, ILI9340_DISCTRL, 0x08, 0x82, 0x27);

	/* 3Gamma Function Disable */
	lcdreg_writereg(reg, 0xF2, 0x00);

	lcdreg_writereg(reg, ILI9340_GAMSET, 0x01);
	lcdreg_writereg(reg, ILI9340_PGAMCTRL,
			0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
			0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00);
	lcdreg_writereg(reg, ILI9340_NGAMCTRL,
			0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
			0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F);

	lcdreg_writereg(reg, ILI9340_SLPOUT);
	msleep(120);
//	mipi_dbi_check(reg, 1);

	lcdreg_writereg(reg, ILI9340_DISPON);

lcdreg_writereg(reg, MIPI_DCS_SET_ADDRESS_MODE, ILI9340_MADCTL_MX | (1 << 3));

	return 0;
}

static int ada_mipi_panel_enable(struct drm_panel *panel)
{
	struct tinydrm_device *tdev = tinydrm_from_panel(panel);

	dev_dbg(tdev->base->dev, "%s\n", __func__);

	return 0;
}

struct drm_panel_funcs ada_mipi_1601_drm_panel_funcs = {
	.disable = ada_mipi_panel_disable,
	.unprepare = ada_mipi_1601_panel_unprepare,
	.prepare = ada_mipi_1601_panel_prepare,
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
	const struct of_device_id *of_id;
	struct device *dev = &spi->dev;
	struct tinydrm_device *tdev;
	enum lcdreg_spi_mode mode;
	bool readable = false;
	struct lcdreg *reg;
	int ret;

	of_id = of_match_device(ada_mipi_ids, dev);
	if (!of_id)
		return -EINVAL;

	tdev = devm_kzalloc(dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;

	tdev->panel.funcs = &ada_mipi_1601_drm_panel_funcs;
	tdev->update = mipi_dbi_update;
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
		readable = true;
		mode = LCDREG_SPI_4WIRE;
		tdev->width = 240;
		tdev->height = 320;
		break;
	default:
		return -EINVAL;
	}

	reg = devm_lcdreg_spi_init_of(spi, mode);
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	reg->readable = readable;
reg->def_width = 8;
	tdev->lcdreg = reg;

	ret = mipi_dbi_check(reg);
	if (ret)
		dev_warn(dev, "mipi_dbi_check failed: %d\n", ret);

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
