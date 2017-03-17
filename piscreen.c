/*
 * DRM driver for Ozzmaker PiScreen displays
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
#include <linux/of_device.h>
#include <linux/property.h>
#include <linux/spi/spi.h>

#include <drm/tinydrm/mipi-dbi.h>
#include <drm/tinydrm/tinydrm-helpers.h>

#include <video/mipi_display.h>

/*
 * The PiScreen has a SPI to 16-bit parallel bus converter in front of the
 * display controller. This means that 8-bit values has to be transferred
 * as 16-bit.
 */
static int piscreen_command(struct mipi_dbi *mipi, u8 cmd, u8 *par, size_t num)
{
	struct spi_device *spi = mipi->spi;
	void *data = par;
	u32 speed_hz = 0;
	int i, ret;
	u16 *buf;

	buf = kmalloc(32 * sizeof(u16), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (!num)
		DRM_DEBUG_DRIVER("cmd=%02x\n", cmd);
	else if (num <= 32)
		DRM_DEBUG_DRIVER("cmd=%02x, par=%*ph\n", cmd, (int)num, par);
	else
		DRM_DEBUG_DRIVER("cmd=%02x, len=%zu\n", cmd, num);

	/*
	 * The Raspberry Pi supports only 8-bit on the DMA capable SPI
	 * controller and is little endian, so byte swapping is needed.
	 */
	buf[0] = cpu_to_be16(cmd);
	gpiod_set_value_cansleep(mipi->dc, 0);
	ret = tinydrm_spi_transfer(spi, 10000000, NULL, 8, buf, 2);
	if (ret || !num)
		goto free;

//	if (cmd == MIPI_DCS_WRITE_MEMORY_START && !mipi->swap_bytes)
//		bpw = 16;

	/* 8-bit configuration data, not 16-bit pixel data */
	if (num <= 32) {
		for (i = 0; i < num; i++)
			buf[i] = cpu_to_be16(par[i]);
		num *= 2;
		speed_hz = 10000000; /* slow down config */
		data = buf;
	}

	gpiod_set_value_cansleep(mipi->dc, 1);
	ret = tinydrm_spi_transfer(spi, speed_hz, NULL, 8, data, num);
free:
	kfree(buf);

	return ret;
}

/* ILI9486 controller */
static void piscreen_enable(struct drm_simple_display_pipe *pipe,
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

	mipi_dbi_command(mipi, MIPI_DCS_SET_PIXEL_FORMAT, 0x55);
	mipi_dbi_command(mipi, 0xc2, 0x44);
	mipi_dbi_command(mipi, 0xc5, 0x00, 0x00, 0x00, 0x00);
	mipi_dbi_command(mipi, 0xe0, 0x0f, 0x1f, 0x1c, 0x0c, 0x0f,
				     0x08, 0x48, 0x98, 0x37, 0x0a,
				     0x13, 0x04, 0x11, 0x0d, 0x00);
	mipi_dbi_command(mipi, 0xe1, 0x0f, 0x32, 0x2e, 0x0b, 0x0d,
				     0x05, 0x47, 0x75, 0x37, 0x06,
				     0x10, 0x03, 0x24, 0x20, 0x00);
	mipi_dbi_command(mipi, 0xe2, 0x0f, 0x32, 0x2e, 0x0b, 0x0d,
				     0x05, 0x47, 0x75, 0x37, 0x06,
				     0x10, 0x03, 0x24, 0x20, 0x00);

#define MY BIT(7)
#define MX BIT(6)
#define MV BIT(5)
#define BGR BIT(3)

	switch (mipi->rotation) {
	case 90:
		addr_mode = MY;
		break;
	case 180:
		addr_mode = MV;
		break;
	case 270:
		addr_mode = MX;
		break;
	default:
		addr_mode = MY | MX | MV;
		break;
	}
	addr_mode |= BGR;
	mipi_dbi_command(mipi, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);

	mipi_dbi_command(mipi, MIPI_DCS_SET_DISPLAY_ON);

	mipi->enabled = true;
	fb->funcs->dirty(fb, NULL, 0, 0, NULL, 0);

	tinydrm_enable_backlight(mipi->backlight);
}

static void piscreen_disable(struct drm_simple_display_pipe *pipe)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);

	DRM_DEBUG_KMS("\n");

	mipi->enabled = false;
	tinydrm_disable_backlight(mipi->backlight);
}

