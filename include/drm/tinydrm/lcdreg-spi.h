/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __LINUX_LCDREG_SPI_H
#define __LINUX_LCDREG_SPI_H

#include <drm/tinydrm/lcdreg.h>
#include <linux/spi/spi.h>

/**
 * enum lcdreg_spi_mode - SPI interface mode
 * @LCDREG_SPI_4WIRE: 8-bit + D/CX line, MIPI DBI Type C option 3
 * @LCDREG_SPI_3WIRE: 9-bit inc. D/CX bit, MIPI DBI Type C option 1
 * @LCDREG_SPI_STARTBYTE: Startbyte header on every transaction (non MIPI)
 */
enum lcdreg_spi_mode {
	LCDREG_SPI_NOMODE = 0,
	LCDREG_SPI_4WIRE,
	LCDREG_SPI_3WIRE,
	LCDREG_SPI_STARTBYTE,
};

/**
 * struct lcdreg_spi_config - SPI interface configuration
 * @mode: Register interface mode
 * @def_width: Default register width
 * @readable: Is the register readable, not all displays have MISO wired.
 * @id: Display id used with LCDREG_SPI_STARTBYTE
 * @dc_name: Index pin name, usually dc, rs or di (default is 'dc').
 * @quirks: Deviations from the MIPI DBI standard
 * @startbyte: Used with LCDREG_SPI_STARTBYTE to get the startbyte
 *             (default is lcdreg_spi_startbyte).
 */
struct lcdreg_spi_config {
	enum lcdreg_spi_mode mode;
	unsigned def_width;
	bool readable;
	u32 id;
	char *dc_name;
	u32 quirks;
/* slowdown command (index=0) */
#define LCDREG_SLOW_INDEX0_WRITE	BIT(0)
/*
 * The MIPI DBI spec states that D/C should be HIGH during register reading.
 * However, not all SPI master drivers support cs_change on last transfer and
 * there are LCD controllers that ignore D/C on read.
 */
#define LCDREG_INDEX0_ON_READ		BIT(1)

	u8 (*startbyte)(struct lcdreg *reg, struct lcdreg_transfer *tr,
			bool read);
};

struct lcdreg *devm_lcdreg_spi_init(struct spi_device *sdev,
				    const struct lcdreg_spi_config *config);

#endif /* __LINUX_LCDREG_SPI_H */
