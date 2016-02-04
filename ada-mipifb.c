#define DEBUG

/*
 * DRM driver for Adafruit MIPI compatible SPI displays
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
#include <linux/spi/spi.h>
#include <video/mipi_display.h>

static u32 ada_mipi_get_rotation(struct device *dev)
{
	u32 rotation = 0;

	device_property_read_u32(dev, "rotation", &rotation);

	return rotation;
}

static int ada_mipi_1601_panel_prepare(struct drm_panel *panel)
{
	struct tinydrm_device *tdev = tinydrm_from_panel(panel);
	struct lcdreg *reg = tdev->lcdreg;
	u8 addr_mode;

	dev_dbg(tdev->base->dev, "%s\n", __func__);

	mipi_dbi_debug_dump_regs(reg);

	/* Avoid flicker by skipping setup if the bootloader has done it */
	if (mipi_dbi_display_is_on(reg))
		return 0;

	lcdreg_reset(reg);
	lcdreg_writereg(reg, ILI9340_SWRESET);
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

	switch (ada_mipi_get_rotation(reg->dev)) {
		default:
			addr_mode = ILI9340_MADCTL_MV | ILI9340_MADCTL_MY |
				    ILI9340_MADCTL_MX;
			break;
		case 90:
			addr_mode = ILI9340_MADCTL_MY;;
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

	lcdreg_writereg(reg, ILI9340_SLPOUT);
	msleep(120);
	lcdreg_writereg(reg, ILI9340_DISPON);

	mipi_dbi_debug_dump_regs(reg);

	return 0;
}

static int ada_mipi_1601_panel_unprepare(struct drm_panel *panel)
{
	struct tinydrm_device *tdev = tinydrm_from_panel(panel);
	struct lcdreg *reg = tdev->lcdreg;

	dev_dbg(tdev->base->dev, "%s\n", __func__);

	/*
	 * Only do this if we have turned off backlight because if it's on the
	 * display will be all white when the pixels are turned off.
	 */
	if (tdev->backlight) {
		lcdreg_writereg(reg, ILI9340_DISPOFF);
		lcdreg_writereg(reg, ILI9340_SLPIN);
	}

	return 0;
}

static int ada_mipi_panel_enable(struct drm_panel *panel)
{
	struct tinydrm_device *tdev = tinydrm_from_panel(panel);

	dev_dbg(tdev->base->dev, "%s\n", __func__);

	if (tdev->backlight) {
		tdev->backlight->props.state &= ~BL_CORE_SUSPENDED;
		backlight_update_status(tdev->backlight);
	}

	return 0;
}

static int ada_mipi_panel_disable(struct drm_panel *panel)
{
	struct tinydrm_device *tdev = tinydrm_from_panel(panel);

	dev_dbg(tdev->base->dev, "%s\n", __func__);

	if (tdev->backlight) {
		tdev->backlight->props.state |= BL_CORE_SUSPENDED;
		backlight_update_status(tdev->backlight);
	}

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

static const struct of_device_id ada_mipi_of_match[] = {
	{ .compatible = "adafruit,ada358",  .data = (void *)ADAFRUIT_358 },
	{ .compatible = "adafruit,ada797",  .data = (void *)ADAFRUIT_797 },
	{ .compatible = "adafruit,ada1480", .data = (void *)ADAFRUIT_1480 },
	{ .compatible = "adafruit,ada1601", .data = (void *)ADAFRUIT_1601 },
{ .compatible = "sainsmart18", .data = (void *)ADAFRUIT_358 },
	{},
};
MODULE_DEVICE_TABLE(of, ada_mipi_of_match);

static const struct spi_device_id ada_mipi_id[] = {
	{ "ada358",  ADAFRUIT_358 },
	{ "ada797",  ADAFRUIT_797 },
	{ "ada1480", ADAFRUIT_1480 },
	{ "ada1601", ADAFRUIT_1601 },
        { }
};
MODULE_DEVICE_TABLE(spi, ada_mipi_id);

static int ada_mipi_probe(struct spi_device *spi)
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

	of_id = of_match_device(ada_mipi_of_match, dev);
	if (of_id) {
		id = (int)of_id->data;
	} else {
		const struct spi_device_id *spi_id = spi_get_device_id(spi);

		if (!spi_id)
			return -EINVAL;

		id = spi_id->driver_data;
	}

	tdev = devm_kzalloc(dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;

	tdev->backlight = tinydrm_of_find_backlight(dev);
	if (IS_ERR(tdev->backlight))
		return PTR_ERR(tdev->backlight);

	tdev->panel.funcs = &ada_mipi_1601_drm_panel_funcs;
	tdev->update = mipi_dbi_update;

	/* TODO: Make configurable */
	tdev->dirty.defer_ms = 40;

	switch (id) {
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
		cfg.mode = LCDREG_SPI_4WIRE;
		tdev->width = 320;
		tdev->height = 240;
		break;
	default:
		return -EINVAL;
	}

	DRM_DEBUG_DRIVER("rotation = %u\n", ada_mipi_get_rotation(dev));
	switch (ada_mipi_get_rotation(dev)) {
		case 90:
		case 270:
			swap(tdev->width, tdev->height);
			break;
	}

	reg = devm_lcdreg_spi_init(spi, &cfg);
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	reg->readable = readable;
	reg->def_width = MIPI_DBI_DEFAULT_REGWIDTH;
	tdev->lcdreg = reg;

	/* Make sure we at least can write */
	ret = lcdreg_writereg(reg, MIPI_DCS_NOP);
	if (ret) {
		dev_err(dev, "Error writing lcdreg\n");
		return ret;
	}

	spi_set_drvdata(spi, tdev);

	return devm_tinydrm_register(dev, tdev);
}

static void ada_mipi_shutdown(struct spi_device *spi)
{
	struct tinydrm_device *tdev = spi_get_drvdata(spi);

	dev_dbg(tdev->base->dev, "%s\n", __func__);

	drm_panel_disable(&tdev->panel);
	drm_panel_unprepare(&tdev->panel);
}

static struct spi_driver ada_mipi_spi_driver = {
	.driver = {
		.name = "ada-mipifb",
		.owner = THIS_MODULE,
		.of_match_table = ada_mipi_of_match,
	},
	.id_table = ada_mipi_id,
	.probe = ada_mipi_probe,
	.shutdown = ada_mipi_shutdown,
};
module_spi_driver(ada_mipi_spi_driver);

//MODULE_ALIAS("spi:ada358");
//MODULE_ALIAS("spi:ada797");
//MODULE_ALIAS("spi:ada1480");
//MODULE_ALIAS("spi:ada1601");


MODULE_DESCRIPTION("Adafruit MIPI compatible SPI displays");
MODULE_AUTHOR("Noralf Trønnes");
MODULE_LICENSE("GPL");
