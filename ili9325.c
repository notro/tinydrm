// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM driver for ILI9320 and ILI9325 display controllers
 *
 * Copyright 2020 Noralf Trønnes
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include <asm/unaligned.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_rect.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

struct tinydrm_ili9325 {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct drm_display_mode mode;
	struct spi_device *spi;
	unsigned int devcode;
	bool enabled;
	void *tx_buf;
	bool swap_bytes;
	unsigned int rotation;
	unsigned int set_win_type;
	struct gpio_desc *reset;
	struct backlight_device *backlight;
	struct regulator *regulator;
};

static inline struct tinydrm_ili9325 *
drm_to_ili9325(struct drm_device *drm)
{
	return container_of(drm, struct tinydrm_ili9325, drm);
}

/* Startbyte: | 0 | 1 | 1 | 1 | 0 | ID | RS | RW | */
static u8 ili9325_get_startbyte(bool id, bool rs, bool read)
{
	return 0x70 | (id << 2) | (rs << 1) | read;
}

static int ili9325_spi_transfer(struct tinydrm_ili9325 *ili9325,
				u8 startbyte, const void *buf, size_t len)
{
	struct spi_device *spi = ili9325->spi;
	/* For reliability only run pixel data above spec */
	u32 norm_speed_hz = min_t(u32, 10000000, spi->max_speed_hz);
	struct spi_transfer header = {
		.speed_hz = norm_speed_hz,
		.bits_per_word = 8,
		.len = 1,
	};
	struct spi_transfer tr = {
		.bits_per_word = 16,
	};
	struct spi_message m;
	size_t max_chunk;
	u8 *startbytebuf;
	size_t chunk;
	int ret = 0;

	if (len <= 64)
		tr.speed_hz = norm_speed_hz;

	/* Bytes have already been swapped if necessary */
	if (!spi_is_bpw_supported(ili9325->spi, 16))
		tr.bits_per_word = 8;

	startbytebuf = kmalloc(1, GFP_KERNEL);
	if (!startbytebuf)
		return -ENOMEM;

	header.tx_buf = startbytebuf;
	*startbytebuf = startbyte;

	max_chunk = spi_max_transfer_size(spi);

	spi_message_init(&m);
	spi_message_add_tail(&header, &m);
	spi_message_add_tail(&tr, &m);

	while (len) {
		chunk = min(len, max_chunk);

		tr.tx_buf = buf;
		tr.len = chunk;

		ret = spi_sync(spi, &m);
		if (ret)
			goto err_free;

		buf += chunk;
		len -= chunk;
	}

err_free:
	kfree(startbytebuf);

	return ret;
}

static int ili9325_write_index(struct tinydrm_ili9325 *ili9325, u16 index)
{
	u8 startbyte;
	u16 *buf;
	int ret;

	buf = kmalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (ili9325->swap_bytes)
		*buf = swab16(index);
	else
		*buf = index;

	startbyte = ili9325_get_startbyte(0, 0, 0);
	ret = ili9325_spi_transfer(ili9325, startbyte, buf, sizeof(*buf));
	kfree(buf);

	return ret;
}

static int ili9325_writebuf(struct tinydrm_ili9325 *ili9325, u16 reg,
			    const void *buf, size_t len)
{
	u8 startbyte;
	int ret;

	ret = ili9325_write_index(ili9325, reg);
	if (ret)
		return ret;

	startbyte = ili9325_get_startbyte(0, 1, 0);
	return ili9325_spi_transfer(ili9325, startbyte, buf, len);
}

static int ili9325_write(struct tinydrm_ili9325 *ili9325, u16 reg, u16 val)
{
	u16 *buf;
	int ret;

	buf = kmalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (ili9325->swap_bytes)
		*buf = swab16(val);
	else
		*buf = val;

	ret = ili9325_writebuf(ili9325, reg, buf, sizeof(*buf));
	kfree(buf);

	return ret;
}

