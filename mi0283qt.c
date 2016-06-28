#define DEBUG

/*
 * DRM driver for Mulri-Inno MI0283QT panels
 *
 * Copyright 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/* TODO

of: Add vendor prefix for Multi-Inno
Multi-Inno Technology Co.,Ltd is a Hong Kong based company offering
LCD, LCD module products and complete panel solutions.

Documentation/devicetree/bindings/vendor-prefixes.txt
-------------------------------------------------------------------------------
multi-inno	Multi-Inno Technology Co.,Ltd
-------------------------------------------------------------------------------

dt-bindings: Add Multi-Inno MI0283QT binding
Add device-tree binding documentation for the MI0283QT display panel.

Documentation/devicetree/bindings/display/multi-inno,mi0283qt.txt
-------------------------------------------------------------------------------
Multi-Inno MI0283QT display panel

Required properties:
- compatible:	"multi-inno,mi0283qt".

The node for this driver must be a child node of a SPI controller, hence
all mandatory properties described in
	Documentation/devicetree/bindings/spi/spi-bus.txt
must be specified.

Optional properties:
- power-supply:	A regulator node for the supply voltage.
- reset-gpios:	Reset pin
- dc-gpios:	D/C pin. The presence/absence of this GPIO determines
		the panel interface mode (IM[3:0] pins):
		- absent:  IM=x101 3-wire 9-bit data serial interface
		- present: IM=x110 4-wire 8-bit data serial interface
- write-only:	LCD controller is write only. This depends on the interface
		mode, SPI master driver and wiring:
		- IM=11xx: MISO is not connected
		- IM=01xx: SPI master driver doesn't support spi-3wire (SDA)
- rotation:	panel rotation in degrees counter clockwise (0,90,180,270)
- backlight:	phandle of the backlight device attached to the panel

Example:
	mi0283qt@0{
		compatible = "multi-inno,mi0283qt";
		reg = <0>;
		spi-max-frequency = <32000000>;
		rotation = <90>;
		dc-gpios = <&gpio 25 0>;
		backlight = <&backlight>;
	};
-------------------------------------------------------------------------------
*/

#include <drm/tinydrm/ili9341.h>
#include <drm/tinydrm/mipi-dbi.h>
#include <drm/tinydrm/tinydrm.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <video/mipi_display.h>

static int mi0283qt_prepare(struct tinydrm_device *tdev)
{
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);
	struct regmap *reg = mipi->reg;
	u8 addr_mode;
	int ret;

	dev_dbg(tdev->base->dev, "%s\n", __func__);

	if (mipi->regulator) {
		ret = regulator_enable(mipi->regulator);
		if (ret) {
			dev_err(tdev->base->dev,
				"Failed to enable regulator %d\n", ret);
			return ret;
		}
	}

//	mipi_dbi_debug_dump_regs(reg);

	/* Avoid flicker by skipping setup if the bootloader has done it */
	if (mipi_dbi_display_is_on(reg))
		return 0;

	mipi_dbi_hw_reset(mipi);
	ret = mipi_dbi_write(reg, MIPI_DCS_SOFT_RESET);
	if (ret) {
		dev_err(tdev->base->dev, "Error writing command %d\n", ret);
		return ret;
	}

	msleep(20);

	mipi_dbi_write(reg, MIPI_DCS_SET_DISPLAY_OFF);

	mipi_dbi_write(reg, ILI9341_PWCTRLB, 0x00, 0x83, 0x30);
	mipi_dbi_write(reg, ILI9341_PWRSEQ, 0x64, 0x03, 0x12, 0x81);
	mipi_dbi_write(reg, ILI9341_DTCTRLA, 0x85, 0x01, 0x79);
	mipi_dbi_write(reg, ILI9341_PWCTRLA, 0x39, 0x2c, 0x00, 0x34, 0x02);
	mipi_dbi_write(reg, ILI9341_PUMPCTRL, 0x20);
	mipi_dbi_write(reg, ILI9341_DTCTRLB, 0x00, 0x00);

	/* Power Control */
	mipi_dbi_write(reg, ILI9341_PWCTRL1, 0x26);
	mipi_dbi_write(reg, ILI9341_PWCTRL2, 0x11);
	/* VCOM */
	mipi_dbi_write(reg, ILI9341_VMCTRL1, 0x35, 0x3e);
	mipi_dbi_write(reg, ILI9341_VMCTRL2, 0xbe);

	/* Memory Access Control */
	mipi_dbi_write(reg, MIPI_DCS_SET_PIXEL_FORMAT, 0x55);

	switch (mipi->rotation) {
	default:
		addr_mode = ILI9341_MADCTL_MV | ILI9341_MADCTL_MY |
			    ILI9341_MADCTL_MX;
		break;
	case 90:
		addr_mode = ILI9341_MADCTL_MY;
		break;
	case 180:
		addr_mode = ILI9341_MADCTL_MV;
		break;
	case 270:
		addr_mode = ILI9341_MADCTL_MX;
		break;
	}
	addr_mode |= ILI9341_MADCTL_BGR;
	mipi_dbi_write(reg, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);

	/* Frame Rate */
	mipi_dbi_write(reg, ILI9341_FRMCTR1, 0x00, 0x1b);

	/* Gamma */
	mipi_dbi_write(reg, ILI9341_EN3GAM, 0x08);
	mipi_dbi_write(reg, MIPI_DCS_SET_GAMMA_CURVE, 0x01);
	mipi_dbi_write(reg, ILI9341_PGAMCTRL,
		       0x1f, 0x1a, 0x18, 0x0a, 0x0f, 0x06, 0x45, 0x87,
		       0x32, 0x0a, 0x07, 0x02, 0x07, 0x05, 0x00);
	mipi_dbi_write(reg, ILI9341_NGAMCTRL,
		       0x00, 0x25, 0x27, 0x05, 0x10, 0x09, 0x3a, 0x78,
		       0x4d, 0x05, 0x18, 0x0d, 0x38, 0x3a, 0x1f);

	/* DDRAM */
	mipi_dbi_write(reg, ILI9341_ETMOD, 0x07);

	/* Display */
	mipi_dbi_write(reg, ILI9341_DISCTRL, 0x0a, 0x82, 0x27, 0x00);
	mipi_dbi_write(reg, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(100);

	mipi_dbi_write(reg, MIPI_DCS_SET_DISPLAY_ON);
	msleep(50);

//	mipi_dbi_debug_dump_regs(reg);

	return 0;
}

