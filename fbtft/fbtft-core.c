/*
 * Copyright (C) 2013 Noralf Tronnes
 *
 * This driver is inspired by:
 *   st7735fb.c, Copyright (C) 2011, Matt Porter
 *   broadsheetfb.c, Copyright (C) 2008, Jaya Kumar
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/string.h>
#include <video/mipi_display.h>

#include "fbtft.h"

static bool no_set_var;
module_param(no_set_var, bool, 0000);
MODULE_PARM_DESC(no_set_var, "Don't use fbtft_ops.set_var()");

static inline struct fbtft_par *
fbtft_par_from_tinydrm(struct tinydrm_device *tdev)
{
	return container_of(tdev, struct fbtft_par, tinydrm);
}

void fbtft_dbg_hex(const struct device *dev, int groupsize,
			void *buf, size_t len, const char *fmt, ...)
{
	va_list args;
	static char textbuf[512];
	char *text = textbuf;
	size_t text_len;

	va_start(args, fmt);
	text_len = vscnprintf(text, sizeof(textbuf), fmt, args);
	va_end(args);

	hex_dump_to_buffer(buf, len, 32, groupsize, text + text_len,
				512 - text_len, false);

	if (len > 32)
		dev_info(dev, "%s ...\n", text);
	else
		dev_info(dev, "%s\n", text);
}
EXPORT_SYMBOL(fbtft_dbg_hex);

static int fbtft_request_one_gpio(struct fbtft_par *par,
				  const char *name, int index, int *gpiop,
				  enum gpiod_flags flags)
{
	struct device *dev = par->info->device;
	struct gpio_desc *desc;
	int ret;

	desc = devm_gpiod_get_index_optional(dev, name, index, flags);
	if (!desc)
		return 0;

	if (IS_ERR(desc)) {
		ret = PTR_ERR(desc);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "gpiod_get('%s') failed with %d\n",
				name, ret);
		return ret;
	}

	if (gpiod_is_active_low(desc))
		gpiod_set_value_cansleep(desc, flags & GPIOD_OUT_LOW ? 1 : 0);

	*gpiop = desc_to_gpio(desc);
	DRM_DEBUG_DRIVER("'%s' = GPIO%d\n", name, *gpiop);

	return 0;
}

static int fbtft_request_gpios(struct fbtft_par *par)
{
	int i, ret;

	ret = fbtft_request_one_gpio(par, "reset", 0, &par->gpio.reset,
				     GPIOD_OUT_LOW);
	if (ret)
		return ret;

	ret = fbtft_request_one_gpio(par, "dc", 0, &par->gpio.dc,
				     GPIOD_OUT_LOW);
	if (ret)
		return ret;

	ret = fbtft_request_one_gpio(par, "rd", 0, &par->gpio.rd,
				     GPIOD_OUT_HIGH);
	if (ret)
		return ret;

	ret = fbtft_request_one_gpio(par, "wr", 0, &par->gpio.wr,
				     GPIOD_OUT_HIGH);
	if (ret)
		return ret;

	ret = fbtft_request_one_gpio(par, "cs", 0, &par->gpio.cs,
				     GPIOD_OUT_HIGH);
	if (ret)
		return ret;

	for (i = 0; i < 16; i++) {
		ret = fbtft_request_one_gpio(par, "db", i, &par->gpio.db[i],
					     GPIOD_OUT_LOW);
		if (ret)
			return ret;

		ret = fbtft_request_one_gpio(par, "led", i, &par->gpio.led[i],
					     GPIOD_OUT_LOW);
		if (ret)
			return ret;
	}

	return 0;
}

#ifdef CONFIG_FB_BACKLIGHT
static int fbtft_backlight_update_status(struct backlight_device *bl)
{
	struct fbtft_par *par = bl_get_data(bl);
	int brightness = bl->props.brightness;
	bool polarity = !!(bl->props.state & BL_CORE_DRIVER1);

	if (bl->props.power != FB_BLANK_UNBLANK ||
	    bl->props.fb_blank != FB_BLANK_UNBLANK ||
	    bl->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
		brightness = 0;

	gpio_set_value_cansleep(par->gpio.led[0],
				brightness ? polarity : !polarity);

	return 0;
}

static int fbtft_backlight_get_brightness(struct backlight_device *bl)
{
	return bl->props.brightness;
}

void fbtft_unregister_backlight(struct fbtft_par *par)
{
	if (par->info->bl_dev) {
		par->info->bl_dev->props.state |= BL_CORE_FBBLANK;
		backlight_update_status(par->info->bl_dev);
		backlight_device_unregister(par->info->bl_dev);
		par->info->bl_dev = NULL;
	}
}

static const struct backlight_ops fbtft_bl_ops = {
	.get_brightness	= fbtft_backlight_get_brightness,
	.update_status	= fbtft_backlight_update_status,
};

void fbtft_register_backlight(struct fbtft_par *par)
{
	struct backlight_device *bl;
	struct backlight_properties bl_props = { 0, };

	if (par->gpio.led[0] == -1) {
		fbtft_par_dbg(DEBUG_BACKLIGHT, par,
			"%s(): led pin not set, exiting.\n", __func__);
		return;
	}

	bl_props.type = BACKLIGHT_RAW;
	bl_props.brightness = 1;
	/* Assume backlight is off, get polarity from current state of pin */
	bl_props.state = BL_CORE_FBBLANK;
	if (!gpio_get_value(par->gpio.led[0]))
		bl_props.state |= BL_CORE_DRIVER1;

	bl = backlight_device_register(dev_driver_string(par->info->device),
				       par->info->device, par,
				       &fbtft_bl_ops, &bl_props);
	if (IS_ERR(bl)) {
		dev_err(par->info->device,
			"cannot register backlight device (%ld)\n",
			PTR_ERR(bl));
		return;
	}
	par->info->bl_dev = bl;

	if (!par->fbtftops.unregister_backlight)
		par->fbtftops.unregister_backlight = fbtft_unregister_backlight;
}
#else
void fbtft_register_backlight(struct fbtft_par *par) { };
void fbtft_unregister_backlight(struct fbtft_par *par) { };
#endif
EXPORT_SYMBOL(fbtft_register_backlight);
EXPORT_SYMBOL(fbtft_unregister_backlight);

