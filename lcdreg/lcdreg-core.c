//#define DEBUG
/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/tinydrm/lcdreg.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "internal.h"

/**
 * DOC: Overview
 *
 * This library provides a register interface abstraction for LCD controllers
 * that are similar to MIPI DBI/DCS.
 */

/**
 * Write to LCD register
 *
 * @reg: LCD register
 * @regnr: Register number
 * @transfer: Transfer to write
 */
int lcdreg_write(struct lcdreg *reg, unsigned regnr,
		 struct lcdreg_transfer *transfer)
{
	if (WARN_ON_ONCE(!reg || !reg->write || !transfer))
		return -EINVAL;

	if (!transfer->width)
		transfer->width = reg->def_width;

	dev_dbg(reg->dev,
		"lcdreg_write: regnr=0x%02x, index=%u, count=%u, width=%u\n",
		regnr, transfer->index, transfer->count, transfer->width);
	lcdreg_dbg_transfer_buf(transfer);

	return reg->write(reg, regnr, transfer);
}
EXPORT_SYMBOL(lcdreg_write);

/**
 * Write 32-bit wide buffer to LCD register
 * @reg: lcdreg
 * @regnr: Register number
 * @buf: Buffer to write
 * @count: Number of words to write
 */
int lcdreg_write_buf32(struct lcdreg *reg, unsigned regnr, const u32 *buf,
		       unsigned count)
{
	struct lcdreg_transfer tr = {
		.index = 1,
		.width = reg->def_width,
		.count = count,
	};
	int i, ret;

	if (!buf)
		return -EINVAL;

	tr.buf = kmalloc_array(count, sizeof(*buf), GFP_KERNEL);
	if (!tr.buf)
		return -ENOMEM;

	if (reg->def_width <= 8)
		for (i = 0; i < tr.count; i++)
			((u8 *)tr.buf)[i] = buf[i];
	else
		for (i = 0; i < tr.count; i++)
			((u16 *)tr.buf)[i] = buf[i];
	ret = lcdreg_write(reg, regnr, &tr);
	kfree(tr.buf);

	return ret;
}
EXPORT_SYMBOL(lcdreg_write_buf32);

/**
 * Read from LCD register
 *
 * @reg: LCD register
 * @regnr: Register number
 * @transfer: Transfer to read into
 */
int lcdreg_read(struct lcdreg *reg, unsigned regnr,
		struct lcdreg_transfer *transfer)
{
	int ret;

	if (WARN_ON_ONCE(!reg || !transfer))
		return -EINVAL;

	if (!reg->read)
		return -EOPNOTSUPP;

	if (!transfer->width)
		transfer->width = reg->def_width;

	dev_dbg(reg->dev,
		"lcdreg_read: regnr=0x%02x, index=%u, count=%u, width=%u\n",
		regnr, transfer->index, transfer->count, transfer->width);

	ret = reg->read(reg, regnr, transfer);

	lcdreg_dbg_transfer_buf(transfer);

	return ret;
}
EXPORT_SYMBOL(lcdreg_read);

/**
 * Read from LCD register into 32-bit wide buffer
 * @reg: LCD register
 * @regnr: Register number
 * @buf: Buffer to read into
 * @count: Number of words to read
 */
int lcdreg_readreg_buf32(struct lcdreg *reg, unsigned regnr, u32 *buf,
			 unsigned count)
{
	struct lcdreg_transfer tr = {
		.index = 1,
		.count = count,
	};
	int i, ret;

	if (!buf || !count)
		return -EINVAL;

	tr.buf = kmalloc_array(count, sizeof(*buf), GFP_KERNEL);
	if (!tr.buf)
		return -ENOMEM;

	ret = lcdreg_read(reg, regnr, &tr);
	if (ret) {
		kfree(tr.buf);
		return ret;
	}

	if (reg->def_width <= 8)
		for (i = 0; i < count; i++)
			buf[i] = ((u8 *)tr.buf)[i];
	else
		for (i = 0; i < count; i++)
			buf[i] = ((u16 *)tr.buf)[i];
	kfree(tr.buf);

	return ret;
}
EXPORT_SYMBOL(lcdreg_readreg_buf32);

static void devm_lcdreg_release(struct device *dev, void *res)
{
	struct lcdreg *reg = *(struct lcdreg **)res;

	lcdreg_debugfs_exit(reg);
	mutex_destroy(&reg->lock);
}

/**
 * Device managed lcdreg initialization
 *
 * @dev: Device backing the LCD register
 * @reg: LCD register
 */
struct lcdreg *devm_lcdreg_init(struct device *dev, struct lcdreg *reg)
{
	struct lcdreg **ptr;

	if (!dev || !reg)
		return ERR_PTR(-EINVAL);

	ptr = devres_alloc(devm_lcdreg_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	*ptr = reg;
	devres_add(dev, ptr);
	reg->dev = dev;
	mutex_init(&reg->lock);
	lcdreg_debugfs_init(reg);

	return reg;
}
EXPORT_SYMBOL(devm_lcdreg_init);

MODULE_LICENSE("GPL");
