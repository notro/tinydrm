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
 * - collect_dirty: Writing a positive number <n> to this file (re)starts the
 *                  process of collecting update statistics for the last <n>
 *                  updates. Writing a zero stops it.
 *
 * - dirty: Reading this file will provide a list of the last <n> updates.
 *          Reading will not clear the list.
 *
 * Example use:
 *     # cd /sys/kernel/debug/tinydrm/spi0.0
 *     # echo 4 > collect_dirty
 *     # cat dirty
 *     [ 2140.061740] 2798 KiB/s, 151 KiB in 54 ms,    full(320x240+0+0)
 *     [ 2140.161710] 2798 KiB/s, 151 KiB in 54 ms,    full(320x240+0+0),  99 ms since last, 10 fps
 *     [ 2140.301724] 2798 KiB/s, 151 KiB in 54 ms,    full(320x240+0+0), 140 ms since last,  7 fps
 *     [ 2140.361702] 3552 KiB/s,  10 KiB in  3 ms, partial(320x16+0+224),  59 ms since last
 *
 * To get this functionality the driver has to call devm_tinydrm_debugfs_init()
 * to set it up and then bracket the display update with calls to
 * tinydrm_debugfs_dirty_begin() and tinydrm_debugfs_dirty_end().
 *
 */

#define MAX_DIRTY_ENTRIES 40 /* PAGE_SIZE(4k) / 100(linelength) */

struct tinydrm_dirty_entry {
	struct list_head list;
	struct drm_clip_rect clip;
	bool full;
	size_t len;
	u64 start;
	u64 end;
};

struct tinydrm_debugfs_dirty {
	struct dentry *debugfs;
	struct list_head list;
	struct mutex list_lock;
};

static struct dentry *tinydrm_debugfs_root;

static struct tinydrm_dirty_entry *
tinydrm_debugfs_dirty_get_entry(struct tinydrm_debugfs_dirty *dirty)
{
	struct tinydrm_dirty_entry *entry;

	entry = list_last_entry(&dirty->list,
				struct tinydrm_dirty_entry, list);
	if (entry->start) {
		if (entry->end)
			return NULL; /* buffer is full */
		else
			return entry; /* in progress */
	}
	/* The buffer hasn't been filled yet */
	list_for_each_entry(entry, &dirty->list, list) {
		if (!entry->end)
			return entry;
	}

	WARN_ON(1);
	return NULL;
}

/**
 * tinydrm_debugfs_dirty_begin - Display update is starting
 * @tdev: tinydrm device
 * @fb: framebuffer
 * @clip: The part of the display that is to be updated.
 */
void tinydrm_debugfs_dirty_begin(struct tinydrm_device *tdev,
				 struct drm_framebuffer *fb,
				 const struct drm_clip_rect *clip)
{
	struct tinydrm_debugfs_dirty *dirty = tdev->debugfs_dirty;
	struct tinydrm_dirty_entry *entry;

	if (!dirty)
		return;

	mutex_lock(&dirty->list_lock);

	if (list_empty(&dirty->list))
		goto out_unlock;

	entry = tinydrm_debugfs_dirty_get_entry(dirty);
	if (!entry) {
		list_rotate_left(&dirty->list);
		entry = list_last_entry(&dirty->list,
					struct tinydrm_dirty_entry, list);
	}

	entry->clip = *clip;
	entry->full = clip->x1 == 0 && clip->x2 == fb->width &&
		      clip->y1 == 0 && clip->y2 == fb->height;
	entry->start = local_clock();
	entry->end = 0;

out_unlock:
	mutex_unlock(&dirty->list_lock);
}
EXPORT_SYMBOL(tinydrm_debugfs_dirty_begin);

/**
 * tinydrm_debugfs_dirty_end - Display update has ended
 * @tdev: tinydrm device
 * @len: Length of transfer buffer
 * @bits_per_pixel: Used to calculate transfer length if @len is zero by
 *                  multiplying with number of pixels in clip.
 */
void tinydrm_debugfs_dirty_end(struct tinydrm_device *tdev, size_t len,
			       unsigned bits_per_pixel)
{
	struct tinydrm_debugfs_dirty *dirty = tdev->debugfs_dirty;
	struct tinydrm_dirty_entry *entry;

	if (!dirty)
		return;

	mutex_lock(&dirty->list_lock);

	if (list_empty(&dirty->list))
		goto out_unlock;

	entry = tinydrm_debugfs_dirty_get_entry(dirty);
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
	mutex_unlock(&dirty->list_lock);
}
EXPORT_SYMBOL(tinydrm_debugfs_dirty_end);

static void *tinydrm_debugfs_dirty_seq_start(struct seq_file *s, loff_t *pos)
{
	return *pos ? NULL : SEQ_START_TOKEN;
}

static void *tinydrm_debugfs_dirty_seq_next(struct seq_file *s, void *v,
					    loff_t *pos)
{
	return NULL;
}

static void tinydrm_debugfs_dirty_seq_stop(struct seq_file *s, void *v)
{
}