static void fbtft_reset(struct fbtft_par *par)
{
	if (par->gpio.reset == -1)
		return;
	fbtft_par_dbg(DEBUG_RESET, par, "%s()\n", __func__);
	gpio_set_value_cansleep(par->gpio.reset, 0);
	usleep_range(20, 40);
	gpio_set_value_cansleep(par->gpio.reset, 1);
	msleep(120);
}

static int fbtft_verify_gpios(struct fbtft_par *par)
{
	int i;

	fbtft_par_dbg(DEBUG_VERIFY_GPIOS, par, "%s()\n", __func__);

	if (par->display.buswidth != 9 && par->startbyte == 0 &&
							par->gpio.dc < 0) {
		dev_err(par->info->device,
			"Missing info about 'dc' gpio. Aborting.\n");
		return -EINVAL;
	}

	if (!par->pdev)
		return 0;

	if (par->gpio.wr < 0) {
		dev_err(par->info->device, "Missing 'wr' gpio. Aborting.\n");
		return -EINVAL;
	}
	for (i = 0; i < par->display.buswidth; i++) {
		if (par->gpio.db[i] < 0) {
			dev_err(par->info->device,
				"Missing 'db%02d' gpio. Aborting.\n", i);
			return -EINVAL;
		}
	}

	return 0;
}

#ifdef CONFIG_OF
/**
 * fbtft_init_display_dt() - Device Tree init_display() function
 * @par: Driver data
 *
 * Return: 0 if successful, negative if error
 */
