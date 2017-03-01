/*
 * Copyright (C) 2017 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __LINUX_TINYDRM_PANEL_H
#define __LINUX_TINYDRM_PANEL_H

/*
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX

	FIXME

	select REGMAP

XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
*/

#include <drm/tinydrm/tinydrm.h>

struct tinydrm_panel;
struct gpio_desc;
struct regmap;

/**
 * struct tinydrm_panel_funcs - tinydrm panel functions
 *
 * All functions are optional.
 */
struct tinydrm_panel_funcs {
	/**
	 * @prepare:
	 *
	 * Prepare controller/display.
	 * This function is called before framebuffer flushing starts.
	 * Drivers can use callback to power on and configure the
	 * controller/display.
	 */
	int (*prepare)(struct tinydrm_panel *panel);

	/**
	 * @enable:
	 *
	 * Enable display.
	 * This function is called when the display pipeline is enabled.
	 * Drivers can use this callback to turn on the display.
	 * If this is not set and &tinydrm_panel->backlight is set,
	 * the backlight is turned on.
	 */
	int (*enable)(struct tinydrm_panel *panel);

	/**
	 * @disable:
	 *
	 * Disable display.
	 * This function is called when the display pipeline is disabled.
	 * Drivers can use this callback to turn off the display.
	 * If this is not set and &tinydrm_panel->backlight is set,
	 * the backlight is turned off.
	 */
	int (*disable)(struct tinydrm_panel *panel);

	/**
	 * @unprepare:
	 *
	 * Unprepare controller/display.
	 * This function is called when no framebuffer is set on the plane.
	 * Drivers can use callback to power down the controller/display.
	 */
	int (*unprepare)(struct tinydrm_panel *panel);
};

/**
 * tinydrm_panel - tinydrm panel device
 * @tinydrm: Base &tinydrm_device
 * @funcs: tinydrm panel functions (optional)
 * @reg: Register map
// * @enabled: Pipeline is enabled
 * @tx_buf: Transmit buffer
 * @swap_bytes: Swap pixel data bytes
 * @always_tx_buf: Always use @tx_buf
 * @rotation: Rotation in degrees Counter Clock Wise
 * @reset: Optional reset gpio
 * @backlight: Optional backlight device
 * @regulator: Optional regulator
 */
struct tinydrm_panel {
	struct tinydrm_device tinydrm;
	const struct tinydrm_panel_funcs *funcs;
	struct regmap *reg;
//	bool enabled;
	void *tx_buf;
	bool swap_bytes;
	bool always_tx_buf;
	unsigned int rotation;
	struct gpio_desc *reset;
	struct backlight_device *backlight;
	struct regulator *regulator;
};

static inline struct tinydrm_panel *
to_tinydrm_panel(struct tinydrm_device *tdev)
{
	return container_of(tdev, struct tinydrm_panel, tinydrm);
}

int tinydrm_panel_init(struct device *dev, struct tinydrm_panel *panel,
			const struct tinydrm_panel_funcs *funcs,
			const uint32_t *formats, unsigned int format_count,
			const struct drm_framebuffer_funcs *fb_funcs,
		  	struct drm_driver *driver,
		  	const struct drm_display_mode *mode,
		  	unsigned int rotation);

bool tinydrm_regmap_raw_swap_bytes(struct regmap *reg);

#ifdef CONFIG_DEBUG_FS
int tinydrm_panel_debugfs_init(struct drm_minor *minor);
#else
#define tinydrm_panel_debugfs_init	NULL
#endif

#endif /* __LINUX_TINYDRM_PANEL_H */
