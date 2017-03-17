/*
 * DRM driver for Tontec mz61581 panels
 *
 * Copyright 2017 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/spi/spi.h>

#include <drm/tinydrm/mipi-dbi.h>
#include <drm/tinydrm/tinydrm-helpers.h>

#include <video/mipi_display.h>

/* Renesas R61581 controller with a CPLD SPI conversion in front */
static void mz61581_enable(struct drm_simple_display_pipe *pipe,
			   struct drm_crtc_state *crtc_state)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);
	struct drm_framebuffer *fb = pipe->plane.fb;
	u8 addr_mode;

	DRM_DEBUG_KMS("\n");

	mipi_dbi_hw_reset(mipi);

	mipi_dbi_command(mipi, 0xb0, 0x00);
	mipi_dbi_command(mipi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(120);

	mipi_dbi_command(mipi, 0xb3, 0x02, 0x00, 0x00, 0x00);
	mipi_dbi_command(mipi, 0xc0, 0x13, 0x3b, 0x00, 0x02,
				     0x00, 0x01, 0x00, 0x43);
	mipi_dbi_command(mipi, 0xc1, 0x08, 0x16, 0x08, 0x08);
	mipi_dbi_command(mipi, 0xc4, 0x11, 0x07, 0x03, 0x03);
	mipi_dbi_command(mipi, 0xc6, 0x00);
	mipi_dbi_command(mipi, 0xc8, 0x03, 0x03, 0x13, 0x5c, 0x03,
				     0x07, 0x14, 0x08, 0x00, 0x21,
				     0x08, 0x14, 0x07, 0x53, 0x0c,
				     0x13, 0x03, 0x03, 0x21, 0x00);
	mipi_dbi_command(mipi, MIPI_DCS_SET_TEAR_ON, 0x00);
	mipi_dbi_command(mipi, MIPI_DCS_SET_ADDRESS_MODE, 0xa0);
	mipi_dbi_command(mipi, MIPI_DCS_SET_PIXEL_FORMAT, 0x55);
	mipi_dbi_command(mipi, MIPI_DCS_SET_TEAR_SCANLINE, 0x00, 0x01);
	mipi_dbi_command(mipi, 0xd0, 0x07, 0x07, 0x1d, 0x03);
	mipi_dbi_command(mipi, 0xd1, 0x03, 0x30, 0x10);
	mipi_dbi_command(mipi, 0xd2, 0x03, 0x14, 0x04);

#define MY BIT(7)
#define MX BIT(6)
#define MV BIT(5)
#define BGR BIT(3)

	switch (mipi->rotation) {
	case 90:
		addr_mode = MY | MX;
		break;
	case 180:
		addr_mode = MX | MV;
		break;
	case 270:
		addr_mode = 0;
		break;
	default:
		addr_mode = MY | MV;
		break;
	}
	addr_mode |= BGR;
	mipi_dbi_command(mipi, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);

	mipi_dbi_command(mipi, MIPI_DCS_SET_DISPLAY_ON);

	mipi->enabled = true;
	fb->funcs->dirty(fb, NULL, 0, 0, NULL, 0);

	tinydrm_enable_backlight(mipi->backlight);
}

static void mz61581_disable(struct drm_simple_display_pipe *pipe)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);

	DRM_DEBUG_KMS("\n");

	mipi->enabled = false;
	tinydrm_disable_backlight(mipi->backlight);
}

static const struct drm_simple_display_pipe_funcs mz61581_funcs = {
	.enable = mz61581_enable,
	.disable = mz61581_disable,
	.update = tinydrm_display_pipe_update,
	.prepare_fb = tinydrm_display_pipe_prepare_fb,
};

static const struct drm_display_mode mz61581_mode = {
	TINYDRM_MODE(480, 320, 73, 49),
};

static struct drm_driver mz61581_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME |
				  DRIVER_ATOMIC,
	TINYDRM_GEM_DRIVER_OPS,
	.lastclose		= tinydrm_lastclose,
	.debugfs_init		= mipi_dbi_debugfs_init,
	.name			= "mz61581",
	.desc			= "Tontec mz61581",
	.date			= "20170316",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id mz61581_of_match[] = {
	{ .compatible = "tontec,mz61581" },
	{},
};
MODULE_DEVICE_TABLE(of, mz61581_of_match);

static const struct spi_device_id mz61581_id[] = {
	{ "mz61581", 0 },
	{ },
};
MODULE_DEVICE_TABLE(spi, mz61581_id);

static int mz61581_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct tinydrm_device *tdev;
	struct mipi_dbi *mipi;
	struct gpio_desc *dc;
	u32 rotation = 0;
	int ret;

	mipi = devm_kzalloc(dev, sizeof(*mipi), GFP_KERNEL);
	if (!mipi)
		return -ENOMEM;

	mipi->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(mipi->reset)) {
		dev_err(dev, "Failed to get gpio 'reset'\n");
		return PTR_ERR(mipi->reset);
	}

	dc = devm_gpiod_get(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(dc)) {
		dev_err(dev, "Failed to get gpio 'dc'\n");
		return PTR_ERR(dc);
	}

	mipi->backlight = tinydrm_of_find_backlight(dev);
	if (IS_ERR(mipi->backlight))
		return PTR_ERR(mipi->backlight);

	device_property_read_u32(dev, "rotation", &rotation);

	ret = mipi_dbi_spi_init(spi, mipi, dc, &mz61581_funcs,
				&mz61581_driver, &mz61581_mode, rotation);
	if (ret)
		return ret;

	/* Reading is not supported */
	mipi->read_commands = NULL;

	tdev = &mipi->tinydrm;

	ret = devm_tinydrm_register(tdev);
	if (ret)
		return ret;

	spi_set_drvdata(spi, mipi);

	DRM_DEBUG_DRIVER("Initialized %s:%s @%uMHz on minor %d\n",
			 tdev->drm->driver->name, dev_name(dev),
			 spi->max_speed_hz / 1000000,
			 tdev->drm->primary->index);

	return 0;
}

static void mz61581_shutdown(struct spi_device *spi)
{
	struct mipi_dbi *mipi = spi_get_drvdata(spi);

	tinydrm_shutdown(&mipi->tinydrm);
}

static struct spi_driver mz61581_spi_driver = {
	.driver = {
		.name = "mz61581",
		.owner = THIS_MODULE,
		.of_match_table = mz61581_of_match,
	},
	.id_table = mz61581_id,
	.probe = mz61581_probe,
	.shutdown = mz61581_shutdown,
};
module_spi_driver(mz61581_spi_driver);

MODULE_DESCRIPTION("Tontec mz61581 DRM driver");
MODULE_AUTHOR("Noralf Trønnes");
MODULE_LICENSE("GPL");