static int fbtft_init_display_dt(struct fbtft_par *par)
{
	struct device_node *node = par->info->device->of_node;
	struct property *prop;
	const __be32 *p;
	u32 val;
	int buf[64], i, j;

	if (!node)
		return -EINVAL;

	prop = of_find_property(node, "init", NULL);
	p = of_prop_next_u32(prop, NULL, &val);
	if (!p)
		return -EINVAL;

	par->fbtftops.reset(par);
	if (par->gpio.cs != -1)
		gpio_set_value(par->gpio.cs, 0);  /* Activate chip */

	while (p) {
		if (val & FBTFT_OF_INIT_CMD) {
			val &= 0xFFFF;
			i = 0;
			while (p && !(val & 0xFFFF0000)) {
				if (i > 63) {
					dev_err(par->info->device,
					"%s: Maximum register values exceeded\n",
					__func__);
					return -EINVAL;
				}
				buf[i++] = val;
				p = of_prop_next_u32(prop, p, &val);
			}
			/* make debug message */
			fbtft_par_dbg(DEBUG_INIT_DISPLAY, par,
				"init: write_register:\n");
			for (j = 0; j < i; j++)
				fbtft_par_dbg(DEBUG_INIT_DISPLAY, par,
					      "buf[%d] = %02X\n", j, buf[j]);

			par->fbtftops.write_register(par, i,
				buf[0], buf[1], buf[2], buf[3],
				buf[4], buf[5], buf[6], buf[7],
				buf[8], buf[9], buf[10], buf[11],
				buf[12], buf[13], buf[14], buf[15],
				buf[16], buf[17], buf[18], buf[19],
				buf[20], buf[21], buf[22], buf[23],
				buf[24], buf[25], buf[26], buf[27],
				buf[28], buf[29], buf[30], buf[31],
				buf[32], buf[33], buf[34], buf[35],
				buf[36], buf[37], buf[38], buf[39],
				buf[40], buf[41], buf[42], buf[43],
				buf[44], buf[45], buf[46], buf[47],
				buf[48], buf[49], buf[50], buf[51],
				buf[52], buf[53], buf[54], buf[55],
				buf[56], buf[57], buf[58], buf[59],
				buf[60], buf[61], buf[62], buf[63]);
		} else if (val & FBTFT_OF_INIT_DELAY) {
			fbtft_par_dbg(DEBUG_INIT_DISPLAY, par,
				"init: msleep(%u)\n", val & 0xFFFF);
			msleep(val & 0xFFFF);
			p = of_prop_next_u32(prop, p, &val);
		} else {
			dev_err(par->info->device, "illegal init value 0x%X\n",
									val);
			return -EINVAL;
		}
	}

	return 0;
}
#else
static int fbtft_init_display_dt(struct fbtft_par *par)
{
	return -EINVAL;
}
#endif

/**
 * fbtft_init_display() - Generic init_display() function
 * @par: Driver data
 *
 * Uses par->init_sequence to do the initialization
 *
 * Return: 0 if successful, negative if error
 */