static int tinydrm_debugfs_dirty_seq_show(struct seq_file *s, void *v)
{
	struct tinydrm_debugfs_dirty *dirty = s->private;
	struct tinydrm_dirty_entry *entry;
	u64 previous_start = 0;
	bool previous_full = false;

	mutex_lock(&dirty->list_lock);

	list_for_each_entry(entry, &dirty->list, list) {
		u32 start_rem_nsec, duration_ms, last_ms = 0;
		u64 start_sec, throughput;

		/* stop on empty entry (buffer not full nor empty) */
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

		seq_printf(s, ", %s(%ux%u+%u+%u)",
			   entry->full ? "   full" : "partial",
			   entry->clip.x2 - entry->clip.x1,
			   entry->clip.y2 - entry->clip.y1,
			   entry->clip.x1, entry->clip.y1);

		if (previous_start) {
			last_ms = div_u64(entry->start - previous_start,
					  1000000);
			seq_printf(s, ", %3u ms since last", last_ms);
		}

		if (entry->full && previous_full && last_ms)
			seq_printf(s, ", %2u fps", 1000 / last_ms);

		seq_puts(s, "\n");
		previous_start = entry->start;
		previous_full = entry->full;
	}

	mutex_unlock(&dirty->list_lock);

	return 0;
}

static const struct seq_operations tinydrm_debugfs_dirty_seq_ops = {
	.start = tinydrm_debugfs_dirty_seq_start,
	.next  = tinydrm_debugfs_dirty_seq_next,
	.stop  = tinydrm_debugfs_dirty_seq_stop,
	.show  = tinydrm_debugfs_dirty_seq_show
};

static int tinydrm_debugfs_dirty_open(struct inode *inode, struct file *file)
{
	struct drm_device *dev = inode->i_private;
	struct tinydrm_device *tdev = dev->dev_private;
	int ret;

	if (!tdev)
		return -ENODEV;

	ret = seq_open(file, &tinydrm_debugfs_dirty_seq_ops);
	((struct seq_file *)file->private_data)->private = tdev->debugfs_dirty;

	return ret;
}

static const struct file_operations tinydrm_debugfs_dirty_file_ops = {
	.owner   = THIS_MODULE,
	.open    = tinydrm_debugfs_dirty_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release
};

static void
tinydrm_debugfs_dirty_list_delete(struct tinydrm_debugfs_dirty *dirty)
{
	struct tinydrm_dirty_entry *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, &dirty->list, list) {
		list_del(&entry->list);
		kfree(entry);
	}
}

static int tinydrm_debugfs_collect_updates_set(void *data, u64 val)
{
	struct drm_device *dev = data;
	struct tinydrm_device *tdev = dev->dev_private;
	struct tinydrm_debugfs_dirty *dirty;
	struct tinydrm_dirty_entry *entry;
	int i, ret = 0;

	if (!tdev)
		return -ENODEV;

	dirty = tdev->debugfs_dirty;
	if (!dirty)
		return -ENODEV;

	if (val > MAX_DIRTY_ENTRIES)
		return -ERANGE;

	mutex_lock(&dirty->list_lock);

	if (!list_empty(&dirty->list))
		tinydrm_debugfs_dirty_list_delete(dirty);

	for (i = 0; i < val; i++) {
		entry = kzalloc(sizeof(*entry), GFP_KERNEL);
		if (!entry) {
			tinydrm_debugfs_dirty_list_delete(dirty);
			ret = -ENOMEM;
			break;
		}
		list_add(&entry->list, &dirty->list);
	}

	mutex_unlock(&dirty->list_lock);

	return ret;
}

DEFINE_SIMPLE_ATTRIBUTE(tinydrm_debugfs_collect_updates_fops, NULL,
			tinydrm_debugfs_collect_updates_set, "%llu\n");

static void tinydrm_debugfs_release(struct device *dev, void *res)
{
	struct tinydrm_device *tdev = *(struct tinydrm_device **)res;
	struct tinydrm_debugfs_dirty *dirty = tdev->debugfs_dirty;

	tinydrm_debugfs_dirty_list_delete(dirty);
	debugfs_remove_recursive(dirty->debugfs);
	kfree(dirty);
}

/**
 * devm_tinydrm_debugfs_init - Initialize performance reporting
 * @tdev: tinydrm device
 *
 * Resources will be automatically freed on driver detach (devres).
 */
void devm_tinydrm_debugfs_init(struct tinydrm_device *tdev)
{
	struct tinydrm_debugfs_dirty *dirty = NULL;
	struct drm_device *dev = tdev->base;
	struct tinydrm_device **ptr;
	struct dentry *dentry;

	if (IS_ERR_OR_NULL(tinydrm_debugfs_root))
		goto err_msg;

	dirty = kzalloc(sizeof(*dirty), GFP_KERNEL);
	if (!dirty)
		goto err_msg;

	mutex_init(&dirty->list_lock);
	INIT_LIST_HEAD(&dirty->list);
	tdev->debugfs_dirty = dirty;

	ptr = devres_alloc(tinydrm_debugfs_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		goto err_msg;

	dentry = debugfs_create_dir(dev_name(dev->dev), tinydrm_debugfs_root);
	if (!dentry)
		goto err_remove;

	if (!debugfs_create_file("collect_dirty", 0200, dentry, dev,
				 &tinydrm_debugfs_collect_updates_fops))
		goto err_remove;

	if (!debugfs_create_file("dirty", 0400, dentry, dev,
				 &tinydrm_debugfs_dirty_file_ops))
		goto err_remove;

	dirty->debugfs = dentry;
	*ptr = tdev;
	devres_add(dev->dev, ptr);

	return;

err_remove:
	debugfs_remove_recursive(dentry);
	devres_free(ptr);
err_msg:
	kfree(dirty);
	tdev->debugfs_dirty = NULL;
	dev_err(dev->dev, "Failed to create debugfs entries\n");
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