static int ili9325_read(struct tinydrm_ili9325 *ili9325, u16 reg, u16 *val)
{
	struct spi_device *spi = ili9325->spi;
	u32 speed_hz = min_t(u32, 5000000, spi->max_speed_hz / 2);
	struct spi_transfer header = {
		.speed_hz = speed_hz,
		.bits_per_word = 8,
		.len = 1,
	};
	struct spi_transfer trrx = {
		.speed_hz = speed_hz,
		.bits_per_word = 8,
		.len = 3, /* including dummy byte */
	};
	struct spi_message m;
	u8 *startbytebuf;
	int ret;

	startbytebuf = kmalloc(header.len, GFP_KERNEL);
	trrx.rx_buf = kzalloc(trrx.len, GFP_KERNEL);
	if (!startbytebuf || !trrx.rx_buf) {
		ret = -ENOMEM;
		goto err_free;
	}

	ret = ili9325_write_index(ili9325, reg);
	if (ret)
		goto err_free;

	header.tx_buf = startbytebuf;
	*startbytebuf = ili9325_get_startbyte(0, 1, true);
	spi_message_init(&m);
	spi_message_add_tail(&header, &m);
	spi_message_add_tail(&trrx, &m);
	ret = spi_sync(spi, &m);
	if (ret)
		goto err_free;

	/* throw away dummy byte */
	*val = get_unaligned_be16(trrx.rx_buf + 1);

err_free:
	kfree(startbytebuf);
	kfree(trrx.rx_buf);

	return ret;
}

static int ili9325_rgb565_buf_copy(void *dst, struct drm_framebuffer *fb,
				   struct drm_rect *clip, bool swap)
{
	struct drm_gem_cma_object *cma_obj = drm_fb_cma_get_gem_obj(fb, 0);
	struct dma_buf_attachment *import_attach = cma_obj->base.import_attach;
	void *src = cma_obj->vaddr;
	int ret = 0;

	if (import_attach) {
		ret = dma_buf_begin_cpu_access(import_attach->dmabuf,
					       DMA_FROM_DEVICE);
		if (ret)
			return ret;
	}

	switch (fb->format->format) {
	case DRM_FORMAT_RGB565:
		if (swap)
			drm_fb_swab16(dst, src, fb, clip);
		else
			drm_fb_memcpy(dst, src, fb, clip);
		break;
	case DRM_FORMAT_XRGB8888:
		drm_fb_xrgb8888_to_rgb565(dst, src, fb, clip, swap);
		break;
	default:
		return -EINVAL;
	}

	if (import_attach)
		ret = dma_buf_end_cpu_access(import_attach->dmabuf,
					     DMA_FROM_DEVICE);
	return ret;
}

static void ili9325_fb_dirty(struct drm_framebuffer *fb, struct drm_rect *rect)
{
	struct drm_gem_cma_object *cma_obj = drm_fb_cma_get_gem_obj(fb, 0);
	struct tinydrm_ili9325 *ili9325 = drm_to_ili9325(fb->dev);
	unsigned int height = drm_rect_height(rect);
	unsigned int width = drm_rect_width(rect);
	int idx, ret = 0;
	bool full;
	void *tr;

	if (!ili9325->enabled)
		return;

	if (!drm_dev_enter(fb->dev, &idx))
		return;

	full = width == fb->width && height == fb->height;

	DRM_DEBUG_KMS("Flushing [FB:%d] " DRM_RECT_FMT "\n", fb->base.id, DRM_RECT_ARG(rect));

	if (ili9325->swap_bytes || !full || fb->format->format == DRM_FORMAT_XRGB8888) {
		tr = ili9325->tx_buf;
		ret = ili9325_rgb565_buf_copy(tr, fb, rect, ili9325->swap_bytes);
		if (ret)
			goto err_exit;
	} else {
		tr = cma_obj->vaddr;
	}

	switch (ili9325->set_win_type) {
	case 0:
		ili9325_write(ili9325, 0x50, rect->x1);
		ili9325_write(ili9325, 0x51, rect->x2 - 1);
		ili9325_write(ili9325, 0x52, rect->y1);
		ili9325_write(ili9325, 0x53, rect->y2 - 1);
		ili9325_write(ili9325, 0x20, rect->x1);
		ili9325_write(ili9325, 0x21, rect->y1);
		break;
	case 1:
		ili9325_write(ili9325, 0x50, rect->y1);
		ili9325_write(ili9325, 0x51, rect->y2 - 1);
		ili9325_write(ili9325, 0x52, 319 - (rect->x2 - 1));
		ili9325_write(ili9325, 0x53, 319 - rect->x1);
		ili9325_write(ili9325, 0x20, rect->y1);
		ili9325_write(ili9325, 0x21, 319 - rect->x1);
		break;
	case 2:
		ili9325_write(ili9325, 0x50, 239 - (rect->x2 - 1));
		ili9325_write(ili9325, 0x51, 239 - rect->x1);
		ili9325_write(ili9325, 0x52, 319 - (rect->y2 - 1));
		ili9325_write(ili9325, 0x53, 319 - rect->y1);
		ili9325_write(ili9325, 0x20, 239 - rect->x1);
		ili9325_write(ili9325, 0x21, 319 - rect->y1);
		break;
	case 3:
		ili9325_write(ili9325, 0x50, 239 - (rect->y2 - 1));
		ili9325_write(ili9325, 0x51, 239 - rect->y1);
		ili9325_write(ili9325, 0x52, rect->x1);
		ili9325_write(ili9325, 0x53, rect->x2 - 1);
		ili9325_write(ili9325, 0x20, 239 - rect->y1);
		ili9325_write(ili9325, 0x21, rect->x1);
		break;
	};

	ret = ili9325_writebuf(ili9325, 0x0022, tr, width * height * 2);

err_exit:
	drm_dev_exit(idx);
	if (ret)
		dev_err_once(fb->dev->dev, "Failed to update display %d\n", ret);
}