static const struct drm_simple_display_pipe_funcs piscreen_funcs = {
	.enable = piscreen_enable,
	.disable = piscreen_disable,
	.update = tinydrm_display_pipe_update,
	.prepare_fb = tinydrm_display_pipe_prepare_fb,
};

/* ILI9488 controller */
static void piscreen2_enable(struct drm_simple_display_pipe *pipe,
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

	mipi_dbi_command(mipi, MIPI_DCS_SET_PIXEL_FORMAT, 0x55);
	mipi_dbi_command(mipi, 0xc0, 0x11, 0x09);
	mipi_dbi_command(mipi, 0xc1, 0x41);
	mipi_dbi_command(mipi, 0xc5, 0x00, 0x00, 0x00, 0x00);
	mipi_dbi_command(mipi, 0xb6, 0x00, 0x02);
	mipi_dbi_command(mipi, 0xf7, 0xa9, 0x51, 0x2c, 0x2);
	mipi_dbi_command(mipi, 0xbe, 0x00, 0x04);
	mipi_dbi_command(mipi, 0xe9, 0x00);

#define MY BIT(7)
#define MX BIT(6)
#define MV BIT(5)
#define BGR BIT(3)

	switch (mipi->rotation) {
	default:
		addr_mode = MV; //0x20;
		break;
	case 90:
		addr_mode = MX; //0x40;
		break;
	case 180:
		addr_mode = MY | MX | MV; //0xe0;
		break;
	case 270:
		addr_mode = MY; //0x80;
		break;
	}
	addr_mode |= BGR;
	mipi_dbi_command(mipi, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);

	mipi_dbi_command(mipi, MIPI_DCS_SET_DISPLAY_ON);

	mipi->enabled = true;
	fb->funcs->dirty(fb, NULL, 0, 0, NULL, 0);

	tinydrm_enable_backlight(mipi->backlight);
}

static const struct drm_simple_display_pipe_funcs piscreen2_funcs = {
	.enable = piscreen2_enable,
	.disable = piscreen_disable,
	.update = tinydrm_display_pipe_update,
	.prepare_fb = tinydrm_display_pipe_prepare_fb,
};

static const struct drm_display_mode piscreen_mode = {
	TINYDRM_MODE(480, 320, 73, 49),
};

static struct drm_driver piscreen_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME |
				  DRIVER_ATOMIC,
	TINYDRM_GEM_DRIVER_OPS,
	.lastclose		= tinydrm_lastclose,
	.debugfs_init		= mipi_dbi_debugfs_init,
	.name			= "piscreen",
	.desc			= "Ozzmaker PiScreen",
	.date			= "20170317",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id piscreen_of_match[] = {
	{ .compatible = "ozzmaker,piscreen", .data = &piscreen_funcs },
	{ .compatible = "ozzmaker,piscreen2", .data = &piscreen2_funcs },
	{},
};
MODULE_DEVICE_TABLE(of, piscreen_of_match);

static int piscreen_probe(struct spi_device *spi)
{
	const struct drm_simple_display_pipe_funcs *funcs;
	const struct of_device_id *match;
	struct device *dev = &spi->dev;
	struct tinydrm_device *tdev;
	struct mipi_dbi *mipi;
	struct gpio_desc *dc;
	u32 rotation = 0;
	int ret;

	match = of_match_device(piscreen_of_match, dev);
	if (!match)
		return -ENODEV;

	funcs = match->data;

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

	ret = mipi_dbi_spi_init(spi, mipi, dc, funcs, &piscreen_driver,
				&piscreen_mode, rotation);
	if (ret)
		return ret;

	mipi->command = piscreen_command;
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

static void piscreen_shutdown(struct spi_device *spi)
{
	struct mipi_dbi *mipi = spi_get_drvdata(spi);

	tinydrm_shutdown(&mipi->tinydrm);
}

static struct spi_driver piscreen_spi_driver = {
	.driver = {
		.name = "piscreen",
		.owner = THIS_MODULE,
		.of_match_table = piscreen_of_match,
	},
	.probe = piscreen_probe,
	.shutdown = piscreen_shutdown,
};
module_spi_driver(piscreen_spi_driver);

MODULE_ALIAS("spi:piscreen");
MODULE_ALIAS("spi:piscreen2");

MODULE_DESCRIPTION("Ozzmaker PiScreen DRM driver");
MODULE_AUTHOR("Noralf Trønnes");
MODULE_LICENSE("GPL");
