/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/tinydrm/tinydrm.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

/**
 * DOC: Performance reporting
 *
 * tinydrm can provide performance reporting when built with CONFIG_DEBUG_FS.
 * Each device gets a directory <debugfs>/tinydrm/<devname> which contains
 * two files:
 *
 * - collect_updates: Writing a positive number <n> to this file (re)starts the
 *                    process of collecting update statistics for the last <n>
 *                    updates. Writing a zero stops it.
 *
 * - updates: Reading this file will provide a list of the last <n> updates.
 *            Reading will not clear the list.
 *
 * Example use:
 *     # cd /sys/kernel/debug/tinydrm/spi0.0
 *     # echo 4 > collect_updates
 *     # cat updates
 *     [ 2140.061740] 2798 KiB/s, 151 KiB in 54 ms,    full(320x240+0+0)
 *     [ 2140.161710] 2798 KiB/s, 151 KiB in 54 ms,    full(320x240+0+0),  99 ms since last, 10 fps
 *     [ 2140.301724] 2798 KiB/s, 151 KiB in 54 ms,    full(320x240+0+0), 140 ms since last,  7 fps
 *     [ 2140.361702] 3552 KiB/s,  10 KiB in  3 ms, partial(320x16+0+224),  59 ms since last
 *
 * To get this functionality the driver has to call devm_tinydrm_debugfs_init()
 * to set it up and then bracket the display update with calls to
 * tinydrm_debugfs_update_begin() and tinydrm_debugfs_update_end().
 *
 */

#define MAX_UPDATE_ENTRIES 40 /* PAGE_SIZE(4k) / 100(linelength) */

struct tinydrm_update_entry {
	struct list_head list;
	struct drm_clip_rect clip;
	size_t len;
	u64 start;
	u64 end;
};

static struct dentry *tinydrm_debugfs_root;

static struct tinydrm_update_entry *
tinydrm_debugfs_update_get_entry(struct tinydrm_device *tdev)
{
	struct tinydrm_update_entry *entry;

	entry = list_last_entry(&tdev->update_list,
				struct tinydrm_update_entry, list);
	if (entry->start) {
		if (entry->end)
			return NULL; /* buffer is full */
		else
			return entry; /* in progress */
	}
	/* The buffer hasn't been filled yet */
	list_for_each_entry(entry, &tdev->update_list, list) {
		if (!entry->end)
			return entry;
	}

	WARN_ON(1);
	return NULL;
}

/**
 * tinydrm_debugfs_update_begin - Display update is starting
 * @tdev: tinydrm device
 * @clip: The part of the display that is to be updated.
 */
void tinydrm_debugfs_update_begin(struct tinydrm_device *tdev,
				  const struct drm_clip_rect *clip)
{
	struct tinydrm_update_entry *entry;

	mutex_lock(&tdev->update_list_lock);

	if (list_empty(&tdev->update_list))
		goto out_unlock;

	entry = tinydrm_debugfs_update_get_entry(tdev);
	if (!entry) {
		list_rotate_left(&tdev->update_list);
		entry = list_last_entry(&tdev->update_list,
					struct tinydrm_update_entry, list);
	}

	entry->clip = *clip;
	entry->start = local_clock();
	entry->end = 0;

out_unlock:
	mutex_unlock(&tdev->update_list_lock);
}
EXPORT_SYMBOL(tinydrm_debugfs_update_begin);

/**
 * tinydrm_debugfs_update_end - Display update has ended
 * @tdev: tinydrm device
 * @len: Length of transfer buffer
 * @bits_per_pixel: Used to calculate transfer length if @len is zero by
 *                  multiplying with number of pixels in clip.
 */