static void ili9325_reset(struct tinydrm_ili9325 *ili9325)
{
	if (!ili9325->reset)
		return;

	gpiod_set_value_cansleep(ili9325->reset, 0);
	msleep(1);
	gpiod_set_value_cansleep(ili9325->reset, 1);
	msleep(10);
}

static void ili9325_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct tinydrm_ili9325 *ili9325 = drm_to_ili9325(pipe->crtc.dev);

	ili9325->enabled = false;
	backlight_disable(ili9325->backlight);
}

static void ili9325_pipe_update(struct drm_simple_display_pipe *pipe,
				struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_rect rect;

	if (drm_atomic_helper_damage_merged(old_state, state, &rect))
		ili9325_fb_dirty(state->fb, &rect);

	/* DRM core handles this in Linux 5.7 */
	if (crtc->state->event) {
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		spin_unlock_irq(&crtc->dev->event_lock);
		crtc->state->event = NULL;
	}
}

static void ili9325_enable_flush(struct tinydrm_ili9325 *ili9325,
				 struct drm_plane_state *plane_state)
{
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_rect rect = {
		.x1 = 0,
		.x2 = fb->width,
		.y1 = 0,
		.y2 = fb->height,
	};

	ili9325->enabled = true;
	ili9325_fb_dirty(fb, &rect);
	backlight_enable(ili9325->backlight);
}

/* Uses an ILI9320 controller */
static void hy28a_pipe_enable(struct drm_simple_display_pipe *pipe,
			      struct drm_crtc_state *crtc_state,
			      struct drm_plane_state *plane_state)
{
	struct tinydrm_ili9325 *ili9325 = drm_to_ili9325(pipe->crtc.dev);
	struct device *dev = ili9325->drm.dev;
	int idx, ret;

	if (!drm_dev_enter(pipe->crtc.dev, &idx))
		return;

	ili9325_reset(ili9325);

	/* Initialization sequence from HY28A example code */

	ret = ili9325_write(ili9325, 0x00, 0x0000);
	if (ret) {
		dev_err(dev, "Failed to write register\n");
		goto out_exit;
	}