static int fbtft_init_display(struct fbtft_par *par)
{
	int buf[64];
	char msg[128];
	char str[16];
	int i = 0;
	int j;

	/* sanity check */
	if (!par->init_sequence) {
		dev_err(par->info->device,
			"error: init_sequence is not set\n");
		return -EINVAL;
	}

	/* make sure stop marker exists */
	for (i = 0; i < FBTFT_MAX_INIT_SEQUENCE; i++)
		if (par->init_sequence[i] == -3)
			break;
	if (i == FBTFT_MAX_INIT_SEQUENCE) {
		dev_err(par->info->device,
			"missing stop marker at end of init sequence\n");
		return -EINVAL;
	}

	par->fbtftops.reset(par);
	if (par->gpio.cs != -1)
		gpio_set_value(par->gpio.cs, 0);  /* Activate chip */

	i = 0;
	while (i < FBTFT_MAX_INIT_SEQUENCE) {
		if (par->init_sequence[i] == -3) {
			/* done */
			return 0;
		}
		if (par->init_sequence[i] >= 0) {
			dev_err(par->info->device,
				"missing delimiter at position %d\n", i);
			return -EINVAL;
		}
		if (par->init_sequence[i + 1] < 0) {
			dev_err(par->info->device,
				"missing value after delimiter %d at position %d\n",
				par->init_sequence[i], i);
			return -EINVAL;
		}
		switch (par->init_sequence[i]) {
		case -1:
			i++;
			/* make debug message */
			strcpy(msg, "");
			j = i + 1;
			while (par->init_sequence[j] >= 0) {
				sprintf(str, "0x%02X ", par->init_sequence[j]);
				strcat(msg, str);
				j++;
			}
			fbtft_par_dbg(DEBUG_INIT_DISPLAY, par,
				"init: write(0x%02X) %s\n",
				par->init_sequence[i], msg);

			/* Write */
			j = 0;
			while (par->init_sequence[i] >= 0) {
				if (j > 63) {
					dev_err(par->info->device,
					"%s: Maximum register values exceeded\n",
					__func__);
					return -EINVAL;
				}
				buf[j++] = par->init_sequence[i++];
			}
			par->fbtftops.write_register(par, j,
				buf[0], buf[1], buf[2], buf[3],
				buf[4], buf[5], buf[6], buf[7],
				buf[8], buf[9], buf[10], buf[11],
				buf[12], buf[13], buf[14], buf[15],
				buf[16], buf[17], buf[18], buf[19],
				buf[20], buf[21], buf[22], buf[23],
				buf[24], buf[25], buf[26], buf[27],
				buf[28], buf[29], buf[30], buf[31],
				buf[32], buf[33], buf[34], buf[35],
				buf[36], buf[37], buf[38], buf[39],
				buf[40], buf[41], buf[42], buf[43],
				buf[44], buf[45], buf[46], buf[47],
				buf[48], buf[49], buf[50], buf[51],
				buf[52], buf[53], buf[54], buf[55],
				buf[56], buf[57], buf[58], buf[59],
				buf[60], buf[61], buf[62], buf[63]);
			break;
		case -2:
			i++;
			fbtft_par_dbg(DEBUG_INIT_DISPLAY, par,
				"init: mdelay(%d)\n", par->init_sequence[i]);
			mdelay(par->init_sequence[i++]);
			break;
		default:
			dev_err(par->info->device,
				"unknown delimiter %d at position %d\n",
				par->init_sequence[i], i);
			return -EINVAL;
		}
	}

	dev_err(par->info->device,
		"%s: something is wrong. Shouldn't get here.\n", __func__);
	return -EINVAL;
}

static void fbtft_set_addr_win(struct fbtft_par *par, int xs, int ys, int xe,
			       int ye)
{
	write_reg(par, MIPI_DCS_SET_COLUMN_ADDRESS,
		  (xs >> 8) & 0xFF, xs & 0xFF, (xe >> 8) & 0xFF, xe & 0xFF);

	write_reg(par, MIPI_DCS_SET_PAGE_ADDRESS,
		  (ys >> 8) & 0xFF, ys & 0xFF, (ye >> 8) & 0xFF, ye & 0xFF);

	write_reg(par, MIPI_DCS_WRITE_MEMORY_START);
}

static int fbtft_update_display(struct fbtft_par *par, unsigned int start_line,
				unsigned int end_line)
{
	size_t offset = start_line * par->info->fix.line_length;
	size_t len = (end_line - start_line + 1) * par->info->fix.line_length;

	par->fbtftops.set_addr_win(par, 0, start_line,
				   par->info->var.xres - 1, end_line);

	return par->fbtftops.write_vmem(par, offset, len);
}

