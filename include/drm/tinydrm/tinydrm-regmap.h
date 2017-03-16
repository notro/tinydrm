/*
 * Copyright (C) 2017 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __LINUX_TINYDRM_REGMAP_H
#define __LINUX_TINYDRM_REGMAP_H



//Hmmmm
struct gpio_descs;
struct gpio_desc;
struct dentry;
struct regmap;

bool tinydrm_regmap_raw_swap_bytes(struct regmap *reg);

struct regmap *tinydrm_i80_init(struct device *dev, unsigned int reg_width,
				struct gpio_desc *cs, struct gpio_desc *idx,
				struct gpio_desc *wr, struct gpio_descs *db);

int tinydrm_regmap_debugfs_init(struct regmap *reg, struct dentry *parent);

#endif /* __LINUX_TINYDRM_REGMAP_H */