	ili9325_write(ili9325, 0x01, 0x0100);	/* Driver Output Control */
	ili9325_write(ili9325, 0x02, 0x0700);	/* LCD Driver Waveform Control */
	ili9325_write(ili9325, 0x03, 0x1038);	/* Set the scan mode */
	ili9325_write(ili9325, 0x04, 0x0000);	/* Scalling Control */
	ili9325_write(ili9325, 0x08, 0x0202);	/* Display Control 2 */
	ili9325_write(ili9325, 0x09, 0x0000);	/* Display Control 3 */
	ili9325_write(ili9325, 0x0a, 0x0000);	/* Frame Cycle Contal */
	ili9325_write(ili9325, 0x0c, BIT(0));	/* Extern Display Interface Control 1 */
	ili9325_write(ili9325, 0x0d, 0x0000);	/* Frame Maker Position */
	ili9325_write(ili9325, 0x0f, 0x0000);	/* Extern Display Interface Control 2 */
	mdelay(50);
	ili9325_write(ili9325, 0x07, 0x0101);	/* Display Control */
	mdelay(50);
	ili9325_write(ili9325, 0x10, BIT(12) | BIT(7) | BIT(6)); /* Power Control 1 */
	ili9325_write(ili9325, 0x11, 0x0007);	/* Power Control 2 */
	ili9325_write(ili9325, 0x12, BIT(8) | BIT(4));	/* Power Control 3 */
	ili9325_write(ili9325, 0x13, 0x0b00);	/* Power Control 4 */
	ili9325_write(ili9325, 0x29, 0x0000);	/* Power Control 7 */
	ili9325_write(ili9325, 0x2b, BIT(14) | BIT(4));

	ili9325_write(ili9325, 0x50, 0);	/* Set X Start */
	ili9325_write(ili9325, 0x51, 239);	/* Set X End */
	ili9325_write(ili9325, 0x52, 0);	/* Set Y Start */
	ili9325_write(ili9325, 0x53, 319);	/* Set Y End */
	mdelay(50);

	ili9325_write(ili9325, 0x60, 0x2700);	/* Driver Output Control */
	ili9325_write(ili9325, 0x61, 0x0001);	/* Driver Output Control */
	ili9325_write(ili9325, 0x6a, 0x0000);	/* Vertical Srcoll Control */

	ili9325_write(ili9325, 0x80, 0x0000);	/* Display Position? Partial Display 1 */
	ili9325_write(ili9325, 0x81, 0x0000);	/* RAM Address Start? Partial Display 1 */
	ili9325_write(ili9325, 0x82, 0x0000);	/* RAM Address End-Partial Display 1 */
	ili9325_write(ili9325, 0x83, 0x0000);	/* Displsy Position? Partial Display 2 */
	ili9325_write(ili9325, 0x84, 0x0000);	/* RAM Address Start? Partial Display 2 */
	ili9325_write(ili9325, 0x85, 0x0000);	/* RAM Address End? Partial Display 2 */

	ili9325_write(ili9325, 0x90, 16); /* Frame Cycle Control */
	ili9325_write(ili9325, 0x92, 0x0000);	/* Panel Interface Control 2 */
	ili9325_write(ili9325, 0x93, 0x0001);	/* Panel Interface Control 3 */
	ili9325_write(ili9325, 0x95, 0x0110);	/* Frame Cycle Control */
	ili9325_write(ili9325, 0x97, 0);
	ili9325_write(ili9325, 0x98, 0x0000);	/* Frame Cycle Control */

	switch (ili9325->rotation) {
	case 0:
		ili9325_write(ili9325, 0x0003, 0x1028);
		ili9325->set_win_type = 3;
		break;
	case 90:
		ili9325_write(ili9325, 0x0003, 0x1030);
		ili9325->set_win_type = 0;
		break;
	case 180:
		ili9325_write(ili9325, 0x0003, 0x1018);
		ili9325->set_win_type = 1;
		break;
	case 270:
		ili9325_write(ili9325, 0x0003, 0x1000);
		ili9325->set_win_type = 2;
		break;
	}

	ili9325_write(ili9325, 0x0007, 0x0133);
	mdelay(100);

	ili9325_enable_flush(ili9325, plane_state);
out_exit:
	drm_dev_exit(idx);
}

static const struct drm_simple_display_pipe_funcs hy28a_funcs = {
	.enable =  hy28a_pipe_enable,
	.disable = ili9325_pipe_disable,
	.update = ili9325_pipe_update,
	.prepare_fb = drm_gem_fb_simple_display_pipe_prepare_fb,
};

/* Uses an ILI9325 controller */
static void hy28b_pipe_enable(struct drm_simple_display_pipe *pipe,
			      struct drm_crtc_state *crtc_state,
			      struct drm_plane_state *plane_state)
{
	struct tinydrm_ili9325 *ili9325 = drm_to_ili9325(pipe->crtc.dev);
	struct device *dev = ili9325->drm.dev;
	int idx, ret;

