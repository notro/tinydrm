/*
 * Copyright 2013-2017 Pervasive Displays, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __LINUX_REPAPER_H
#define __LINUX_REPAPER_H

#include <linux/gpio/consumer.h>
#include <linux/pwm.h>
#include <linux/spi/spi.h>

#include <drm/tinydrm/tinydrm.h>

struct repaper_epd {
	struct tinydrm_device tinydrm;
	struct spi_device *spi;

	void *buf;

	struct gpio_desc *panel_on;
	struct gpio_desc *border;
	struct gpio_desc *discharge;
	struct gpio_desc *reset;
	struct gpio_desc *busy;
	struct pwm_device *pwm;

	bool enabled;
	bool cleared;

	int stage_time;
	int factored_stage_time;
	int lines_per_display;
	int dots_per_line;
	int bytes_per_line;
	int bytes_per_scan;
	bool filler;

	const u8 *channel_select;
	size_t channel_select_length;
	const u8 *gate_source;
	size_t gate_source_length;

	u8 *line_buffer;
	size_t line_buffer_size;
};

static inline struct repaper_epd *
epd_from_tinydrm(struct tinydrm_device *tdev)
{
	return container_of(tdev, struct repaper_epd, tinydrm);
}

extern const struct drm_framebuffer_funcs repaper_v110g1_fb_funcs;
extern const struct drm_simple_display_pipe_funcs repaper_v110g1_pipe_funcs;

#endif /* __LINUX_REPAPER_H */