static int fbtft_fb_dirty(struct drm_framebuffer *fb,
			  struct drm_file *file_priv,
			  unsigned int flags, unsigned int color,
			  struct drm_clip_rect *clips,
			  unsigned int num_clips)
{
	struct drm_gem_cma_object *cma_obj = drm_fb_cma_get_gem_obj(fb, 0);
	struct tinydrm_device *tdev = fb->dev->dev_private;
	struct fbtft_par *par = fbtft_par_from_tinydrm(tdev);
	bool mipi = !par->fbtftops.set_addr_win;
	struct drm_clip_rect fullclip = {
		.x1 = 0,
		.x2 = fb->width,
		.y1 = 0,
		.y2 = fb->height,
	};
	struct drm_clip_rect clip;
	int ret = 0;

	mutex_lock(&tdev->dirty_lock);

	/* fbdev can flush even when we're not interested */
	if (tdev->pipe.plane.fb != fb)
		goto out_unlock;

	tinydrm_merge_clips(&clip, clips, num_clips, flags,
			    fb->width, fb->height);

	/*
	 * MIPI is the default controller type supported by fbtft and it can
	 * handle clips that are not full width.
	 */
	if (!mipi) {
		clip.x1 = 0;
		clip.x2 = fb->width;
	}

	DRM_DEBUG("Flushing [FB:%d] x1=%u, x2=%u, y1=%u, y2=%u\n", fb->base.id,
		  clip.x1, clip.x2, clip.y1, clip.y2);

	/*
	 * tinydrm framebuffers are backed by write-combined memory with
	 * uncached reads. For simplicity copy the entire buffer to memory
	 * that has cacheable reads. We have to copy everything because of the
	 * way .write_mem() is implemented with offset into the buffer.
	 * This puts a penalty on displays connected to a DMA capable SPI
	 * controller that supports 16-bit words, since in that case the buffer
	 * will be passed straight through without being read by the CPU.
	 *
	 * Since MIPI controllers are the fbtft default, we can easily copy
	 * just the clip part of the buffer.
	 */
	switch (fb->format->format) {
	case DRM_FORMAT_RGB565:
		tinydrm_memcpy(par->info->screen_buffer, cma_obj->vaddr, fb,
			       mipi ? &clip : &fullclip);
		break;
	case DRM_FORMAT_XRGB8888:
		tinydrm_xrgb8888_to_rgb565(par->info->screen_buffer,
					   cma_obj->vaddr, fb,
					   mipi ? &clip : &fullclip, false);
		break;
	}

	if (mipi) {
		fbtft_set_addr_win(par, clip.x1, clip.y1, clip.x2 - 1, clip.y2 - 1);
		ret = par->fbtftops.write_vmem(par, 0, (clip.x2 - clip.x1) *
					       (clip.y2 - clip.y1) * 2);
	} else {
		ret = fbtft_update_display(par, clip.y1, clip.y2 - 1);
	}

out_unlock:
	mutex_unlock(&tdev->dirty_lock);

	if (ret)
		dev_err_once(fb->dev->dev, "Failed to update display %d\n",
			     ret);

	return ret;
}

static const struct drm_framebuffer_funcs fbtft_fb_funcs = {
	.destroy	= drm_fb_cma_destroy,
	.create_handle	= drm_fb_cma_create_handle,
	.dirty		= fbtft_fb_dirty,
};

static void fbtft_pipe_enable(struct drm_simple_display_pipe *pipe,
			      struct drm_crtc_state *crtc_state)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct fbtft_par *par = fbtft_par_from_tinydrm(tdev);
	struct drm_framebuffer *fb = pipe->plane.fb;

	DRM_DEBUG_KMS("\n");

	if (fb)
		fb->funcs->dirty(fb, NULL, 0, 0, NULL, 0);

	tinydrm_enable_backlight(par->info->bl_dev);
}

static void fbtft_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct fbtft_par *par = fbtft_par_from_tinydrm(tdev);

	DRM_DEBUG_KMS("\n");
	tinydrm_disable_backlight(par->info->bl_dev);
}

static const uint32_t fbtft_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

static const struct drm_simple_display_pipe_funcs fbtft_pipe_funcs = {
	.enable = fbtft_pipe_enable,
	.disable = fbtft_pipe_disable,
	.update = tinydrm_display_pipe_update,
	.prepare_fb = tinydrm_display_pipe_prepare_fb,
};

static struct drm_driver fbtft_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME |
				  DRIVER_ATOMIC,
	TINYDRM_GEM_DRIVER_OPS,
	.lastclose		= tinydrm_lastclose,
	.date			= "20170202",
	.major			= 1,
	.minor			= 0,
};

static int fbtft_property_unsigned(struct device *dev, const char *propname,
				   unsigned int *val)
{
	u32 val32;
	int ret;

	if (!device_property_present(dev, propname))
		return 0;

	ret = device_property_read_u32(dev, propname, &val32);
	if (ret)
		return ret;

	*val = val32;

	return 0;
}