	if (!drm_dev_enter(pipe->crtc.dev, &idx))
		return;

	ili9325_reset(ili9325);

	/*
	 * FIXME:
	 * Apparently there are 2 versions of this display:
	 * https://github.com/raspberrypi/linux/pull/2721
	 *
	 * The ILI9325D has the same ID code (0x9325) as the ILI9325, so it can't be detected at runtime.
	 * Maybe the OTP registers are programmed?
	 * SPI reading is controlled by register R66h on ILI9325D.
	 */

	/* Initialization sequence from HY28B example code */

	ret = ili9325_write(ili9325, 0x00e7, 0x0010);
	if (ret) {
		dev_err(dev, "Failed to write register\n");
		goto out_exit;
	}

	ili9325_write(ili9325, 0x0000, 0x0001);
	ili9325_write(ili9325, 0x0001, 0x0100);
	ili9325_write(ili9325, 0x0002, 0x0700);
	ili9325_write(ili9325, 0x0003, BIT(12) | BIT(5) | BIT(4));
	ili9325_write(ili9325, 0x0004, 0x0000);
	ili9325_write(ili9325, 0x0008, 0x0207);
	ili9325_write(ili9325, 0x0009, 0x0000);
	ili9325_write(ili9325, 0x000a, 0x0000);
	ili9325_write(ili9325, 0x000c, 0x0001);
	ili9325_write(ili9325, 0x000d, 0x0000);
	ili9325_write(ili9325, 0x000f, 0x0000);

	/* Power On sequence */
	ili9325_write(ili9325, 0x0010, 0x0000);
	ili9325_write(ili9325, 0x0011, 0x0007);
	ili9325_write(ili9325, 0x0012, 0x0000);
	ili9325_write(ili9325, 0x0013, 0x0000);
	mdelay(50);

	ili9325_write(ili9325, 0x0010, 0x1590);
	ili9325_write(ili9325, 0x0011, 0x0227);
	mdelay(50);

	ili9325_write(ili9325, 0x0012, 0x009c);
	mdelay(50);

	ili9325_write(ili9325, 0x0013, 0x1900);
	ili9325_write(ili9325, 0x0029, 0x0023);
	ili9325_write(ili9325, 0x002b, 0x000e);
	mdelay(50);

	ili9325_write(ili9325, 0x0020, 0x0000);
	ili9325_write(ili9325, 0x0021, 0x0000);
	mdelay(50);

	ili9325_write(ili9325, 0x0030, 0x0007);
	ili9325_write(ili9325, 0x0031, 0x0707);
	ili9325_write(ili9325, 0x0032, 0x0006);
	ili9325_write(ili9325, 0x0035, 0x0704);
	ili9325_write(ili9325, 0x0036, 0x1f04);
	ili9325_write(ili9325, 0x0037, 0x0004);
	ili9325_write(ili9325, 0x0038, 0x0000);
	ili9325_write(ili9325, 0x0039, 0x0706);
	ili9325_write(ili9325, 0x003c, 0x0701);
	ili9325_write(ili9325, 0x003d, 0x000f);
	mdelay(50);

	ili9325_write(ili9325, 0x0050, 0);
	ili9325_write(ili9325, 0x0051, 239);
	ili9325_write(ili9325, 0x0052, 0);
	ili9325_write(ili9325, 0x0053, 319);

	ili9325_write(ili9325, 0x0060, 0xa700);
	ili9325_write(ili9325, 0x0061, 0x0001);
	ili9325_write(ili9325, 0x006a, 0x0000);

	ili9325_write(ili9325, 0x0080, 0x0000);
	ili9325_write(ili9325, 0x0081, 0x0000);
	ili9325_write(ili9325, 0x0082, 0x0000);
	ili9325_write(ili9325, 0x0083, 0x0000);
	ili9325_write(ili9325, 0x0084, 0x0000);
	ili9325_write(ili9325, 0x0085, 0x0000);

