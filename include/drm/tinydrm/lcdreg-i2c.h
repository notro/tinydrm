/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __LINUX_LCDREG_I2C_H
#define __LINUX_LCDREG_I2C_H

#include <drm/tinydrm/lcdreg.h>

struct i2c_client;

struct lcdreg *devm_lcdreg_i2c_init(struct i2c_client *client);

#endif /* __LINUX_LCDREG_I2C_H */
