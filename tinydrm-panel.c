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
#include <linux/dma-buf.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>

#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/tinydrm/tinydrm-helpers2.h>
#include <drm/tinydrm/tinydrm-regmap.h>
#include <drm/tinydrm/tinydrm-panel.h>

/**
 * DOC: overview
 *
 * This library provides helpers for
 */




static void tinydrm_panel_pipe_enable(struct drm_simple_display_pipe *pipe,
				      struct drm_crtc_state *crtc_state)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct tinydrm_panel *panel = to_tinydrm_panel(tdev);
//struct drm_framebuffer *fb = pipe->plane.fb;

	DRM_DEBUG_KMS("\n");


//panel->enabled = true;
//if (fb)
//	fb->funcs->dirty(fb, NULL, 0, 0, NULL, 0);


	if (panel->funcs && panel->funcs->enable)
		panel->funcs->enable(panel);
	else
		tinydrm_enable_backlight(panel->backlight);
}

static void tinydrm_panel_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct tinydrm_panel *panel = to_tinydrm_panel(tdev);

	DRM_DEBUG_KMS("\n");

//panel->enabled = false;

	if (panel->funcs && panel->funcs->disable)
		panel->funcs->enable(panel);
	else
		tinydrm_disable_backlight(panel->backlight);
}

static void tinydrm_panel_pipe_update(struct drm_simple_display_pipe *pipe,
				      struct drm_plane_state *old_state)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct tinydrm_panel *panel = to_tinydrm_panel(tdev);
	struct drm_framebuffer *fb = pipe->plane.state->fb;

	/* fb is set, not changed */
	if (fb && !old_state->fb && panel->funcs && panel->funcs->prepare)
		panel->funcs->prepare(panel);

	tinydrm_display_pipe_update(pipe, old_state);

	/* fb is unset */
	if (!fb && panel->funcs && panel->funcs->unprepare)
		panel->funcs->unprepare(panel);
}

static const struct drm_simple_display_pipe_funcs tinydrm_panel_pipe_funcs = {
	.enable = tinydrm_panel_pipe_enable,
	.disable = tinydrm_panel_pipe_disable,
	.update = tinydrm_panel_pipe_update,
	.prepare_fb = tinydrm_display_pipe_prepare_fb,
};

/**
 * tinydrm_panel_init - initialization XXXX
 * @dev: Parent device
 * @panel: &tinydrm_panel structure to initialize
 * @funcs: Callbacks for the panel (optional)
 * @formats: Array of supported formats (DRM_FORMAT\_\*)
 * @format_count: Number of elements in @formats
 * @fb_funcs: Framebuffer functions
 * @driver: DRM driver
 * @mode: Display mode
 * @rotation: Initial rotation in degrees Counter Clock Wise
 *
 * This function initializes a &tinydrm_panel structure and it's underlying
 * @tinydrm_device. It also sets up the display pipeline.
 *
 * Objects created by this function will be automatically freed on driver
 * detach (devres).
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int tinydrm_panel_init(struct device *dev, struct tinydrm_panel *panel,
			const struct tinydrm_panel_funcs *funcs,
			const uint32_t *formats, unsigned int format_count,
			const struct drm_framebuffer_funcs *fb_funcs,
		  	struct drm_driver *driver,
		  	const struct drm_display_mode *mode,
		  	unsigned int rotation)
{


size_t bufsize = mode->vdisplay * mode->hdisplay * sizeof(u16);


	struct tinydrm_device *tdev = &panel->tinydrm;
	const struct drm_format_info *format_info;
	int ret;

	panel->tx_buf = devm_kmalloc(dev, bufsize, GFP_KERNEL);
	if (!panel->tx_buf)
		return -ENOMEM;

	ret = devm_tinydrm_init(dev, tdev, fb_funcs, driver);
	if (ret)
		return ret;

	ret = tinydrm_display_pipe_init(tdev, &tinydrm_panel_pipe_funcs,
					DRM_MODE_CONNECTOR_VIRTUAL,
					formats, format_count, mode,
					rotation);
	if (ret)
		return ret;

	format_info = drm_format_info(formats[0]);
	tdev->drm->mode_config.preferred_depth = format_info->depth;

	panel->rotation = rotation;
	panel->funcs = funcs;

	drm_mode_config_reset(tdev->drm);

	DRM_DEBUG_KMS("preferred_depth=%u, rotation = %u\n",
		      tdev->drm->mode_config.preferred_depth, rotation);

	return 0;
}
EXPORT_SYMBOL(tinydrm_panel_init);

static int __maybe_unused tinydrm_panel_pm_suspend(struct device *dev)
{
	struct tinydrm_panel *panel = dev_get_drvdata(dev);
	int ret;

	ret = tinydrm_suspend(&panel->tinydrm);
	if (ret)
		return ret;

	/* fb isn't set to NULL by suspend, so do unprepare() explicitly */
	if (panel->funcs && panel->funcs->unprepare)
		return panel->funcs->unprepare(panel);

	return 0;
}

