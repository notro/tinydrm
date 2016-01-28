/*
 * lcdreg I2C support
 *
 * Copyright 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/tinydrm/lcdreg-i2c.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

struct lcdreg_i2c {
	struct lcdreg reg;
	struct i2c_client *client;
	struct gpio_desc *reset;
};

static inline struct lcdreg_i2c *to_lcdreg_i2c(struct lcdreg *reg)
{
	return reg ? container_of(reg, struct lcdreg_i2c, reg) : NULL;
}

static int lcdreg_i2c_send(struct i2c_client *client, unsigned index,
			   void *buf, size_t len)
{
	u8 *txbuf;
	int ret;

	txbuf = kmalloc(1 + len, GFP_KERNEL);
	if (!txbuf)
		return -ENOMEM;

	txbuf[0] = index ? 0x40 : 0x80;
	memcpy(&txbuf[1], buf, len);

	ret = i2c_master_send(client, txbuf, 1 + len);
	kfree(txbuf);

	return ret < 0 ? ret : 0;
}

static int lcdreg_i2c_write(struct lcdreg *reg, unsigned regnr,
			    struct lcdreg_transfer *transfer)
{
	struct lcdreg_i2c *i2c = to_lcdreg_i2c(reg);
	u8 regnr_buf[1] = { regnr };
	int ret;

	ret = lcdreg_i2c_send(i2c->client, 0, regnr_buf, 1);
	if (ret)
		return ret;

	if (!transfer || !transfer->count)
		return 0;

	if (WARN_ON(transfer->width != 8))
		return -EINVAL;

	return lcdreg_i2c_send(i2c->client, transfer->index, transfer->buf,
			       transfer->count);
}

static int lcdreg_i2c_read(struct lcdreg *reg, unsigned regnr,
			   struct lcdreg_transfer *transfer)
{
	struct lcdreg_i2c *i2c = to_lcdreg_i2c(reg);
	int ret;

	if (WARN_ON_ONCE(transfer->width != 8))
		return -EINVAL;

	if (!reg->readable)
		return -EACCES;

	ret = i2c_master_recv(i2c->client, transfer->buf, transfer->count);

	return ret < 0 ? ret : 0;
}

static void lcdreg_i2c_reset(struct lcdreg *reg)
{
	struct lcdreg_i2c *i2c = to_lcdreg_i2c(reg);

	if (!i2c->reset)
		return;

	gpiod_set_value_cansleep(i2c->reset, 0);
	msleep(20);
	gpiod_set_value_cansleep(i2c->reset, 1);
	msleep(120);
}

struct lcdreg *devm_lcdreg_i2c_init(struct i2c_client *client)
{
	struct lcdreg_i2c *i2c;

	i2c = devm_kzalloc(&client->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return ERR_PTR(-ENOMEM);

	i2c->reg.bits_per_word_mask = LCDREG_BPW_MASK(8);
	i2c->reg.readable = true;
	i2c->client = client;
	i2c->reset = devm_gpiod_get_optional(&client->dev, "reset",
					     GPIOD_OUT_HIGH);
	if (IS_ERR(i2c->reset))
		return ERR_PTR(PTR_ERR(i2c->reset));

	i2c->reg.write = lcdreg_i2c_write;
	i2c->reg.read = lcdreg_i2c_read;
	i2c->reg.reset = lcdreg_i2c_reset;

	return devm_lcdreg_init(&client->dev, &i2c->reg);
}
EXPORT_SYMBOL(devm_lcdreg_i2c_init);

MODULE_LICENSE("GPL");