static const struct drm_display_mode mi0283qt_mode = {
	TINYDRM_MODE(320, 240, 58, 43),
};

static const struct tinydrm_funcs mi0283qt_funcs = {
	.prepare = mi0283qt_prepare,
	.unprepare = mipi_dbi_unprepare,
	.enable = mipi_dbi_enable_backlight,
	.disable = mipi_dbi_disable_backlight,
	.dirty = mipi_dbi_dirty,
};

static const struct of_device_id mi0283qt_of_match[] = {
	{ .compatible = "multi-inno,mi0283qt" },
	{},
};
MODULE_DEVICE_TABLE(of, mi0283qt_of_match);

static const struct spi_device_id mi0283qt_id[] = {
	{ "mi0283qt", 0 },
	{ },
};
MODULE_DEVICE_TABLE(spi, mi0283qt_id);

TINYDRM_DRM_DRIVER(mi0283qt_driver, "mi0283qt", "Multi-Inno MI0283QT",
		   "20160614");

static int mi0283qt_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct gpio_desc *reset, *dc;
	struct tinydrm_device *tdev;
	struct mipi_dbi *mipi;
	u32 rotation = 0;
	bool writeonly;
	int ret;

	if (!dev->coherent_dma_mask) {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret)
			dev_warn(dev, "Failed to set dma mask %d\n", ret);
	}

	mipi = devm_kzalloc(dev, sizeof(*mipi), GFP_KERNEL);
	if (!mipi)
		return -ENOMEM;

	reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(reset)) {
		dev_err(dev, "Failed to get gpio 'reset'\n");
		return PTR_ERR(reset);
	}

	dc = devm_gpiod_get_optional(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(dc)) {
		dev_err(dev, "Failed to get gpio 'dc'\n");
		return PTR_ERR(dc);
	}

	mipi->regulator = devm_regulator_get_optional(dev, "power");
	if (IS_ERR(mipi->regulator)) {
		ret = PTR_ERR(mipi->regulator);
		if (ret != -ENODEV)
			return ret;

		mipi->regulator = NULL;
	}

	mipi->backlight = tinydrm_of_find_backlight(dev);
	if (IS_ERR(mipi->backlight))
		return PTR_ERR(mipi->backlight);

	writeonly = device_property_read_bool(dev, "write-only");
	device_property_read_u32(dev, "rotation", &rotation);

	ret = mipi_dbi_spi_init(mipi, spi, dc, reset, writeonly);
	if (ret)
		return ret;

	ret = mipi_dbi_init(dev, mipi, &mi0283qt_driver, &mi0283qt_mode,
			    rotation);
	if (ret)
		return ret;

	tdev = &mipi->tinydrm;

	ret = devm_tinydrm_register(tdev, &mi0283qt_funcs);
	if (ret)
		return ret;

	spi_set_drvdata(spi, tdev);

	DRM_DEBUG_DRIVER("Initialized %s:%s on minor %d\n",
			 tdev->base->driver->name, dev_name(dev),
			 tdev->base->primary->index);

	return 0;
}

static struct spi_driver mi0283qt_spi_driver = {
	.driver = {
		.name = "mi0283qt",
		.owner = THIS_MODULE,
		.of_match_table = mi0283qt_of_match,
		.pm = &tinydrm_simple_pm_ops,
	},
	.id_table = mi0283qt_id,
	.probe = mi0283qt_probe,
	.shutdown = tinydrm_spi_shutdown,
};
module_spi_driver(mi0283qt_spi_driver);

MODULE_DESCRIPTION("Multi-Inno MI0283QT DRM driver");
MODULE_AUTHOR("Noralf Trønnes");
MODULE_LICENSE("GPL");