void tinydrm_debugfs_update_end(struct tinydrm_device *tdev, size_t len,
				unsigned bits_per_pixel)
{
	struct tinydrm_update_entry *entry;

	mutex_lock(&tdev->update_list_lock);

	if (list_empty(&tdev->update_list))
		goto out_unlock;

	entry = tinydrm_debugfs_update_get_entry(tdev);
	if (WARN_ON(!entry))
		goto out_unlock;

	if (!entry->start)
		goto out_unlock; /* enabled during an update */

	if (!len)
		len = (entry->clip.x2 - entry->clip.x1) *
		      (entry->clip.y2 - entry->clip.y1) *
		      bits_per_pixel / 8;
	entry->end = local_clock();
	entry->len = len;

out_unlock:
	mutex_unlock(&tdev->update_list_lock);
}
EXPORT_SYMBOL(tinydrm_debugfs_update_end);

static void *tinydrm_debugfs_update_seq_start(struct seq_file *s, loff_t *pos)
{
	return *pos ? NULL : SEQ_START_TOKEN;
}

static void *tinydrm_debugfs_update_seq_next(struct seq_file *s, void *v,
					     loff_t *pos)
{
	return NULL;
}

static void tinydrm_debugfs_update_seq_stop(struct seq_file *s, void *v)
{
}

static int tinydrm_debugfs_update_seq_show(struct seq_file *s, void *v)
{
	struct tinydrm_device *tdev = s->private;
	struct tinydrm_update_entry *entry;
	u64 previous_start = 0;

	mutex_lock(&tdev->update_list_lock);

	list_for_each_entry(entry, &tdev->update_list, list) {
		u32 start_rem_nsec, duration_ms, last_ms = 0;
		u64 start_sec, throughput;
		bool full;

		/* stop on empty entry (buffer not full or empty) */
		if (!entry->start)
			break;

		start_sec = div_u64_rem(entry->start, 1000000000,
					&start_rem_nsec);
		seq_printf(s, "[%5llu.%06u]", start_sec,
			   start_rem_nsec / 1000);

		if (!entry->end) {
			seq_puts(s, " update in progress\n");
			break;
		}

		if (entry->end <= entry->start) {
			seq_puts(s, " illegal entry\n");
			continue;
		}

		duration_ms = div_u64(entry->end - entry->start, 1000000);
		if (!duration_ms)
			duration_ms = 1;

		throughput = entry->len * 1000 / duration_ms / SZ_1K;
		seq_printf(s, " %5llu KiB/s", throughput);
		if (entry->len < SZ_4K)
			seq_printf(s, ", %4u bytes", entry->len);
		else
			seq_printf(s, ", %6u KiB", entry->len / SZ_1K);

		seq_printf(s, " in %3u ms", duration_ms);

		full = entry->clip.x1 == 0 &&
		       entry->clip.x2 == tdev->width &&
		       entry->clip.y1 == 0 &&
		       entry->clip.y2 == tdev->height;
		seq_printf(s, ", %s(%ux%u+%u+%u)", full ? "   full" : "partial",
			   entry->clip.x2 - entry->clip.x1,
			   entry->clip.y2 - entry->clip.y1,
			   entry->clip.x1, entry->clip.y1);

		if (previous_start) {
			last_ms = div_u64(entry->start - previous_start,
					  1000000);
			seq_printf(s, ", %3u ms since last", last_ms);
		}

		if (full && last_ms)
			seq_printf(s, ", %2u fps", 1000 / last_ms);

		seq_puts(s, "\n");
		previous_start = entry->start;
	}

	mutex_unlock(&tdev->update_list_lock);

	return 0;
}

static const struct seq_operations tinydrm_debugfs_update_seq_ops = {
	.start = tinydrm_debugfs_update_seq_start,
	.next  = tinydrm_debugfs_update_seq_next,
	.stop  = tinydrm_debugfs_update_seq_stop,
	.show  = tinydrm_debugfs_update_seq_show
};

static int tinydrm_debugfs_update_open(struct inode *inode, struct file *file)
{
	struct tinydrm_device *tdev = inode->i_private;
	int ret;

	ret = seq_open(file, &tinydrm_debugfs_update_seq_ops);
	((struct seq_file *)file->private_data)->private = tdev;

	return ret;
}