int fbtft_probe_common(struct fbtft_display *display,
			struct spi_device *sdev, struct platform_device *pdev)
{
	unsigned int startbyte = 0, rotate = 0;
	unsigned long *gamma_curves = NULL;
	unsigned int txbuflen = 0;
	unsigned int vmem_size;
	struct fbtft_par *par;
	struct device *dev;
	int ret, i;

	DRM_DEBUG_DRIVER("\n");

	if (sdev)
		dev = &sdev->dev;
	else
		dev = &pdev->dev;

	if (display->gamma_num * display->gamma_len >
			FBTFT_GAMMA_MAX_VALUES_TOTAL) {
		dev_err(dev, "FBTFT_GAMMA_MAX_VALUES_TOTAL=%d is exceeded\n",
			FBTFT_GAMMA_MAX_VALUES_TOTAL);
		return -EINVAL;
	}

	par = devm_kzalloc(dev, sizeof(*par), GFP_KERNEL);
	if (!par)
		return -ENOMEM;

	par->buf = devm_kzalloc(dev, 128, GFP_KERNEL);
	if (!par->buf)
		return -ENOMEM;

	par->spi = sdev;
	par->pdev = pdev;

	/* make a copy that we can modify */
	par->display = *display;
	display = &par->display;

	if (!display->fps)
		display->fps = 20;
	if (!display->bpp)
		display->bpp = 16;

	if (display->bpp != 16) {
		dev_err(dev, "Only bpp=16 is supported\n");
		return -EINVAL;
	}

	ret = fbtft_property_unsigned(dev, "width", &display->width);
	if (ret)
		return ret;

	ret = fbtft_property_unsigned(dev, "height", &display->height);
	if (ret)
		return ret;

	ret = fbtft_property_unsigned(dev, "regwidth", &display->regwidth);
	if (ret)
		return ret;

	ret = fbtft_property_unsigned(dev, "buswidth", &display->buswidth);
	if (ret)
		return ret;

	ret = fbtft_property_unsigned(dev, "backlight", &display->backlight);
	if (ret)
		return ret;

	ret = fbtft_property_unsigned(dev, "rotate", &rotate);
	if (ret)
		return ret;

	par->bgr = device_property_present(dev, "bgr");

	ret = fbtft_property_unsigned(dev, "txbuflen", &txbuflen);
	if (ret)
		return ret;

	ret = fbtft_property_unsigned(dev, "startbyte", &startbyte);
	if (ret)
		return ret;

	par->startbyte = startbyte;

	if (device_property_present(dev, "gamma")) {
		ret = device_property_read_string(dev, "gamma", (const char **)&display->gamma);
		if (ret)
			return ret;
	}

	if (of_find_property(dev->of_node, "led-gpios", NULL))
		display->backlight = 1;

	if (!display->buswidth) {
		dev_err(dev, "buswidth is not set\n");
		return -EINVAL;
	}

	/* Needed by fb_uc1611 and fb_ssd1351 */
	par->pdata = devm_kzalloc(dev, sizeof(*par->pdata), GFP_KERNEL);
	if (!par->pdata)
		return -ENOMEM;

	par->pdata->display = *display;

	spin_lock_init(&par->dirty_lock);
	par->init_sequence = display->init_sequence;

	if (display->gamma_num && display->gamma_len) {
		gamma_curves = devm_kcalloc(dev,
					    display->gamma_num *
					    display->gamma_len,
					    sizeof(gamma_curves[0]),
					    GFP_KERNEL);
		if (!gamma_curves)
			return -ENOMEM;
	}

	mutex_init(&par->gamma.lock);
	par->gamma.curves = gamma_curves;
	par->gamma.num_curves = display->gamma_num;
	par->gamma.num_values = display->gamma_len;
	if (par->gamma.curves && display->gamma) {
		if (fbtft_gamma_parse_str(par, par->gamma.curves,
		    display->gamma, strlen(display->gamma)))
			return -ENOMEM;
	}

