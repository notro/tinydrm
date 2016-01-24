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

struct tinydrm_device;
struct lcdreg;

int mipi_dbi_update(struct tinydrm_device *tdev);
int mipi_dbi_check(struct lcdreg *reg);

#endif /* __LINUX_MIPI_DBI_H */