	ili9325_write(ili9325, 0x0090, 0x0010);
	ili9325_write(ili9325, 0x0092, 0x0000);
	ili9325_write(ili9325, 0x0093, 0x0003);
	ili9325_write(ili9325, 0x0095, 0x0110);
	ili9325_write(ili9325, 0x0097, 0x0000);
	ili9325_write(ili9325, 0x0098, 0x0000);

	switch (ili9325->rotation) {
	case 0:
		ili9325_write(ili9325, 0x0003, 0x1018);
		ili9325->set_win_type = 1;
		break;
	case 90:
		ili9325_write(ili9325, 0x0003, 0x1000);
		ili9325->set_win_type = 2;
		break;
	case 180:
		ili9325_write(ili9325, 0x0003, 0x1028);
		ili9325->set_win_type = 3;
		break;
	case 270:
		ili9325_write(ili9325, 0x0003, 0x1030);
		ili9325->set_win_type = 0;
		break;
	}

	ili9325_write(ili9325, 0x0007, 0x0133);
	mdelay(100);

	ili9325_enable_flush(ili9325, plane_state);
out_exit:
	drm_dev_exit(idx);
}

static const struct drm_simple_display_pipe_funcs hy28b_funcs = {
	.enable =  hy28b_pipe_enable,
	.disable = ili9325_pipe_disable,
	.update = ili9325_pipe_update,
	.prepare_fb = drm_gem_fb_simple_display_pipe_prepare_fb,
};

static int ili9325_connector_get_modes(struct drm_connector *connector)
{
	struct tinydrm_ili9325 *ili9325 = drm_to_ili9325(connector->dev);
	struct drm_display_mode *mode;
	mode = drm_mode_duplicate(connector->dev, &ili9325->mode);
	if (!mode) {
		DRM_ERROR("Failed to duplicate mode\n");
		return 0;
	}

	if (mode->name[0] == '\0')
		drm_mode_set_name(mode);

	mode->type |= DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	if (mode->width_mm) {
		connector->display_info.width_mm = mode->width_mm;
		connector->display_info.height_mm = mode->height_mm;
	}

	return 1;
}

static const struct drm_connector_helper_funcs ili9325_connector_hfuncs = {
	.get_modes = ili9325_connector_get_modes,
};

static const struct drm_connector_funcs ili9325_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int ili9325_rotate_mode(struct drm_display_mode *mode,
				unsigned int rotation)
{
	if (rotation == 0 || rotation == 180) {
		return 0;
	} else if (rotation == 90 || rotation == 270) {
		swap(mode->hdisplay, mode->vdisplay);
		swap(mode->hsync_start, mode->vsync_start);
		swap(mode->hsync_end, mode->vsync_end);
		swap(mode->htotal, mode->vtotal);
		swap(mode->width_mm, mode->height_mm);
		return 0;
	} else {
		return -EINVAL;
	}
}

static ssize_t ili9325_debugfs_reg_write(struct file *file,
					 const char __user *user_buf,
					 size_t count, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct tinydrm_ili9325 *ili9325 = m->private;
	char *buf, *regc, *valc;
	unsigned long reg, val;
	int idx, ret;

	if (!drm_dev_enter(&ili9325->drm, &idx))
		return -ENODEV;

	buf = memdup_user_nul(user_buf, count);
	if (IS_ERR(buf)) {
		ret = PTR_ERR(buf);
		goto err_exit;
	}

	valc = buf;

	regc = strsep(&valc, " ");
	if (!regc) {
		ret = -EINVAL;
		goto err_free;
	}

	ret = kstrtoul(regc, 16, &reg);
	if (ret < 0)
		goto err_free;
	ret = kstrtoul(valc, 16, &val);
	if (ret < 0)
		goto err_free;

	ret = ili9325_write(ili9325, reg, val);
err_free:
	kfree(buf);
err_exit:
	drm_dev_exit(idx);

	return ret < 0 ? ret : count;
}

static int ili9325_debugfs_reg_show(struct seq_file *m, void *d)
{
	struct tinydrm_ili9325 *ili9325 = m->private;
	u16 reg, val;
	int idx, ret;

	if (!drm_dev_enter(&ili9325->drm, &idx))
		return -ENODEV;

	for (reg = 0; reg < 0xaf; reg++) {
		seq_printf(m, "%04x: ", reg);
		ret = ili9325_read(ili9325, reg, &val);
		if (ret)
			seq_puts(m, "XX\n");
		else
			seq_printf(m, "%04x\n", val);
	}

	drm_dev_exit(idx);

	return 0;
}