	/* Initialize gpios to disabled */
	par->gpio.reset = -1;
	par->gpio.dc = -1;
	par->gpio.rd = -1;
	par->gpio.wr = -1;
	par->gpio.cs = -1;
	for (i = 0; i < 16; i++) {
		par->gpio.db[i] = -1;
		par->gpio.led[i] = -1;
	}

	/* Satisfy fb_ra8875 and fb_ssd1331 */
	if (drm_debug & DRM_UT_DRIVER)
		par->debug = DEBUG_WRITE_REGISTER;

	vmem_size = display->width * display->height * display->bpp / 8;

	/* special case used in fb_uc1611 */
	if (!txbuflen && display->txbuflen == -1)
		txbuflen = vmem_size + 2; /* add in case startbyte is used */

	/* Transmit buffer */
	if (!txbuflen)
		txbuflen = display->txbuflen;
	if (txbuflen > vmem_size + 2)
		txbuflen = vmem_size + 2;

#ifdef __LITTLE_ENDIAN
	if (!txbuflen && (display->bpp > 8))
		txbuflen = PAGE_SIZE; /* need buffer for byteswapping */
#endif

	if (txbuflen) {
		par->txbuf.len = txbuflen;
		par->txbuf.buf = devm_kzalloc(dev, txbuflen, GFP_KERNEL);
		if (!par->txbuf.buf)
			return -ENOMEM;
	}

	par->fbtftops = display->fbtftops;

	if (!par->fbtftops.reset)
		par->fbtftops.reset = fbtft_reset;

	if (!par->fbtftops.register_backlight && display->backlight)
		par->fbtftops.register_backlight = fbtft_register_backlight;

	if (!par->fbtftops.write_register) {
		if (display->regwidth == 8 && display->buswidth == 8)
			par->fbtftops.write_register = fbtft_write_reg8_bus8;
		else if (display->regwidth == 8 && display->buswidth == 9)
			par->fbtftops.write_register = fbtft_write_reg8_bus9;
		else if (display->regwidth == 16 && display->buswidth == 8)
			par->fbtftops.write_register = fbtft_write_reg16_bus8;
		else if (display->regwidth == 16 && display->buswidth == 16)
			par->fbtftops.write_register = fbtft_write_reg16_bus16;
		else
			return -EINVAL;
	}

	if (!par->fbtftops.write_vmem) {
		if (display->buswidth == 8)
			par->fbtftops.write_vmem = fbtft_write_vmem16_bus8;
		else if (display->buswidth == 9)
			par->fbtftops.write_vmem = fbtft_write_vmem16_bus9;
		else if (display->buswidth == 16)
			par->fbtftops.write_vmem = fbtft_write_vmem16_bus16;
		else
			return -EINVAL;
	}

	if (!par->fbtftops.write && par->spi) {
		par->fbtftops.write = fbtft_write_spi;
		if (display->buswidth == 9) {
			if (par->spi->master->bits_per_word_mask & SPI_BPW_MASK(9)) {
				par->spi->bits_per_word = 9;
			} else {
				size_t sz = par->txbuf.len +
					    (par->txbuf.len / 8) + 8;

				dev_warn(dev, "9-bit SPI not available, emulating using 8-bit.\n");
				par->fbtftops.write = fbtft_write_spi_emulate_9;
				/* allocate buffer with room for dc bits */
				par->extra = devm_kzalloc(dev, sz, GFP_KERNEL);
				if (!par->extra)
					return -ENOMEM;
			}
		}
	} else if (!par->fbtftops.write) {
		if (display->buswidth == 8)
			par->fbtftops.write = fbtft_write_gpio8_wr;
		else if (display->buswidth == 16)
			par->fbtftops.write = fbtft_write_gpio16_wr;
	}

	par->fbtftops.read = fbtft_read_spi;