static int __maybe_unused tinydrm_panel_pm_resume(struct device *dev)
{
	struct tinydrm_panel *panel = dev_get_drvdata(dev);

	/* fb is NULL on resume, so prepare() will be called in pipe_update */

	return tinydrm_resume(&panel->tinydrm);
}

const struct dev_pm_ops tinydrm_panel_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(tinydrm_panel_pm_suspend,
				tinydrm_panel_pm_resume)
};
EXPORT_SYMBOL(tinydrm_panel_pm_ops);

/**
 * tinydrm_panel_spi_shutdown - tinydrm-panel SPI shutdown helper
 * @spi: SPI device
 *
 * tinydrm-panel drivers can use this as their shutdown callback to turn off
 * the display on machine shutdown and reboot. Use spi_set_drvdata() or
 * similar to set &tinydrm_panel as driver data.
 */
void tinydrm_panel_spi_shutdown(struct spi_device *spi)
{
	struct tinydrm_panel *panel = spi_get_drvdata(spi);

	tinydrm_shutdown(&panel->tinydrm);
}
EXPORT_SYMBOL(tinydrm_panel_spi_shutdown);

/**
 * tinydrm_panel_platform_shutdown - tinydrm-panel platform driver shutdown helper
 * @pdev: Platform device
 *
 * tinydrm-panel drivers can use this as their shutdown callback to turn off
 * the display on machine shutdown and reboot. Use platform_set_drvdata() or
 * similar to set &tinydrm_panel as driver data.
 */
void tinydrm_panel_platform_shutdown(struct platform_device *pdev)
{
	struct tinydrm_panel *panel = platform_get_drvdata(pdev);

	tinydrm_shutdown(&panel->tinydrm);
}
EXPORT_SYMBOL(tinydrm_panel_platform_shutdown);

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

#ifdef CONFIG_DEBUG_FS

static int
tinydrm_kstrtoul_array_from_user(const char __user *s, size_t count,
				 unsigned int base,
				 unsigned long *vals, size_t num_vals)
{
	char *buf, *pos, *token;
	int ret, i = 0;

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

static int
tinydrm_regmap_debugfs_init(struct regmap *reg, struct dentry *parent)
{
	umode_t mode = S_IFREG | S_IWUSR;

	if (regmap_get_max_register(reg))
		mode |= S_IRUGO;

	debugfs_create_file("registers", mode, parent, reg,
			    &tinydrm_regmap_debugfs_reg_fops);
	return 0;
}

static const struct drm_info_list tinydrm_panel_debugfslist[] = {
	{ "fb",   drm_fb_cma_debugfs_show, 0 },
};

/**
 * tinydrm_panel_debugfs_init - Create tinydrm panel debugfs entries
 * @minor: DRM minor
 *
 * &tinydrm_panel drivers can use this as their
 * &drm_driver->debugfs_init callback.
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int tinydrm_panel_debugfs_init(struct drm_minor *minor)
{
	struct tinydrm_device *tdev = minor->dev->dev_private;
	struct tinydrm_panel *panel = to_tinydrm_panel(tdev);
	struct regmap *reg = panel->reg;
	int ret;

	if (reg) {
		ret = tinydrm_regmap_debugfs_init(reg, minor->debugfs_root);
		if (ret)
			return ret;
	}

	return drm_debugfs_create_files(tinydrm_panel_debugfslist,
					ARRAY_SIZE(tinydrm_panel_debugfslist),
					minor->debugfs_root, minor);
}
EXPORT_SYMBOL(tinydrm_panel_debugfs_init);

#endif
