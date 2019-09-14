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

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper.h>
#include <drm/tinydrm/mipi-dbi.h>

#include <video/mipi_display.h>

/* Renesas R61581 controller with a CPLD SPI conversion in front */
static void mz61581_enable(struct drm_simple_display_pipe *pipe,
			   struct drm_crtc_state *crtc_state,
			   struct drm_plane_state *plane_state)
{
	struct mipi_dbi *mipi = drm_to_mipi_dbi(pipe->crtc.dev);
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

	mipi_dbi_enable_flush(mipi, crtc_state, plane_state);
}

static const struct drm_simple_display_pipe_funcs mz61581_funcs = {
	.enable = mz61581_enable,
	.disable = mipi_dbi_pipe_disable,
	.update = mipi_dbi_pipe_update,
	.prepare_fb = drm_gem_fb_simple_display_pipe_prepare_fb,
};

static const struct drm_display_mode mz61581_mode = {
	DRM_SIMPLE_MODE(480, 320, 73, 49),
};

static struct drm_driver mz61581_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME |
				  DRIVER_ATOMIC,
	.release		= mipi_dbi_release,
	DRM_GEM_CMA_VMAP_DRIVER_OPS,
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
	struct drm_device *drm;
	struct mipi_dbi *mipi;
	struct gpio_desc *dc;
	u32 rotation = 0;
	int ret;

	mipi = kzalloc(sizeof(*mipi), GFP_KERNEL);
	if (!mipi)
		return -ENOMEM;

	drm = &mipi->drm;
	ret = devm_drm_dev_init(dev, drm, &mz61581_driver);
	if (ret) {
		kfree(mipi);
		return ret;
	}

	drm_mode_config_init(drm);

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

	mipi->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(mipi->backlight))
		return PTR_ERR(mipi->backlight);

	device_property_read_u32(dev, "rotation", &rotation);

	ret = mipi_dbi_spi_init(spi, mipi, dc);
	if (ret)
		return ret;

	ret = mipi_dbi_init(mipi, &mz61581_funcs, &mz61581_mode, rotation);
	if (ret)
		return ret;

	/* Reading is not supported */
	mipi->read_commands = NULL;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	spi_set_drvdata(spi, drm);

	drm_fbdev_generic_setup(drm, 16);

	return 0;
}

static int mz61581_remove(struct spi_device *spi)
{
	struct drm_device *drm = spi_get_drvdata(spi);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);

	return 0;
}

static void mz61581_shutdown(struct spi_device *spi)
{
	drm_atomic_helper_shutdown(spi_get_drvdata(spi));
}

static struct spi_driver mz61581_spi_driver = {
	.driver = {
		.name = "mz61581",
		.owner = THIS_MODULE,
		.of_match_table = mz61581_of_match,
	},
	.id_table = mz61581_id,
	.probe = mz61581_probe,
	.remove = mz61581_remove,
	.shutdown = mz61581_shutdown,
};
module_spi_driver(mz61581_spi_driver);

MODULE_DESCRIPTION("Tontec mz61581 DRM driver");
MODULE_AUTHOR("Noralf Trønnes");
MODULE_LICENSE("GPL");