	if (of_find_property(dev->of_node, "init", NULL))
		display->fbtftops.init_display = fbtft_init_display_dt;
	else if (par->init_sequence)
		par->fbtftops.init_display = fbtft_init_display;



{
	struct tinydrm_device *tdev = &par->tinydrm;
	const struct drm_display_mode fbtft_mode = {
		TINYDRM_MODE(display->width, display->height, 0, 0),
	};
	struct drm_driver *driver;

	/* spi needs this */
	if (!dev->coherent_dma_mask) {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_warn(dev, "Failed to set dma mask %d\n", ret);
			return ret;
		}
	}

	par->info = devm_kzalloc(dev, sizeof(*par->info), GFP_KERNEL);
	if (!par->info)
		return -ENOMEM;

	par->info->par = par;
	par->info->device = dev;

	par->info->screen_buffer = devm_kzalloc(dev, vmem_size, GFP_KERNEL);
	if (!par->info->screen_buffer)
		return -ENOMEM;

	driver = devm_kmalloc(dev, sizeof(*driver), GFP_KERNEL);
	if (!driver)
		return -ENOMEM;

	*driver = fbtft_driver;
	driver->name = devm_kstrdup(dev, dev_driver_string(dev), GFP_KERNEL);
	if (!driver->name)
		return -ENOMEM;

	driver->desc = driver->name;

	ret = devm_tinydrm_init(dev, tdev, &fbtft_fb_funcs, driver);
	if (ret)
		return ret;

	ret = tinydrm_display_pipe_init(tdev, &fbtft_pipe_funcs,
					DRM_MODE_CONNECTOR_VIRTUAL,
					fbtft_formats,
					ARRAY_SIZE(fbtft_formats), &fbtft_mode,
					rotate);
	if (ret)
		return ret;

	par->info->var.xres = tdev->drm->mode_config.min_width;
	par->info->var.yres = tdev->drm->mode_config.min_height;
	par->info->var.rotate = rotate;
	par->info->fix.line_length = par->info->var.xres * 2;

	tdev->drm->mode_config.preferred_depth = 16;

	drm_mode_config_reset(tdev->drm);

	if (!par->fbtftops.init_display) {
		dev_err(dev, "missing fbtftops.init_display()\n");
		return -EINVAL;
	}

	ret = fbtft_request_gpios(par);
	if (ret < 0)
		return ret;

	ret = fbtft_verify_gpios(par);
	if (ret < 0)
		return ret;

	ret = par->fbtftops.init_display(par);
	if (ret < 0)
		return ret;

	if (par->fbtftops.set_var && !no_set_var) {
		ret = par->fbtftops.set_var(par);
		if (ret < 0)
			return ret;
	}

	if (par->fbtftops.set_gamma && par->gamma.curves) {
		ret = par->fbtftops.set_gamma(par, par->gamma.curves);
		if (ret)
			return ret;
	}

	if (par->fbtftops.register_backlight)
		par->fbtftops.register_backlight(par);

//	fbtft_sysfs_init(par);

	ret = devm_tinydrm_register(tdev);
	if (ret)
		return ret;

	if (par->spi)
		spi_set_drvdata(par->spi, par);
	else if (par->pdev)
		platform_set_drvdata(par->pdev, par);

	if (par->spi)
		DRM_DEBUG_DRIVER("Initialized %s:%s %ux%u @%uMHz on minor %d\n",
				 tdev->drm->driver->name, dev_name(dev),
				 par->info->var.xres, par->info->var.yres,
				 par->spi->max_speed_hz / 1000000,
				 tdev->drm->primary->index);
	else
		DRM_DEBUG_DRIVER("Initialized %s:%s %ux%u on minor %d\n",
				 tdev->drm->driver->name, dev_name(dev),
				 par->info->var.xres, par->info->var.yres,
				 tdev->drm->primary->index);

}


	return 0;
}
EXPORT_SYMBOL(fbtft_probe_common);

int fbtft_remove_common(struct device *dev, struct fbtft_par *par)
{
	DRM_DEBUG_DRIVER("\n");

	if (par->fbtftops.unregister_backlight)
		par->fbtftops.unregister_backlight(par);
//	fbtft_sysfs_exit(par);

	return 0;
}
EXPORT_SYMBOL(fbtft_remove_common);

MODULE_LICENSE("GPL");
