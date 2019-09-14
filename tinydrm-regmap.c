/*
 * Copyright 2017 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/regmap.h>

#include <drm/drmP.h>
#include <drm/tinydrm/tinydrm-helpers.h>
#include <drm/tinydrm/tinydrm-regmap.h>

/**
 * DOC: overview
 *
 */

/**
 * tinydrm_regmap_raw_swap_bytes - Does a raw write require swapping bytes?
 * @reg: Regmap
 *
 * If the bus doesn't support the full regwidth, it has to break up the word.
 * Additionally if the bus and machine doesn't match endian wise, this requires
 * byteswapping the buffer when using regmap_raw_write().
 *
 * Returns:
 * True if byte swapping is needed, otherwise false
 */
bool tinydrm_regmap_raw_swap_bytes(struct regmap *reg)
{
	int val_bytes = regmap_get_val_bytes(reg);
	unsigned int bus_val;
	u16 val16 = 0x00ff;

	if (val_bytes == 1)
		return false;

	if (WARN_ON_ONCE(val_bytes != 2))
		return false;

	regmap_parse_val(reg, &val16, &bus_val);

	return val16 != bus_val;
}
EXPORT_SYMBOL(tinydrm_regmap_raw_swap_bytes);

struct tinydrm_regmap_i80 {
	struct device *dev;
	struct regmap *reg;
	struct gpio_desc *cs;
	struct gpio_desc *idx;
	struct gpio_desc *wr;
	struct gpio_descs *db;
};

static void tinydrm_i80_write_value(struct gpio_desc *wr,
				    struct gpio_descs *db, u32 value)
{
	unsigned long value_bitmap = value;

	gpiod_set_value_cansleep(wr, 0);
	gpiod_set_array_value_cansleep(db->ndescs, db->desc, NULL, &value_bitmap);
	gpiod_set_value_cansleep(wr, 1);
}

static void tinydrm_i80_write_buf(struct gpio_desc *wr, struct gpio_descs *db,
				  const void *buf, size_t len)
{
	unsigned int width = db->ndescs;
	size_t i;

	if (width == 8) {
		const u8 *buf8 = buf;

		for (i = 0; i < len; i++)
			tinydrm_i80_write_value(wr, db, *buf8++);
	} else if (width == 16) {
		const u16 *buf16 = buf;

		for (i = 0; i < (len / 2); i++)
			tinydrm_i80_write_value(wr, db, *buf16++);
	} else {
		WARN_ON_ONCE(1);
	}
}

static int tinydrm_regmap_i80_gather_write(void *context, const void *reg,
					   size_t reg_len, const void *val,
					   size_t val_len)
{
	struct tinydrm_regmap_i80 *i80 = context;

	if (i80->cs)
		gpiod_set_value_cansleep(i80->cs, 0);

	if (i80->idx)
		gpiod_set_value_cansleep(i80->idx, 0);
	tinydrm_i80_write_buf(i80->wr, i80->db, reg, reg_len);

	if (i80->idx)
		gpiod_set_value_cansleep(i80->idx, 1);
	tinydrm_i80_write_buf(i80->wr, i80->db, val, val_len);

	if (i80->cs)
		gpiod_set_value_cansleep(i80->cs, 1);

	return 0;
}

static int tinydrm_regmap_i80_write(void *context, const void *data,
				    size_t count)
{
	struct tinydrm_regmap_i80 *i80 = context;
	size_t sz = regmap_get_val_bytes(i80->reg);

	return tinydrm_regmap_i80_gather_write(context, data, sz,
					       data + sz, count - sz);
}

static int tinydrm_regmap_i80_read(void *context, const void *reg,
				   size_t reg_len, void *val, size_t val_len)
{
	return -ENOTSUPP;
}

static const struct regmap_bus tinydrm_i80_bus = {
	.write = tinydrm_regmap_i80_write,
	.gather_write = tinydrm_regmap_i80_gather_write,
	.read = tinydrm_regmap_i80_read,
	.reg_format_endian_default = REGMAP_ENDIAN_BIG,
	.val_format_endian_default = REGMAP_ENDIAN_BIG,

// NATIVE

};

