/*
 * DRM driver for Pervasive Displays RePaper e-ink panels
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
//#include <linux/property.h>
#include <linux/pwm.h>
#include <linux/spi/spi.h>

#include <drm/tinydrm/tinydrm.h>

#include "repaper.h"

enum repaper_model {
	EN027AS012 = 1,
};

static const uint32_t repaper_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

static const struct drm_display_mode repaper_en027as012_mode = {
	TINYDRM_MODE(264, 176, 57, 38),
};

static const u8 repaper_en027as012_cs[] = { 0x00, 0x00, 0x00, 0x7f,
					    0xff, 0xfe, 0x00, 0x00 };
static const u8 repaper_en027as012_gs[] = { 0x00 };

// for 4.12
//DEFINE_DRM_GEM_CMA_FOPS(repaper_fops);

static struct drm_driver repaper_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME |
				  DRIVER_ATOMIC,
//	.fops			= &repaper_fops,
	TINYDRM_GEM_DRIVER_OPS,
	.name			= "repaper",
	.desc			= "Pervasive Displays RePaper e-ink panels",
	.date			= "20170405",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id repaper_of_match[] = {
	{ .compatible = "pervasive,en027as012", .data = (void *)EN027AS012 },
	{},
};
MODULE_DEVICE_TABLE(of, repaper_of_match);

static const struct spi_device_id repaper_id[] = {
	{ "en027as012", EN027AS012 },
	{ },
};
MODULE_DEVICE_TABLE(spi, repaper_id);

static int repaper_probe(struct spi_device *spi)
{


	const struct drm_framebuffer_funcs *fb_funcs;
	const struct drm_simple_display_pipe_funcs *pipe_funcs;


	const struct drm_display_mode *mode;
	const struct spi_device_id *spi_id;
	const struct of_device_id *match;
	struct device *dev = &spi->dev;
	struct tinydrm_device *tdev;
	enum repaper_model model;
	struct repaper_epd *epd;
	struct pwm_args pargs;
	int ret;

	match = of_match_device(repaper_of_match, dev);
	if (match) {
		model = (enum repaper_model)match->data;
	} else {
		spi_id = spi_get_device_id(spi);
		model = spi_id->driver_data;
	}

	/* The SPI device is used to allocate dma memory */
	if (!dev->coherent_dma_mask) {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_warn(dev, "Failed to set dma mask %d\n", ret);
			return ret;
		}
	}

	epd = devm_kzalloc(dev, sizeof(*epd), GFP_KERNEL);
	if (!epd)
		return -ENOMEM;

	epd->spi = spi;

	epd->panel_on = devm_gpiod_get(dev, "panel-on", GPIOD_OUT_LOW);
	if (IS_ERR(epd->panel_on)) {
		ret = PTR_ERR(epd->panel_on);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get gpio 'panel-on'\n");
		return ret;
	}

	epd->border = devm_gpiod_get(dev, "border", GPIOD_OUT_LOW);
	if (IS_ERR(epd->border)) {
		ret = PTR_ERR(epd->border);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get gpio 'border'\n");
		return ret;
	}

	epd->discharge = devm_gpiod_get(dev, "discharge", GPIOD_OUT_LOW);
	if (IS_ERR(epd->discharge)) {
		ret = PTR_ERR(epd->discharge);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get gpio 'discharge'\n");
		return ret;
	}

	epd->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(epd->reset)) {
		ret = PTR_ERR(epd->reset);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get gpio 'reset'\n");
		return ret;
	}

	epd->busy = devm_gpiod_get(dev, "busy", GPIOD_IN);
	if (IS_ERR(epd->busy)) {
		ret = PTR_ERR(epd->busy);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get gpio 'busy'\n");
		return ret;
	}

	/* PWM is needed by the COG1 driven panels */
	switch (model) {
/*
	case EPD_1_44:
	case EPD_2_0:
*/
	case EN027AS012:
		epd->pwm = devm_pwm_get(dev, "pwm");
		if (IS_ERR(epd->pwm)) {
			ret = PTR_ERR(epd->pwm);
			if (ret != -EPROBE_DEFER)
				dev_err(dev, "Failed to get pwm\n");
			return ret;
		}

		pwm_get_args(epd->pwm, &pargs);
		pwm_config(epd->pwm, pargs.period / 2, pargs.period);
		break;
	}

	switch (model) {
/*
	case EPD_1_44: {
		static u8 cs[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xff, 0x00 };
		static u8 gs[] = { 0x03 };

		epd->stage_time = 480; // milliseconds
		epd->lines_per_display = 96;
		epd->dots_per_line = 128;
		epd->bytes_per_line = 128 / 8;
		epd->bytes_per_scan = 96 / 4;
		epd->filler = false;
		epd->channel_select = cs;
		epd->channel_select_length = sizeof(cs);
		epd->gate_source = gs;
		epd->gate_source_length = sizeof(gs);
		break;
	}
	case EPD_2_0: {
		static u8 cs[] = { 0x00, 0x00, 0x00, 0x00, 0x01, 0xff, 0xe0, 0x00 };
		static u8 gs[] = { 0x03 };

		epd->stage_time = 480; // milliseconds
		epd->lines_per_display = 96;
		epd->dots_per_line = 200;
		epd->bytes_per_line = 200 / 8;
		epd->bytes_per_scan = 96 / 4;
		epd->filler = true;
		epd->channel_select = cs;
		epd->channel_select_length = sizeof(cs);
		epd->gate_source = gs;
		epd->gate_source_length = sizeof(gs);
		break;
	}
*/
	case EN027AS012:
		pipe_funcs = &repaper_v110g1_pipe_funcs;
		fb_funcs = &repaper_v110g1_fb_funcs;
		mode = &repaper_en027as012_mode;

		epd->stage_time = 630; /* milliseconds */
		epd->lines_per_display = mode->vdisplay;
		epd->dots_per_line = mode->hdisplay;
		epd->bytes_per_line = epd->dots_per_line / 8;
		epd->bytes_per_scan = epd->lines_per_display / 4;
		epd->filler = true;
		epd->channel_select = repaper_en027as012_cs;
		epd->channel_select_length = sizeof(repaper_en027as012_cs);
		epd->gate_source = repaper_en027as012_gs;
		epd->gate_source_length = sizeof(repaper_en027as012_gs);
		break;
	}

	epd->factored_stage_time = epd->stage_time;

	/* Add for command byte, border byte and filler byte */
	epd->line_buffer_size = 2 * epd->bytes_per_line +
				epd->bytes_per_scan + 3;

	epd->line_buffer = devm_kmalloc(dev, epd->line_buffer_size, GFP_KERNEL);
	if (!epd->line_buffer)
		return -ENOMEM;

	epd->buf = devm_kmalloc(dev, mode->hdisplay * mode->vdisplay * 2,
				GFP_KERNEL);
	if (!epd->buf)
		return -ENOMEM;

	tdev = &epd->tinydrm;

	ret = devm_tinydrm_init(dev, tdev, fb_funcs, &repaper_driver);
	if (ret)
		return ret;

	ret = tinydrm_display_pipe_init(tdev, pipe_funcs,
					DRM_MODE_CONNECTOR_VIRTUAL,
					repaper_formats,
					ARRAY_SIZE(repaper_formats), mode, 0);
	if (ret)
		return ret;

	tdev->drm->mode_config.preferred_depth = 16;

	drm_mode_config_reset(tdev->drm);

	ret = devm_tinydrm_register(tdev);
	if (ret)
		return ret;

	spi_set_drvdata(spi, tdev);

	DRM_DEBUG_DRIVER("Initialized %s:%s @%uMHz on minor %d\n",
			 tdev->drm->driver->name, dev_name(dev),
			 spi->max_speed_hz / 1000000,
			 tdev->drm->primary->index);

	return 0;
}

static void repaper_shutdown(struct spi_device *spi)
{
	struct tinydrm_device *tdev = spi_get_drvdata(spi);

	tinydrm_shutdown(tdev);
}

static struct spi_driver repaper_spi_driver = {
	.driver = {
		.name = "repaper",
		.owner = THIS_MODULE,
		.of_match_table = repaper_of_match,
	},
	.id_table = repaper_id,
	.probe = repaper_probe,
	.shutdown = repaper_shutdown,
};
module_spi_driver(repaper_spi_driver);

MODULE_DESCRIPTION("Pervasive Displays RePaper DRM driver");
MODULE_AUTHOR("Noralf Trønnes");
MODULE_LICENSE("GPL");