static int ili9325_debugfs_reg_open(struct inode *inode,
					   struct file *file)
{
	return single_open(file, ili9325_debugfs_reg_show,
			   inode->i_private);
}

static const struct file_operations ili9325_debugfs_reg_fops = {
	.owner = THIS_MODULE,
	.open = ili9325_debugfs_reg_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = ili9325_debugfs_reg_write,
};

static int ili9325_debugfs_init(struct drm_minor *minor)
{
	struct tinydrm_ili9325 *ili9325 = drm_to_ili9325(minor->dev);
	umode_t mode = S_IFREG | S_IWUSR;

	if (ili9325->devcode)
		mode |= S_IRUGO;

	debugfs_create_file("registers", mode, minor->debugfs_root,
			    ili9325, &ili9325_debugfs_reg_fops);

	return 0;
}

static void fb_ili9325_release(struct drm_device *drm)
{
	struct tinydrm_ili9325 *ili9325 = drm_to_ili9325(drm);

	DRM_DEBUG_DRIVER("\n");

	drm_mode_config_cleanup(drm);
	drm_dev_fini(drm);
	kfree(ili9325);
}

static const struct drm_display_mode ili9325_mode = {
	DRM_SIMPLE_MODE(320, 240, 0, 0),
};

static const uint32_t ili9325_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

static const uint64_t ili9325_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static const struct drm_mode_config_funcs ili9325_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_CMA_FOPS(ili9325_fops);

static struct drm_driver ili9325_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &ili9325_fops,
	.release		= fb_ili9325_release,
	DRM_GEM_CMA_VMAP_DRIVER_OPS,
	.debugfs_init		= ili9325_debugfs_init,
	.name			= "ili9325",
	.desc			= "Ilitek ILI9325",
	.date			= "20200129",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id ili9325_of_match[] = {
	{ .compatible = "haoyu,hy28a", .data = &hy28a_funcs },
	{ .compatible = "haoyu,hy28b", .data = &hy28b_funcs },
	{},
};
MODULE_DEVICE_TABLE(of, ili9325_of_match);

static const struct spi_device_id ili9325_spi_ids[] = {
	{ "hy28a", (unsigned long)&hy28a_funcs },
	{ "hy28b", (unsigned long)&hy28b_funcs },
	{ },
};
MODULE_DEVICE_TABLE(spi, ili9325_spi_ids);

static int ili9325_probe_spi(struct spi_device *spi)
{
	const struct drm_simple_display_pipe_funcs *funcs;
	struct tinydrm_ili9325 *ili9325;
	struct device *dev = &spi->dev;
	struct drm_device *drm;
	u32 rotation = 0;
	u16 devcode;
	int ret;

	funcs = device_get_match_data(dev);
	if (!funcs) {
		const struct spi_device_id *spi_id = spi_get_device_id(spi);

		funcs = (const struct drm_simple_display_pipe_funcs *)spi_id->driver_data;
	}

	/* The SPI device is used to allocate dma memory */
	if (!dev->coherent_dma_mask) {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_warn(dev, "Failed to set dma mask %d\n", ret);
			return ret;
		}
	}

	ili9325 = kzalloc(sizeof(*ili9325), GFP_KERNEL);
	if (!ili9325)
		return -ENOMEM;

	ili9325->spi = spi;
#ifdef __LITTLE_ENDIAN
	if (!spi_is_bpw_supported(spi, 16))
		ili9325->swap_bytes = true;
