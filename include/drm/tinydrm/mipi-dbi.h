/*
 * MIPI Display Bus Interface (DBI) LCD controller support
 *
 * Copyright 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __LINUX_MIPI_DBI_H
#define __LINUX_MIPI_DBI_H

#include <drm/tinydrm/tinydrm.h>

struct drm_gem_cma_object;
struct drm_framebuffer;
struct drm_clip_rect;
struct spi_device;
struct gpio_desc;
struct regulator;

/**
 * mipi_dbi - MIPI DBI controller
 * @tinydrm: tinydrm base
 * @spi: SPI device
 * @command: Bus specific callback executing commands.
 * @read_commands: Array of read commands terminated by a zero entry.
 * @dc: Optional D/C gpio.
 * @write_only: Controller is write only.
 * @tx_buf: Buffer used for transfer (copy clip rect area)
 * @tx_buf9: Buffer used for Option 1 9-bit conversion
 * @tx_buf9_len: Size of tx_buf9.
 * @swap_bytes: Swap bytes in buffer before transfer
 * @reset: Optional reset gpio
 * @rotation: initial rotation in degress Counter Clock Wise
 * @backlight: backlight device (optional)
 * @enable_delay_ms: Optional delay in milliseconds before turning on backlight
 * @regulator: power regulator (optional)
 */
struct mipi_dbi {
	struct tinydrm_device tinydrm;
	struct spi_device *spi;
	int (*command)(struct mipi_dbi *mipi, u8 cmd, u8 *parameters, size_t num);
	const u8 *read_commands;
	struct gpio_desc *dc;
	bool write_only;
	u16 *tx_buf;
	void *tx_buf9;
	size_t tx_buf9_len;
	bool swap_bytes;
	struct gpio_desc *reset;
	unsigned int rotation;
	struct backlight_device *backlight;
	unsigned int enable_delay_ms;
	struct regulator *regulator;
};

static inline struct mipi_dbi *
mipi_dbi_from_tinydrm(struct tinydrm_device *tdev)
{
	return container_of(tdev, struct mipi_dbi, tinydrm);
}

int mipi_dbi_spi_init(struct spi_device *spi, struct mipi_dbi *mipi,
		      struct gpio_desc *dc, bool write_only,
		      const struct drm_simple_display_pipe_funcs *pipe_funcs,
		      struct drm_driver *driver,
		      const struct drm_display_mode *mode,
		      unsigned int rotation);
int mipi_dbi_init(struct device *dev, struct mipi_dbi *mipi,
		  const struct drm_simple_display_pipe_funcs *pipe_funcs,
		  struct drm_driver *driver,
		  const struct drm_display_mode *mode, unsigned int rotation);
void mipi_dbi_pipe_disable(struct drm_simple_display_pipe *pipe);
void mipi_dbi_hw_reset(struct mipi_dbi *mipi);
bool mipi_dbi_display_is_on(struct mipi_dbi *mipi);

/**
 * mipi_dbi_command - MIPI DCS command with optional parameter(s)
 * @mipi: MIPI structure
 * @cmd: Command
 * @...: Parameters
 *
 * Send MIPI DCS command to the controller. Use mipi_dbi_command_buf() for get/read.
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
#define mipi_dbi_command(mipi, cmd, seq...) \
({ \
	u8 d[] = { seq }; \
	mipi->command(mipi, cmd, d, ARRAY_SIZE(d)); \
})

/**
 * mipi_dbi_command_buf - MIPI DCS command with parameter(s) in an array
 * @mipi: MIPI structure
 * @cmd: Command
 * @data: Parameter buffer
 * @len: Buffer length
 *
 * This function should be used for read commands.
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
static inline int mipi_dbi_command_buf(struct mipi_dbi *mipi, u8 cmd,
				       u8 *data, size_t len)
{
	return mipi->command(mipi, cmd, data, len);
}

#ifdef CONFIG_DEBUG_FS
int mipi_dbi_debugfs_init(struct drm_minor *minor);
void mipi_dbi_debugfs_cleanup(struct drm_minor *minor);
#else
#define mipi_dbi_debugfs_init		NULL
#define mipi_dbi_debugfs_cleanup	NULL
#endif

#endif /* __LINUX_MIPI_DBI_H */
