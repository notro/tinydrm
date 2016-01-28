/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/tinydrm/lcdreg.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define READ_RESULT_SIZE 16

static struct dentry *lcdreg_debugfs_root;

static int lcdreg_userbuf_to_u32(const char __user *user_buf, size_t count,
				 u32 *dest, size_t dest_size)
{
	char *buf, *start;
	int ret = -EINVAL;
	int i;

	buf = kmalloc(count, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, user_buf, count)) {
		kfree(buf);
		return -EFAULT;
	}

	/* turn whitespace into end-of-string for number parsing */
	for (i = 0; i < count; i++)
		if (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\t')
			buf[i] = '\0';

	i = 0;
	start = buf;
	while (start < buf + count) {
		/* skip "whitespace" */
		if (*start == '\0') {
			start++;
			continue;
		}

		if (i == dest_size) {
			ret = -EFBIG;
			break;
		}

		ret = kstrtou32(start, 0, &dest[i++]);
		if (ret)
			break;

		/* move past this number */
		while (*start != '\0')
			start++;
	};

	kfree(buf);

	return ret ? : i;
}

static ssize_t lcdreg_debugfs_write_file(struct file *file,
					 const char __user *user_buf,
					 size_t count, loff_t *ppos)
{
	struct lcdreg *reg = file->private_data;
	int ret;
	u32 txbuf[128];

	ret = lcdreg_userbuf_to_u32(user_buf, count, txbuf, ARRAY_SIZE(txbuf));
	if (ret < 0)
		return ret;

	mutex_lock(&reg->lock);
	ret = lcdreg_write_buf32(reg, txbuf[0], txbuf + 1, ret - 1);
	mutex_unlock(&reg->lock);

	return ret ? : count;
}

static const struct file_operations lcdreg_debugfs_write_fops = {
	.open = simple_open,
	.write = lcdreg_debugfs_write_file,
	.llseek = default_llseek,
};

static ssize_t lcdreg_debugfs_read_wr(struct file *file,
				      const char __user *user_buf,
				      size_t count, loff_t *ppos)
{
	struct lcdreg *reg = file->private_data;
	int ret;

	ret = lcdreg_userbuf_to_u32(user_buf, count,
				    &reg->debugfs_read_reg, 1);

	return ret < 0 ? ret : count;
}


static int lcdreg_debugfs_readreg(struct lcdreg *reg)
{
	struct lcdreg_transfer tr = {
		.index = 1,
		.width = reg->debugfs_read_width,
		.count = 1,
	};
	char *buf = reg->debugfs_read_result;
	int ret;

	tr.buf = kmalloc(lcdreg_bytes_per_word(tr.width), GFP_KERNEL);
	if (!tr.buf)
		return -ENOMEM;

	mutex_lock(&reg->lock);
	ret = lcdreg_read(reg, reg->debugfs_read_reg, &tr);
	mutex_unlock(&reg->lock);
	if (ret)
		goto error_out;

	switch (tr.width) {
	case 8:
		snprintf(buf, READ_RESULT_SIZE, "0x%02x\n", *(u8 *)tr.buf);
		break;
	case 16:
		snprintf(buf, READ_RESULT_SIZE, "0x%04x\n", *(u16 *)tr.buf);
		break;
	case 24:
	case 32:
		snprintf(buf, READ_RESULT_SIZE, "0x%08x\n", *(u32 *)tr.buf);
		break;
	default:
		ret = -EINVAL;
	}

error_out:
	kfree(tr.buf);

	return ret;
}

static ssize_t lcdreg_debugfs_read_rd(struct file *file,
				      char __user *user_buf,
				      size_t count, loff_t *ppos)
{
	struct lcdreg *reg = file->private_data;
	int ret;

	if (*ppos < 0 || !count)
		return -EINVAL;

	if (*reg->debugfs_read_result == '\0') {
		ret = lcdreg_debugfs_readreg(reg);
		if (ret)
			return ret;
	}

	if (*ppos >= strlen(reg->debugfs_read_result)) {
		*reg->debugfs_read_result = '\0';
		return 0;
	}

	return simple_read_from_buffer(user_buf, count, ppos,
				       reg->debugfs_read_result,
				       strlen(reg->debugfs_read_result));
}

static const struct file_operations lcdreg_debugfs_read_fops = {
	.open = simple_open,
	.read = lcdreg_debugfs_read_rd,
	.write = lcdreg_debugfs_read_wr,
	.llseek = default_llseek,
};

static ssize_t lcdreg_debugfs_reset_wr(struct file *file,
				       const char __user *user_buf,
				       size_t count, loff_t *ppos)
{
	struct lcdreg *reg = file->private_data;

	lcdreg_reset(reg);

	return count;
}

static const struct file_operations lcdreg_debugfs_reset_fops = {
	.open = simple_open,
	.write = lcdreg_debugfs_reset_wr,
	.llseek = default_llseek,
};

static int lcdreg_debugfs_readwidth_set(void *data, u64 val)
{
	struct lcdreg *reg = data;

	reg->debugfs_read_width = val;

	return 0;
}

static int lcdreg_debugfs_readwidth_get(void *data, u64 *val)
{
	struct lcdreg *reg = data;

	/*
	* def_width is not set when lcdreg_debugfs_init() is run, it's
	* set later by the controller init code. Hence the need for this
	* late assignment.
	*/
	if (!reg->debugfs_read_width)
		reg->debugfs_read_width = reg->def_width;

	*val = reg->debugfs_read_width;

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(lcdreg_debugfs_readwidth_fops,
			lcdreg_debugfs_readwidth_get,
			lcdreg_debugfs_readwidth_set, "%llu\n");

void lcdreg_debugfs_init(struct lcdreg *reg)
{
	if (IS_ERR_OR_NULL(lcdreg_debugfs_root))
		return;

	reg->debugfs_read_result = devm_kzalloc(reg->dev, READ_RESULT_SIZE,
						GFP_KERNEL);
	if (!reg->debugfs_read_result)
		return;

	reg->debugfs = debugfs_create_dir(dev_name(reg->dev),
					  lcdreg_debugfs_root);
	if (!reg->debugfs) {
		dev_warn(reg->dev, "Failed to create debugfs directory\n");
		return;
	}

	debugfs_create_file("write", 0220, reg->debugfs, reg,
			    &lcdreg_debugfs_write_fops);
	if (reg->read) {
		debugfs_create_file("read_width", 0660, reg->debugfs, reg,
				    &lcdreg_debugfs_readwidth_fops);
		debugfs_create_file("read", 0660, reg->debugfs, reg,
				    &lcdreg_debugfs_read_fops);
	}
	if (reg->reset) {
		debugfs_create_file("reset", 0220, reg->debugfs, reg,
				    &lcdreg_debugfs_reset_fops);
	}
}

void lcdreg_debugfs_exit(struct lcdreg *reg)
{
	debugfs_remove_recursive(reg->debugfs);
}

static int lcdreg_debugfs_module_init(void)
{
	lcdreg_debugfs_root = debugfs_create_dir("lcdreg", NULL);
	if (!lcdreg_debugfs_root)
		pr_warn("lcdreg: Failed to create debugfs root\n");

	return 0;
}
module_init(lcdreg_debugfs_module_init);

static void lcdreg_debugfs_module_exit(void)
{
	debugfs_remove_recursive(lcdreg_debugfs_root);
}
module_exit(lcdreg_debugfs_module_exit);

MODULE_LICENSE("GPL");
