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

enum lcdreg_spi_mode {
	LCDREG_SPI_4WIRE, /* 8-bit + D/CX line, MIPI DBI Type C option 3 */
	LCDREG_SPI_3WIRE, /* 9-bit inc. D/CX bit, MIPI DBI Type C option 1 */
	LCDREG_SPI_STARTBYTE,
};

struct lcdreg_spi_config {
	enum lcdreg_spi_mode mode;
	unsigned def_width;
	bool readable;
	u32 id;
	char *dc_name;
	u8 (*startbyte)(struct lcdreg *reg, struct lcdreg_transfer *tr,
			bool read);
};

struct lcdreg *devm_lcdreg_spi_init(struct spi_device *sdev,
				    const struct lcdreg_spi_config *config);

#endif /* __LINUX_LCDREG_SPI_H */