#endif
	drm = &ili9325->drm;
	ret = devm_drm_dev_init(dev, drm, &ili9325_driver);
	if (ret) {
		kfree(ili9325);
		return ret;
	}

	drm_mode_config_init(drm);

	ili9325->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ili9325->reset)) {
		ret = PTR_ERR(ili9325->reset);
		if (ret != EPROBE_DEFER)
			dev_err(dev, "Failed to get gpio 'reset'\n");
		return ret;
	}

	ili9325->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(ili9325->backlight)) {
		ret = PTR_ERR(ili9325->backlight);
		if (ret != EPROBE_DEFER)
			dev_err(dev, "Failed to get backlight\n");
		return ret;
	}

	ili9325->tx_buf = devm_kmalloc(dev, 320 * 240 * 2, GFP_KERNEL);
	if (!ili9325->tx_buf)
		return -ENOMEM;

	device_property_read_u32(dev, "rotation", &rotation);
	ili9325->rotation = rotation;

	/*
	 * FIXME:
	 * Rotating the mode like this won't be accepted in mainline anymore.
	 *
	 * AFAIR the idea is to add a helper that reads the rotation property and
	 * sets the connector->display_info->panel_orientation property.
	 * drm_connector_init_panel_orientation_property() will pick this up and
	 * present a property to userspace. Now it's up to userspace to deal with
	 * rotation. Since this is a fairly new addition, don't expect much support
	 * for this in libaries in the embedded world.
	 *
	 * There is a rotation property on the plane to do hw rotation, but I don't
	 * know how it works for 90/270 rotation. Who flips the axis? How does this
	 * really work?
	 *
	 * The easy solution is probably to let userspace rotate in software and
	 * drop hw rotation support.
	 *
	 * The fbdev emulation does not support 90/270 rotation through the
	 * connector property. This is due to tiling issues on certain framebuffers.
	 * See drm_client_rotation().
	 */
	drm_mode_copy(&ili9325->mode, &ili9325_mode);
	ret = ili9325_rotate_mode(&ili9325->mode, rotation);
	if (ret) {
		dev_err(dev, "Illegal rotation value %u\n", rotation);
		return -EINVAL;
	}

	drm->mode_config.min_width = ili9325->mode.hdisplay;
	drm->mode_config.max_width = ili9325->mode.hdisplay;
	drm->mode_config.min_height = ili9325->mode.vdisplay;
	drm->mode_config.max_height = ili9325->mode.vdisplay;
	drm->mode_config.funcs = &ili9325_mode_config_funcs;
	drm->mode_config.preferred_depth = 16;

	drm_connector_helper_add(&ili9325->connector, &ili9325_connector_hfuncs);
	ret = drm_connector_init(drm, &ili9325->connector, &ili9325_connector_funcs,
				 DRM_MODE_CONNECTOR_SPI);
	if (ret)
		return ret;

	ret = drm_simple_display_pipe_init(drm, &ili9325->pipe, funcs,
					   ili9325_formats, ARRAY_SIZE(ili9325_formats),
					   ili9325_modifiers, &ili9325->connector);
	if (ret)
		return ret;

	/* FIXME: If there's no use for devcode, this can be moved to ili9325_debugfs_init() */
	/* We read garbage if SPI MISO is not wired up */
	ret = ili9325_read(ili9325, 0x0000, &devcode);
	if (!ret && (devcode & 0xff00) == 0x9300) {
		DRM_DEBUG_DRIVER("devcode=0x%x\n", devcode);
		ili9325->devcode = devcode;
	}

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_fbdev_generic_setup(drm, 16);

	spi_set_drvdata(spi, drm);

	DRM_DEBUG_DRIVER("SPI speed: %uMHz\n", spi->max_speed_hz / 1000000);

	return 0;
}

static int ili9325_remove(struct spi_device *spi)
{
	struct drm_device *drm = spi_get_drvdata(spi);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);

	return 0;
}

static void ili9325_shutdown(struct spi_device *spi)
{
	drm_atomic_helper_shutdown(spi_get_drvdata(spi));
}

static struct spi_driver ili9325_spi_driver = {
	.driver = {
		.name   = "ili9325",
		.owner  = THIS_MODULE,
		.of_match_table = of_match_ptr(ili9325_of_match),
	},
	.id_table = ili9325_spi_ids,
	.probe = ili9325_probe_spi,
	.remove = ili9325_remove,
	.shutdown = ili9325_shutdown,
};
module_spi_driver(ili9325_spi_driver);

MODULE_DESCRIPTION("DRM driver for the ILI9325 display controller");
MODULE_AUTHOR("Noralf Trønnes");
MODULE_LICENSE("GPL");