/**
 * tinydrm_i80_init - Initialize an I80 bus regmap
 * @dev: Device
 * @reg_width: Register width in bits (8 or 16).
 * @cs: Chip Select gpio (optional).
 * @idx: Index gpio, low writing register number and high writing value
 *       (optional).
 * @wr: Write latch gpio.
 * @db: Databus gpio array. The bus can be 8-bit wide even if the register is
 *      16-bit.
 *
 * This function creates a &regmap to access the register on a I80 type bus
 * connected controller.
 *
 * Returns I80 &regmap on success or ERR_PTR on failure.
 */
struct regmap *tinydrm_i80_init(struct device *dev, unsigned int reg_width,
				struct gpio_desc *cs, struct gpio_desc *idx,
				struct gpio_desc *wr, struct gpio_descs *db)
{
	struct tinydrm_regmap_i80 *i80;
	struct regmap_config config = {
		.reg_bits = reg_width,
		.val_bits = reg_width,
		.cache_type = REGCACHE_NONE,
	};

	if ((db->ndescs != 8 && db->ndescs != 16) ||
	    (reg_width != 8 && reg_width != 16))
		return ERR_PTR(-EINVAL);

	i80 = devm_kzalloc(dev, sizeof(*i80), GFP_KERNEL);
	if (!i80)
		return ERR_PTR(-ENOMEM);

	i80->dev = dev;
	i80->cs = cs;
	i80->idx = idx;
	i80->wr = wr;
	i80->db = db;
	i80->reg = devm_regmap_init(dev, &tinydrm_i80_bus, i80, &config);

	return i80->reg;
}
EXPORT_SYMBOL(tinydrm_i80_init);

#ifdef CONFIG_DEBUG_FS

static int
tinydrm_kstrtoul_array_from_user(const char __user *s, size_t count,
				 unsigned int base,
				 unsigned long *vals, size_t num_vals)
{
	char *buf, *pos, *token;
	int ret = -EINVAL, i = 0;

	buf = memdup_user_nul(s, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	pos = buf;
	while (pos) {
		if (i == num_vals) {
			ret = -E2BIG;
			goto err_free;
		}

		token = strsep(&pos, " ");
		if (!token) {
			ret = -EINVAL;
			goto err_free;
		}

		ret = kstrtoul(token, base, vals++);
		if (ret < 0)
			goto err_free;
		i++;
	}

err_free:
	kfree(buf);

	return ret ? ret : i;
}

static ssize_t tinydrm_regmap_debugfs_reg_write(struct file *file,
					        const char __user *user_buf,
					        size_t count, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct regmap *reg = m->private;
	unsigned long vals[2];
	int ret;

	ret = tinydrm_kstrtoul_array_from_user(user_buf, count, 16, vals, 2);
	if (ret <= 0)
		return ret;

	if (ret != 2)
		return -EINVAL;

	ret = regmap_write(reg, vals[0], vals[1]);

	return ret < 0 ? ret : count;
}

static int tinydrm_regmap_debugfs_reg_show(struct seq_file *m, void *d)
{
	struct regmap *reg = m->private;
	int max_reg = regmap_get_max_register(reg);
	int val_bytes = regmap_get_val_bytes(reg);
	unsigned int val;
	int regnr, ret;

	for (regnr = 0; regnr < max_reg; regnr++) {
		seq_printf(m, "%.*x: ", val_bytes * 2, regnr);
		ret = regmap_read(reg, regnr, &val);
		if (ret)
			seq_puts(m, "XX\n");
		else
			seq_printf(m, "%.*x\n", val_bytes * 2, val);
	}

	return 0;
}

static int tinydrm_regmap_debugfs_reg_open(struct inode *inode,
					   struct file *file)
{
	return single_open(file, tinydrm_regmap_debugfs_reg_show,
			   inode->i_private);
}

static const struct file_operations tinydrm_regmap_debugfs_reg_fops = {
	.owner = THIS_MODULE,
	.open = tinydrm_regmap_debugfs_reg_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = tinydrm_regmap_debugfs_reg_write,
};

int tinydrm_regmap_debugfs_init(struct regmap *reg, struct dentry *parent)
{
	umode_t mode = S_IFREG | S_IWUSR;

	if (regmap_get_max_register(reg))
		mode |= S_IRUGO;

	debugfs_create_file("registers", mode, parent, reg,
			    &tinydrm_regmap_debugfs_reg_fops);
	return 0;
}
EXPORT_SYMBOL(tinydrm_regmap_debugfs_init);

#endif