static const struct file_operations tinydrm_debugfs_update_file_ops = {
	.owner   = THIS_MODULE,
	.open    = tinydrm_debugfs_update_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release
};

static void tinydrm_debugfs_update_list_delete(struct tinydrm_device *tdev)
{
	struct tinydrm_update_entry *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, &tdev->update_list, list) {
		list_del(&entry->list);
		kfree(entry);
	}
}

static int tinydrm_debugfs_collect_updates_set(void *data, u64 val)
{
	struct tinydrm_update_entry *entry;
	struct tinydrm_device *tdev = data;
	int i, ret = 0;

	if (val > MAX_UPDATE_ENTRIES)
		return -ERANGE;

	mutex_lock(&tdev->update_list_lock);

	if (!list_empty(&tdev->update_list))
		tinydrm_debugfs_update_list_delete(tdev);

	for (i = 0; i < val; i++) {
		entry = kzalloc(sizeof(*entry), GFP_KERNEL);
		if (!entry) {
			tinydrm_debugfs_update_list_delete(tdev);
			ret = -ENOMEM;
			break;
		}
		list_add(&entry->list, &tdev->update_list);
	}

	mutex_unlock(&tdev->update_list_lock);

	return ret;
}

DEFINE_SIMPLE_ATTRIBUTE(tinydrm_debugfs_collect_updates_fops, NULL,
			tinydrm_debugfs_collect_updates_set, "%llu\n");

static void tinydrm_debugfs_release(struct device *dev, void *res)
{
	struct tinydrm_device *tdev = *(struct tinydrm_device **)res;

	tinydrm_debugfs_update_list_delete(tdev);
	debugfs_remove_recursive(tdev->debugfs);
}

/**
 * devm_tinydrm_debugfs_init - Initialize performance reporting
 * @tdev: tinydrm device
 *
 * Resources will be automatically freed on driver detach (devres).
 */
void devm_tinydrm_debugfs_init(struct tinydrm_device *tdev)
{
	struct device *dev = tdev->base->dev;
	struct tinydrm_device **ptr;
	struct dentry *dentry;

	mutex_init(&tdev->update_list_lock);
	INIT_LIST_HEAD(&tdev->update_list);

	if (IS_ERR_OR_NULL(tinydrm_debugfs_root))
		goto err_msg;

	ptr = devres_alloc(tinydrm_debugfs_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		goto err_msg;

	dentry = debugfs_create_dir(dev_name(dev), tinydrm_debugfs_root);
	if (!dentry)
		goto err_remove;

	if (!debugfs_create_file("collect_updates", 0200, dentry, tdev,
				 &tinydrm_debugfs_collect_updates_fops))
		goto err_remove;

	if (!debugfs_create_file("updates", 0400, dentry, tdev,
				 &tinydrm_debugfs_update_file_ops))
		goto err_remove;

	tdev->debugfs = dentry;
	*ptr = tdev;
	devres_add(dev, ptr);

	return;

err_remove:
	debugfs_remove_recursive(dentry);
	devres_free(ptr);
err_msg:
	dev_err(dev, "Failed to create debugfs entries\n");
}
EXPORT_SYMBOL(devm_tinydrm_debugfs_init);

static int tinydrm_debugfs_module_init(void)
{
	tinydrm_debugfs_root = debugfs_create_dir("tinydrm", NULL);
	if (IS_ERR_OR_NULL(tinydrm_debugfs_root))
		pr_err("tinydrm: Failed to create debugfs root\n");

	return 0;
}
module_init(tinydrm_debugfs_module_init);

static void tinydrm_debugfs_module_exit(void)
{
	debugfs_remove_recursive(tinydrm_debugfs_root);
}
module_exit(tinydrm_debugfs_module_exit);

MODULE_LICENSE("GPL");
