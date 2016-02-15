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
struct drm_panel;
struct lcdreg;

int mipi_dbi_init(struct device *dev, struct tinydrm_device *tdev);
bool mipi_dbi_display_is_on(struct lcdreg *reg);
void mipi_dbi_debug_dump_regs(struct lcdreg *reg);
int mipi_dbi_panel_unprepare(struct drm_panel *panel);

#endif /* __LINUX_MIPI_DBI_H */
