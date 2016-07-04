#define DEBUG
#define VERBOSE_DEBUG
/*
 * MIPI Display Bus Interface (DBI) LCD controller support
 *
 * Copyright 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drm_gem_cma_helper.h>
#include <drm/tinydrm/mipi-dbi.h>
#include <drm/tinydrm/tinydrm.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/swab.h>
#include <video/mipi_display.h>

#define MIPI_DBI_DEFAULT_SPI_READ_SPEED 2000000 /* 2MHz */

#define DCS_POWER_MODE_DISPLAY			BIT(2)
#define DCS_POWER_MODE_DISPLAY_NORMAL_MODE	BIT(3)
#define DCS_POWER_MODE_SLEEP_MODE		BIT(4)
#define DCS_POWER_MODE_PARTIAL_MODE		BIT(5)
#define DCS_POWER_MODE_IDLE_MODE		BIT(6)
#define DCS_POWER_MODE_RESERVED_MASK		(BIT(0) | BIT(1) | BIT(7))

struct mipi_dbi_spi {
	struct regmap *map;
	void *context;
	unsigned int ram_reg;
	struct gpio_desc *dc;
	bool write_only;
};

/**
 * mipi_dbi_write_buf - Write command and parameter array
 * @reg: Controller register
 * @cmd: Command
 * @parameters: Array of parameters
 * @num: Number of parameters
 */
int mipi_dbi_write_buf(struct regmap *reg, unsigned cmd, const u8 *parameters,
		       size_t num)
{
	u8 *buf;
	int ret;

	buf = kmalloc(num, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memcpy(buf, parameters, num);

	ret = regmap_raw_write(reg, cmd, buf, num);
	kfree(buf);

	return ret;
}
EXPORT_SYMBOL(mipi_dbi_write_buf);

static void mipi_dbi_hexdump(char *linebuf, size_t linebuflen, const void *buf, size_t len, size_t bpw, size_t max)
{

	if (bpw > 16) {
		snprintf(linebuf, linebuflen, "bpw not supported");
	} else if (bpw > 8) {
		size_t count = len > max ? max / 2 : (len / 2);
		const u16 *buf16 = buf;
		unsigned int j, lx = 0;
		int ret;

		for (j = 0; j < count; j++) {
			ret = snprintf(linebuf + lx, linebuflen - lx,
				       "%s%4.4x", j ? " " : "", *buf16++);
			if (ret >= linebuflen - lx) {
				snprintf(linebuf, linebuflen, "ERROR");
				break;
			}
			lx += ret;
		}
	} else {
		hex_dump_to_buffer(buf, len, max, 1, linebuf, linebuflen, false);
	}
}

#ifdef VERBOSE_DEBUG
static void mipi_dbi_vdbg_spi_message(struct spi_device *spi,
				      struct spi_message *m)
{
	struct spi_master *master = spi->master;
	struct spi_transfer *tmp;
	struct list_head *pos;
	char linebuf[3 * 32];
	int i = 0;

	if (!(drm_debug & DRM_UT_CORE))
		return;

	list_for_each(pos, &m->transfers) {
		tmp = list_entry(pos, struct spi_transfer, transfer_list);

		if (tmp->tx_buf) {
			bool dma = false;

			if (master->can_dma)
				dma = master->can_dma(master, spi, tmp);

			mipi_dbi_hexdump(linebuf, ARRAY_SIZE(linebuf),
					tmp->tx_buf, tmp->len,
					tmp->bits_per_word, 16);
			pr_debug("    tr[%i]: bpw=%i, dma=%u, len=%u, tx_buf(%p)=[%s%s]\n",
				 i, tmp->bits_per_word, dma, tmp->len,
				 tmp->tx_buf, linebuf,
				 tmp->len > 16 ? " ..." : "");
		}
		if (tmp->rx_buf) {
			mipi_dbi_hexdump(linebuf, ARRAY_SIZE(linebuf),
					tmp->rx_buf, tmp->len,
					tmp->bits_per_word, 16);
			pr_debug("    tr[%i]: bpw=%i,        len=%u, rx_buf(%p)=[%s%s]\n",
				 i, tmp->bits_per_word, tmp->len, tmp->rx_buf,
				 linebuf, tmp->len > 16 ? " ..." : "");
		}
		i++;
	}
}
#else
static void mipi_dbi_vdbg_spi_message(struct spi_device *spi,
				      struct spi_message *m)
{
}
#endif

static void mspi_debug(const void *reg, size_t reg_bytes, const void *buf,
		       size_t len, size_t val_bytes)
{
	unsigned int regnr;

	if (!(drm_debug & DRM_UT_CORE))
		return;

	regnr = reg_bytes == 1 ? *(u8 *) reg : *(u16 *) reg;

	if (buf && len) {
		char linebuf[3 * 32];

		mipi_dbi_hexdump(linebuf, ARRAY_SIZE(linebuf), buf, len,
				 val_bytes * 8, 16);
		DRM_DEBUG("reg=0x%0*x, data(%zu)= %s%s\n",
			  reg_bytes == 1 ? 2 : 4, regnr, len, linebuf,
			  len > 32 ? " ..." : "");
	} else {
		DRM_DEBUG("reg=0x%0*x\n", reg_bytes == 1 ? 2 : 4, regnr);
	}
}

static size_t mipi_dbi_spi_clamp_size(struct spi_device *spi, size_t size)
{
	size_t max_spi, clamped;

	max_spi = min(spi_max_transfer_size(spi), spi->master->max_dma_len);
	if (!size)
		size = max_spi;
	clamped = clamp_val(size, 4, max_spi);
	clamped &= ~0x3;

	return clamped;
}

static bool mipi_dbi_spi_bpw_supported(struct spi_device *spi, u8 bpw)
{
	u32 bpw_mask = spi->master->bits_per_word_mask;

	if (bpw == 8)
		return true;

	if (!bpw_mask) {
		dev_warn_once(&spi->dev,
			      "bits_per_word_mask not set, assume only 8\n");
		return false;
	}

	if (bpw_mask & SPI_BPW_MASK(bpw))
		return true;

	return false;
}

/*
 * MIPI DBI Type C Option 1
 *
 * If the SPI controller doesn't have 9 bits per word support,
 * use blocks of 9 bytes to send 8x 9-bit words with a 8-bit SPI transfer.
 * Pad partial blocks with MIPI_DCS_NOP (zero).
 */

#define SHIFT_U9_INTO_U64(_dst, _src, _pos) \
{ \
	(_dst) |= 1ULL << (63 - ((_pos) * 9)); \
	(_dst) |= (u64)(_src) << (63 - 8 - ((_pos) * 9)); \
}

static int mipi_dbi_spi1e_transfer(struct mipi_dbi_spi *mspi, u8 bits_per_word,
				   int dc, const void *buf, size_t len,
				   size_t max_chunk)
{
	struct spi_device *spi = mspi->context;
	struct spi_transfer tr = {
		.bits_per_word = 8,
	};
	struct spi_message m;
	size_t max_src_chunk, chunk;
	int i, ret = 0;
	u8 *dst;
	void *buf_dc;
	const u8 *src = buf;

	max_chunk = mipi_dbi_spi_clamp_size(spi, max_chunk);
	if (max_chunk < 9)
		return -EINVAL;

#ifdef VERBOSE_DEBUG
	DRM_DEBUG("dev=%s, dc=%d, max_chunk=%zu, transfers:\n",
		  dev_name(&spi->dev), dc, max_chunk);
#endif
	spi_message_init_with_transfers(&m, &tr, 1);

	if (!dc) {
		/* pad at beginning of block */
		if (WARN_ON_ONCE(len != 1 || bits_per_word != 8))
			return -EINVAL;

		dst = kzalloc(9, GFP_KERNEL);
		if (!dst)
			return -ENOMEM;

		dst[8] = *src;
		tr.tx_buf = dst;
		tr.len = 9;

		mipi_dbi_vdbg_spi_message(spi, &m);
		ret = spi_sync(spi, &m);
		kfree(dst);

		return ret;
	}

	/* 8-byte aligned max_src_chunk that fits max_chunk */
	max_src_chunk = max_chunk / 9 * 8;
	max_src_chunk = min(max_src_chunk, len);
	max_src_chunk = max_t(size_t, 8, max_src_chunk & ~0x7);

	max_chunk = max_src_chunk + (max_src_chunk / 8);
	buf_dc = kmalloc(max_chunk, GFP_KERNEL);
	if (!buf_dc)
		return -ENOMEM;

	tr.tx_buf = buf_dc;

	while (len) {
		size_t added = 0;

		chunk = min(len, max_src_chunk);
		len -= chunk;
		dst = buf_dc;

		if (chunk < 8) {
			/* pad at end of block */
			u64 tmp = 0;
			int j;

			if (bits_per_word == 8) {
				for (j = 0; j < chunk; j++)
					SHIFT_U9_INTO_U64(tmp, *src++, j);
			} else {
				for (j = 0; j < (chunk / 2); j += 2) {
					SHIFT_U9_INTO_U64(tmp, *src++, j + 1);
					SHIFT_U9_INTO_U64(tmp, *src++, j);
				}
			}

			*(u64 *)dst = cpu_to_be64(tmp);
			dst[8] = 0x00;
			chunk = 8;
			added = 1;
		} else {
			for (i = 0; i < chunk; i += 8) {
				u64 tmp = 0;

				if (bits_per_word == 8) {
					SHIFT_U9_INTO_U64(tmp, *src++, 0);
					SHIFT_U9_INTO_U64(tmp, *src++, 1);
					SHIFT_U9_INTO_U64(tmp, *src++, 2);
					SHIFT_U9_INTO_U64(tmp, *src++, 3);
					SHIFT_U9_INTO_U64(tmp, *src++, 4);
					SHIFT_U9_INTO_U64(tmp, *src++, 5);
					SHIFT_U9_INTO_U64(tmp, *src++, 6);

					tmp |= 0x1;
					/* TODO: unaligned access here? */
					*(u64 *)dst = cpu_to_be64(tmp);
					dst += 8;
					*dst++ = *src++;
				} else {
					u8 src7;

					SHIFT_U9_INTO_U64(tmp, *src++, 1);
					SHIFT_U9_INTO_U64(tmp, *src++, 0);
					SHIFT_U9_INTO_U64(tmp, *src++, 3);
					SHIFT_U9_INTO_U64(tmp, *src++, 2);
					SHIFT_U9_INTO_U64(tmp, *src++, 5);
					SHIFT_U9_INTO_U64(tmp, *src++, 4);
					src7 = *src++;
					SHIFT_U9_INTO_U64(tmp, *src++, 6);

					tmp |= 0x1;
					/* TODO: unaligned access here? */
					*(u64 *)dst = cpu_to_be64(tmp);
					dst += 8;
					*dst++ = src7;
				}
				added++;
			}
		}

		tr.len = chunk + added;

		mipi_dbi_vdbg_spi_message(spi, &m);
		ret = spi_sync(spi, &m);
		if (ret)
			goto err_free;
	};

err_free:
	kfree(buf_dc);

	return ret;
}

#undef SHIFT_U9_INTO_U64

static int mipi_dbi_spi1_transfer(struct mipi_dbi_spi *mspi, u8 bits_per_word,
				  int dc, const void *buf, size_t len,
				  size_t max_chunk)
{
	struct spi_device *spi = mspi->context;
	struct spi_transfer tr = {
		.bits_per_word = 9,
	};
	const u16 *src16 = buf;
	const u8 *src8 = buf;
	struct spi_message m;
	size_t max_src_chunk;
	int ret = 0;
	u16 *dst16;

	if (!mipi_dbi_spi_bpw_supported(spi, 9))
		return mipi_dbi_spi1e_transfer(mspi, bits_per_word, dc, buf,
					       len, max_chunk);

	if (WARN_ON_ONCE(bits_per_word == 16 && len % 2))
		return -EINVAL;

	max_chunk = mipi_dbi_spi_clamp_size(spi, max_chunk);

#ifdef VERBOSE_DEBUG
	DRM_DEBUG("dev=%s, dc=%d, max_chunk=%zu, transfers:\n",
		  dev_name(&spi->dev), dc, max_chunk);
#endif
	max_src_chunk = min(max_chunk / 2, len);

	dst16 = kmalloc(max_src_chunk * 2, GFP_KERNEL);
	if (!dst16)
		return -ENOMEM;

	spi_message_init_with_transfers(&m, &tr, 1);
	tr.tx_buf = dst16;

	while (len) {
		size_t chunk = min(len, max_src_chunk);
		unsigned int i;

		if (bits_per_word == 8) {
			for (i = 0; i < chunk; i++) {
				dst16[i] = *src8++;
				if (dc)
					dst16[i] |= 0x0100;
			}
		} else {
			for (i = 0; i < (chunk * 2); i += 2) {
				dst16[i]     = *src16 >> 8;
				dst16[i + 1] = *src16++ & 0xFF;
				if (dc) {
					dst16[i]     |= 0x0100;
					dst16[i + 1] |= 0x0100;
				}
			}
		}
		tr.len = chunk;
		len -= chunk;

		mipi_dbi_vdbg_spi_message(spi, &m);
		ret = spi_sync(spi, &m);
		if (ret)
			goto err_free;
	};

err_free:
	kfree(dst16);

	return ret;
}

static int mipi_dbi_spi1_gather_write(void *context, const void *reg,
				      size_t reg_len, const void *val,
				      size_t val_len)
{
	struct mipi_dbi_spi *mspi = context;
	size_t val_bytes = regmap_get_val_bytes(mspi->map);
	unsigned int regnr;
	int ret;

	if (reg_len == 1)
		regnr = *(u8 *) reg;
	else if (reg_len == 2)
		regnr = *(u16 *) reg;
	else
		return -EINVAL;

	if (regnr == mspi->ram_reg)
		val_bytes = 2;

	mspi_debug(reg, reg_len, val, val_len, val_bytes);

	ret = mipi_dbi_spi1_transfer(mspi, reg_len * 8, 0, reg, reg_len, 4096);
	if (ret)
		return ret;

	if (val && val_len)
		ret = mipi_dbi_spi1_transfer(mspi, val_bytes * 8, 1, val,
					     val_len, 4096);

	return ret;
}

static int mipi_dbi_spi1_write(void *context, const void *data, size_t count)
{
	return mipi_dbi_spi1_gather_write(context, data, 1,
					 data + 1, count - 1);
}

static int mipi_dbi_spi1_read(void *context, const void *reg, size_t reg_size,
			      void *val, size_t val_size)
{
	return -ENOTSUPP;
}

static const struct regmap_bus mipi_dbi_regmap_bus1 = {
	.write = mipi_dbi_spi1_write,
	.gather_write = mipi_dbi_spi1_gather_write,
	.read = mipi_dbi_spi1_read,
	.reg_format_endian_default = REGMAP_ENDIAN_DEFAULT,
	.val_format_endian_default = REGMAP_ENDIAN_DEFAULT,
};

/* MIPI DBI Type C Option 3 */

static int mipi_dbi_spi3_transfer(struct mipi_dbi_spi *mspi, u8 bits_per_word,
				  int dc, const void *buf, size_t len,
				  size_t max_chunk)
{
	struct spi_device *spi = mspi->context;
	struct spi_transfer tr = {
		.bits_per_word = bits_per_word,
//		.speed_hz = spi->max_speed_hz,
	};
	struct spi_message m;
	u16 *swap_buf = NULL;
	bool swap = false;
	size_t chunk;
	int ret = 0;

#if defined(__LITTLE_ENDIAN)
	if (!mipi_dbi_spi_bpw_supported(spi, 16) && tr.bits_per_word == 16) {
		swap = true;
		tr.bits_per_word = 8;
	}
#endif
	max_chunk = mipi_dbi_spi_clamp_size(spi, max_chunk);

#ifdef VERBOSE_DEBUG
	DRM_DEBUG("%s: dc=%d, max_chunk=%zu, transfers:\n",
		  dev_name(&spi->dev), dc, max_chunk);
#endif
	gpiod_set_value_cansleep(mspi->dc, dc);

	spi_message_init_with_transfers(&m, &tr, 1);

	while (len) {
		chunk = min(len, max_chunk);

		tr.tx_buf = buf;
		tr.len = chunk;

		if (swap) {
			const u16 *buf16 = buf;
			unsigned int i;

			if (!swap_buf)
				swap_buf = kmalloc(chunk, GFP_KERNEL);
			if (!swap_buf)
				return -ENOMEM;

			for (i = 0; i < chunk / 2; i++)
				swap_buf[i] = swab16(buf16[i]);

			tr.tx_buf = swap_buf;
		}

		buf += chunk;
		len -= chunk;

		mipi_dbi_vdbg_spi_message(spi, &m);
		ret = spi_sync(spi, &m);
		if (ret)
			goto err_free;
	};

err_free:
	kfree(swap_buf);

	return ret;
}

static int mipi_dbi_spi3_gather_write(void *context, const void *reg,
				      size_t reg_len, const void *val,
				      size_t val_len)
{
	struct mipi_dbi_spi *mspi = context;
	size_t val_bytes = regmap_get_val_bytes(mspi->map);
	unsigned int regnr;
	int ret;

	if (reg_len == 1)
		regnr = *(u8 *) reg;
	else if (reg_len == 2)
		regnr = *(u16 *) reg;
	else
		return -EINVAL;

	if (regnr == mspi->ram_reg)
		val_bytes = 2;

	mspi_debug(reg, reg_len, val, val_len, val_bytes);

	ret = mipi_dbi_spi3_transfer(mspi, reg_len * 8, 0, reg, reg_len, 4096);
	if (ret)
		return ret;

	if (val && val_len)
		ret = mipi_dbi_spi3_transfer(mspi, val_bytes * 8, 1, val,
					     val_len, 4096);

	return ret;
}

static int mipi_dbi_spi3_write(void *context, const void *data, size_t count)
{
	return mipi_dbi_spi3_gather_write(context, data, 1,
					  data + 1, count - 1);
}

static int mipi_dbi_spi3_read(void *context, const void *reg, size_t reg_size,
			      void *val, size_t val_size)
{
	struct mipi_dbi_spi *mspi = context;
	struct spi_device *spi = mspi->context;
	u32 speed_hz = min_t(u32, MIPI_DBI_DEFAULT_SPI_READ_SPEED,
			     spi->max_speed_hz / 2);
	struct spi_transfer tr[2] = {
		{
			.speed_hz = speed_hz,
			.tx_buf = reg,
			.len = 1,
		}, {
			.speed_hz = speed_hz,
			.len = val_size,
		},
	};
	struct spi_message m;
	u8 cmd = *(u8 *)reg;
	u8 *buf;
	int ret;

	if (mspi->write_only)
		return -EACCES;

#ifdef VERBOSE_DEBUG
	DRM_DEBUG("%s: regnr=0x%02x, dc=0, len=%zu, transfers:\n",
		  dev_name(&spi->dev), *(u8 *)reg, val_size);
#endif

	/*
	 * Support non-standard 24-bit and 32-bit Nokia read commands which
	 * starts with a dummy clock, so we need to read an extra byte.
	 */
	if (cmd == MIPI_DCS_GET_DISPLAY_ID ||
	    cmd == MIPI_DCS_GET_DISPLAY_STATUS) {
		if (!(val_size == 3 || val_size == 4))
			return -EINVAL;

		tr[1].len = val_size + 1;
	}

	buf = kmalloc(tr[1].len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	tr[1].rx_buf = buf;
	gpiod_set_value_cansleep(mspi->dc, 0);

	/*
	 * Can't use spi_write_then_read() because reading speed is slower
	 * than writing speed and that is set on the transfer.
	 */
	spi_message_init_with_transfers(&m, tr, ARRAY_SIZE(tr));
	ret = spi_sync(spi, &m);
	mipi_dbi_vdbg_spi_message(spi, &m);

	if (tr[1].len == val_size) {
		memcpy(val, buf, val_size);
	} else {
		u8 *data = val;
		unsigned int i;

		for (i = 0; i < val_size; i++)
			data[i] = (buf[i] << 1) | !!(buf[i + 1] & BIT(7));
	}
	kfree(buf);

	return ret;
}

/* MIPI DBI Type C Option 3 */
static const struct regmap_bus mipi_dbi_regmap_bus3 = {
	.write = mipi_dbi_spi3_write,
	.gather_write = mipi_dbi_spi3_gather_write,
	.read = mipi_dbi_spi3_read,
	.reg_format_endian_default = REGMAP_ENDIAN_DEFAULT,
	.val_format_endian_default = REGMAP_ENDIAN_DEFAULT,
};

int mipi_dbi_spi_init(struct mipi_dbi *mipi, struct spi_device *spi,
		      struct gpio_desc *dc, struct gpio_desc *reset,
		      bool write_only)
{
	struct regmap_config config = {
		.reg_bits = 8,
		.val_bits = 8,
		.cache_type = REGCACHE_NONE,
	};
	struct device *dev = &spi->dev;
	struct mipi_dbi_spi *mspi;
	struct regmap *map;

	mspi = devm_kzalloc(dev, sizeof(*mspi), GFP_KERNEL);
	if (!mspi)
		return -ENOMEM;

	if (dc)
		map = devm_regmap_init(dev, &mipi_dbi_regmap_bus3, mspi,
				       &config);
	else
		map = devm_regmap_init(dev, &mipi_dbi_regmap_bus1, mspi,
				       &config);
	if (IS_ERR(map))
		return PTR_ERR(map);

	mspi->ram_reg = MIPI_DCS_WRITE_MEMORY_START;
	mspi->context = spi;
	mspi->map = map;
	mipi->reg = map;
	mspi->dc = dc;
	mspi->write_only = write_only;

	mipi->reset = reset;

	return 0;
}
EXPORT_SYMBOL(mipi_dbi_spi_init);





/**
 * DOC: overview
 *
 * This library provides helpers for MIPI display controllers with
 * Display Bus Interface (DBI).
 *
 * Many controllers are MIPI compliant and can use this library.
 * If a controller uses registers 0x2A and 0x2B to set the area to update
 * and uses register 0x2C to write to frame memory, it is most likely MIPI
 * compliant.
 */

/**
 * mipi_dbi_dirty - framebuffer dirty callback
 * @fb: framebuffer
 * @cma_obj: CMA buffer object
 * @flags: dirty fb ioctl flags
 * @color: color for annotated clips
 * @clips: dirty clip rectangles
 * @num_clips: number of @clips
 *
 * This function provides framebuffer flushing for MIPI DBI controllers.
 * Drivers should use this as their &tinydrm_funcs ->dirty callback.
 */
int mipi_dbi_dirty(struct drm_framebuffer *fb,
		   struct drm_gem_cma_object *cma_obj,
		   unsigned flags, unsigned color,
		   struct drm_clip_rect *clips, unsigned num_clips)
{
	struct tinydrm_device *tdev = drm_to_tinydrm(fb->dev);
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);
	struct regmap *reg = mipi->reg;
	struct drm_clip_rect clip;
	int ret;

	tinydrm_merge_clips(&clip, clips, num_clips, flags,
			    fb->width, fb->height);

	clip.x1 = 0;
	clip.x2 = fb->width;

	DRM_DEBUG("Flushing [FB:%d] x1=%u, x2=%u, y1=%u, y2=%u\n", fb->base.id,
		  clip.x1, clip.x2, clip.y1, clip.y2);

	tinydrm_debugfs_dirty_begin(tdev, fb, &clip);

	mipi_dbi_write(reg, MIPI_DCS_SET_COLUMN_ADDRESS,
		       (clip.x1 >> 8) & 0xFF, clip.x1 & 0xFF,
		       (clip.x2 >> 8) & 0xFF, (clip.x2 - 1) & 0xFF);
	mipi_dbi_write(reg, MIPI_DCS_SET_PAGE_ADDRESS,
		       (clip.y1 >> 8) & 0xFF, clip.y1 & 0xFF,
		       (clip.y2 >> 8) & 0xFF, (clip.y2 - 1) & 0xFF);

	ret = tinydrm_regmap_flush_rgb565(reg, MIPI_DCS_WRITE_MEMORY_START,
					  fb, cma_obj->vaddr, &clip);
	if (ret)
		dev_err_once(fb->dev->dev, "Failed to update display %d\n",
			     ret);

	tinydrm_debugfs_dirty_end(tdev, 0, 16);

	return ret;
}
EXPORT_SYMBOL(mipi_dbi_dirty);

/**
 * mipi_dbi_enable_backlight - mipi enable backlight helper
 * @tdev: tinydrm device
 *
 * Helper to enable &mipi_dbi ->backlight for the &tinydrm_funcs ->enable
 * callback.
 */
int mipi_dbi_enable_backlight(struct tinydrm_device *tdev)
{
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);

	return tinydrm_enable_backlight(mipi->backlight);
}
EXPORT_SYMBOL(mipi_dbi_enable_backlight);

static void mipi_dbi_blank(struct mipi_dbi *mipi)
{
	struct drm_device *drm = &mipi->tinydrm.drm;
	int height = drm->mode_config.min_height;
	int width = drm->mode_config.min_width;
	unsigned num_pixels = width * height;
	struct regmap *reg = mipi->reg;
	u16 *buf;

	buf = kzalloc(num_pixels * 2, GFP_KERNEL);
	if (!buf)
		return;

	mipi_dbi_write(reg, MIPI_DCS_SET_COLUMN_ADDRESS, 0, 0,
		       (width >> 8) & 0xFF, (width - 1) & 0xFF);
	mipi_dbi_write(reg, MIPI_DCS_SET_PAGE_ADDRESS, 0, 0,
		       (height >> 8) & 0xFF, (height - 1) & 0xFF);
	regmap_raw_write(reg, MIPI_DCS_WRITE_MEMORY_START, buf,
			 num_pixels * 2);
	kfree(buf);
}

/**
 * mipi_dbi_disable_backlight - mipi disable backlight helper
 * @tdev: tinydrm device
 *
 * Helper to disable &mipi_dbi ->backlight for the &tinydrm_funcs
 * ->disable callback.
 * If there's no backlight nor power control, blank display by writing zeroes.
 */
void mipi_dbi_disable_backlight(struct tinydrm_device *tdev)
{
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);

	if (mipi->backlight)
		tinydrm_disable_backlight(mipi->backlight);
	else if (!mipi->regulator)
		mipi_dbi_blank(mipi);
}
EXPORT_SYMBOL(mipi_dbi_disable_backlight);

/**
 * mipi_dbi_unprepare - mipi power off helper
 * @tdev: tinydrm device
 *
 * Helper to power off a MIPI controller.
 * Puts the controller in sleep mode if backlight control is enabled. It's done
 * like this to make sure we don't have backlight glaring through a panel with
 * all pixels turned off. If a regulator is registered it will be disabled.
 * Drivers can use this as their &tinydrm_funcs ->unprepare callback.
 */
void mipi_dbi_unprepare(struct tinydrm_device *tdev)
{
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);
	struct regmap *reg = mipi->reg;

	if (mipi->backlight) {
		mipi_dbi_write(reg, MIPI_DCS_SET_DISPLAY_OFF);
		mipi_dbi_write(reg, MIPI_DCS_ENTER_SLEEP_MODE);
	} else if (!mipi->regulator) {
		mipi->prepared_once = true;
	}

	if (mipi->regulator)
		regulator_disable(mipi->regulator);
}
EXPORT_SYMBOL(mipi_dbi_unprepare);

static const uint32_t mipi_dbi_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

int tinydrm_rotate_mode(struct drm_display_mode *mode, unsigned int rotation)
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

/**
 * mipi_dbi_init - MIPI DBI initialization
 * @dev: parent device
 * @mipi: &mipi_dbi structure to initialize
 * @reg: LCD register
 * @driver: DRM driver
 * @mode: display mode
 * @rotation: initial rotation in degress Counter Clock Wise
 *
 * This function initializes a &mipi_dbi structure and it's underlying
 * @tinydrm_device and &drm_device. It also sets up the display pipeline.
 * Native RGB565 format is supported and XRGB8888 is emulated.
 * Objects created by this function will be automatically freed on driver
 * detach (devres).
 */
int mipi_dbi_init(struct device *dev, struct mipi_dbi *mipi,
		  struct drm_driver *driver,
		  const struct drm_display_mode *mode, unsigned int rotation)
{
	struct tinydrm_device *tdev = &mipi->tinydrm;
	struct drm_display_mode *mode_copy;
	struct drm_device *drm;
	int ret;

	mode_copy = devm_kmalloc(dev, sizeof(*mode_copy), GFP_KERNEL);
	if (!mode_copy)
		return -ENOMEM;

	*mode_copy = *mode;
	mipi->rotation = rotation;
	ret = tinydrm_rotate_mode(mode_copy, rotation);
	if (ret) {
		DRM_ERROR("Illegal rotation value %u\n", rotation);
		return -EINVAL;
	}

	ret = devm_tinydrm_init(dev, tdev, driver);
	if (ret)
		return ret;

	drm = &tdev->drm;
	drm->mode_config.min_width = mode_copy->hdisplay;
	drm->mode_config.max_width = mode_copy->hdisplay;
	drm->mode_config.min_height = mode_copy->vdisplay;
	drm->mode_config.max_height = mode_copy->vdisplay;
	drm->mode_config.preferred_depth = 16;

	ret = tinydrm_display_pipe_init(tdev, mipi_dbi_formats,
					ARRAY_SIZE(mipi_dbi_formats),
					mode_copy, DRM_MODE_DIRTY_ON);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	tinydrm_debugfs_dirty_init(tdev);

	DRM_DEBUG_KMS("preferred_depth=%u, rotation = %u\n",
		      drm->mode_config.preferred_depth, rotation);

	return 0;
}
EXPORT_SYMBOL(mipi_dbi_init);

void mipi_dbi_hw_reset(struct mipi_dbi *mipi)
{
	if (!mipi->reset)
		return;

	gpiod_set_value_cansleep(mipi->reset, 0);
	msleep(20);
	gpiod_set_value_cansleep(mipi->reset, 1);
	msleep(120);
}
EXPORT_SYMBOL(mipi_dbi_hw_reset);

/**
 * mipi_dbi_display_is_on - check if display is on
 * @reg: LCD register
 *
 * This function checks the Power Mode register (if readable) to see if
 * display output is turned on. This can be used to see if the bootloader
 * has already turned on the display avoiding flicker when the pipeline is
 * enabled.
 *
 * Returns:
 * true if the display can be verified to be on
 * false otherwise.
 */
bool mipi_dbi_display_is_on(struct regmap *reg)
{
	u8 val;

	if (regmap_raw_read(reg, MIPI_DCS_GET_POWER_MODE, &val, 1))
		return false;

	val &= ~DCS_POWER_MODE_RESERVED_MASK;

	if (val != (DCS_POWER_MODE_DISPLAY |
	    DCS_POWER_MODE_DISPLAY_NORMAL_MODE | DCS_POWER_MODE_SLEEP_MODE))
		return false;

	DRM_DEBUG_DRIVER("Display is ON\n");

	return true;
}
EXPORT_SYMBOL(mipi_dbi_display_is_on);

/**
 * mipi_dbi_debug_dump_regs - dump some MIPI DCS registers
 * @reg: LCD register
 *
 * Dump some MIPI DCS registers using DRM_DEBUG_DRIVER().
 */
void mipi_dbi_debug_dump_regs(struct regmap *reg)
{
	u8 val[4];
	int ret;

	if (!(drm_debug & DRM_UT_DRIVER))
		return;

	ret = regmap_raw_read(reg, MIPI_DCS_GET_DISPLAY_ID, val, 3);
	if (ret) {
		struct device *dev = regmap_get_device(reg);

		dev_warn(dev, "failed to read from controller: %d", ret);
		return;
	}

	DRM_DEBUG_DRIVER("Display ID (%02x): %02x %02x %02x\n",
			 MIPI_DCS_GET_DISPLAY_ID, val[0], val[1], val[2]);

	regmap_raw_read(reg, MIPI_DCS_GET_DISPLAY_STATUS, val, 4);
	DRM_DEBUG_DRIVER("Display status (%02x): %02x %02x %02x %02x\n",
			 MIPI_DCS_GET_DISPLAY_STATUS, val[0], val[1], val[2], val[3]);

	regmap_raw_read(reg, MIPI_DCS_GET_POWER_MODE, val, 1);
	DRM_DEBUG_DRIVER("Power mode (%02x): %02x\n",
			 MIPI_DCS_GET_POWER_MODE, val[0]);

	regmap_raw_read(reg, MIPI_DCS_GET_ADDRESS_MODE, val, 1);
	DRM_DEBUG_DRIVER("Address mode (%02x): %02x\n",
			 MIPI_DCS_GET_ADDRESS_MODE, val[0]);

	regmap_raw_read(reg, MIPI_DCS_GET_PIXEL_FORMAT, val, 1);
	DRM_DEBUG_DRIVER("Pixel format (%02x): %02x\n",
			 MIPI_DCS_GET_PIXEL_FORMAT, val[0]);

	regmap_raw_read(reg, MIPI_DCS_GET_DISPLAY_MODE, val, 1);
	DRM_DEBUG_DRIVER("Display mode (%02x): %02x\n",
			 MIPI_DCS_GET_DISPLAY_MODE, val[0]);

	regmap_raw_read(reg, MIPI_DCS_GET_SIGNAL_MODE, val, 1);
	DRM_DEBUG_DRIVER("Display signal mode (%02x): %02x\n",
			 MIPI_DCS_GET_SIGNAL_MODE, val[0]);

	regmap_raw_read(reg, MIPI_DCS_GET_DIAGNOSTIC_RESULT, val, 1);
	DRM_DEBUG_DRIVER("Diagnostic result (%02x): %02x\n",
			 MIPI_DCS_GET_DIAGNOSTIC_RESULT, val[0]);
}
EXPORT_SYMBOL(mipi_dbi_debug_dump_regs);

#ifdef CONFIG_DEBUG_FS

static bool mipi_dbi_debugfs_readreg(struct seq_file *m, struct regmap *reg,
				     unsigned regnr, const char *desc,
				     u8 *buf, size_t len)
{
	int ret;

	ret = regmap_raw_read(reg, regnr, buf, len);
	if (ret) {
		seq_printf(m, "\n%s: command %02Xh failed: %d \n", desc, regnr,
			   ret);
		return false;
	} else {
		seq_printf(m, "\n%s (%02Xh=%*phN):\n", desc, regnr, len, buf);
	}

	return true;
}

static void
seq_bit_val(struct seq_file *m, const char *desc, u32 val, u8 bit)
{
	bool bit_val = !!(val & BIT(bit));

	seq_printf(m, "    D%u=%u: %s\n", bit, bit_val, desc);
}

static void
seq_bit_reserved(struct seq_file *m, u32 val, u8 end, u8 start)
{
	int i;

	for (i = end; i >= start; i--)
		seq_bit_val(m, "Reserved", val, i);
}

static void
seq_bit_array(struct seq_file *m, const char *desc, u32 val, u8 end, u8 start)
{
	u32 bits_val = (val & GENMASK(end, start)) >> start;
	int i;

	seq_printf(m, "    D[%u:%u]=%u: %s ", end, start, bits_val, desc);
	for (i = end; i >= start; i--)
		seq_printf(m, "%u ", !!(val & BIT(i)));

	seq_putc(m, '\n');
}

static void
seq_bit_text(struct seq_file *m, const char *desc, u32 val, u8 bit, const char *on, const char *off)
{
	bool bit_val = val & BIT(bit);

	seq_printf(m, "    D%u=%u: %s %s\n", bit, bit_val, desc, bit_val ? on : off);
}

static inline void
seq_bit_on_off(struct seq_file *m, const char *desc, u32 val, u8 bit)
{
	seq_bit_text(m, desc, val, bit, "On", "Off");
}

static char *mipi_pixel_format_str(u8 val)
{
	switch (val) {
	case 0:
		return "Reserved";
	case 1:
		return "3 bits/pixel";
	case 2:
		return "8 bits/pixel";
	case 3:
		return "12 bits/pixel";
	case 4:
		return "Reserved";
	case 5:
		return "16 bits/pixel";
	case 6:
		return "18 bits/pixel";
	case 7:
		return "24 bits/pixel";
	default:
		return "Illegal format";
	}
}

static int mipi_dbi_debugfs_show(struct seq_file *m, void *arg)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *drm = node->minor->dev;
	struct tinydrm_device *tdev = drm_to_tinydrm(drm);
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);
	struct regmap *reg = mipi->reg;
	u8 buf[4];
	u8 val8;
	int ret;

	ret = regmap_raw_read(reg, MIPI_DCS_GET_POWER_MODE, buf, 1);
	if (ret == -EACCES || ret == -ENOTSUPP) {
		seq_printf(m, "Controller is write-only\n");
		return 0;
	}

	/*
	 * Read Display ID (04h) and Read Display Status (09h) are
	 * non-standard commands that Nokia wanted back in the day,
	 * so most vendors implemented them.
	 */
	if (mipi_dbi_debugfs_readreg(m, reg, MIPI_DCS_GET_DISPLAY_ID,
				     "Display ID", buf, 3)) {
		seq_printf(m, "    ID1 = 0x%02x\n", buf[0]);
		seq_printf(m, "    ID2 = 0x%02x\n", buf[1]);
		seq_printf(m, "    ID3 = 0x%02x\n", buf[2]);
	}

	if (mipi_dbi_debugfs_readreg(m, reg, MIPI_DCS_GET_DISPLAY_STATUS,
				     "Display status", buf, 4)) {
		u32 stat;

		stat = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];

		seq_bit_on_off(m, "Booster voltage status:", stat, 31);
		seq_bit_val(m, "Row address order", stat, 30);
		seq_bit_val(m, "Column address order", stat, 29);
		seq_bit_val(m, "Row/column exchange", stat, 28);
		seq_bit_text(m, "Vertical refresh:", stat, 27,
			     "Bottom to Top", "Top to Bottom");
		seq_bit_text(m, "RGB/BGR order:", stat, 26, "BGR", "RGB");
		seq_bit_text(m, "Horizontal refresh order:", stat, 25,
			     "Right to Left", "Left to Right");
		seq_bit_reserved(m, stat, 24, 23);
		seq_bit_array(m, "Interface color pixel format:", stat, 22, 20);
		seq_bit_on_off(m, "Idle mode:", stat, 19);
		seq_bit_on_off(m, "Partial mode:", stat, 18);
		seq_bit_text(m, "Sleep:", stat, 17, "Out", "In");
		seq_bit_on_off(m, "Display normal mode:", stat, 16);
		seq_bit_on_off(m, "Vertical scrolling status:", stat, 15);
		seq_bit_reserved(m, stat, 14, 14);
		seq_bit_val(m, "Inversion status", stat, 13);
		seq_bit_val(m, "All pixel ON", stat, 12);
		seq_bit_val(m, "All pixel OFF", stat, 11);
		seq_bit_on_off(m, "Display:", stat, 10);
		seq_bit_on_off(m, "Tearing effect line:", stat, 9);
		seq_bit_array(m, "Gamma curve selection:", stat, 8, 6);
		seq_bit_text(m, "Tearing effect line mode:", stat, 5,
			     "Mode 2, both H-Blanking and V-Blanking",
			     "Mode 1, V-Blanking only");
		seq_bit_reserved(m, stat, 4, 0);
	}

	if (mipi_dbi_debugfs_readreg(m, reg, MIPI_DCS_GET_POWER_MODE,
				     "Power mode", &val8, 1)) {
		seq_bit_text(m, "Booster", val8, 7, "On", "Off or faulty");
		seq_bit_on_off(m, "Idle Mode", val8, 6);
		seq_bit_on_off(m, "Partial Mode", val8, 5);
		seq_bit_text(m, "Sleep", val8, 4, "Out Mode", "In Mode");
		seq_bit_on_off(m, "Display Normal Mode", val8, 3);
		seq_bit_on_off(m, "Display is", val8, 2);
		seq_bit_reserved(m, val8, 1, 0);
	}

	if (mipi_dbi_debugfs_readreg(m, reg, MIPI_DCS_GET_ADDRESS_MODE,
				     "Address mode", &val8, 1)) {
		seq_bit_text(m, "Page Address Order:", val8, 7,
			     "Bottom to Top", "Top to Bottom");
		seq_bit_text(m, "Column Address Order:", val8, 6,
			     "Right to Left", "Left to Right");
		seq_bit_text(m, "Page/Column Order:", val8, 5,
			     "Reverse Mode", "Normal Mode");
		seq_bit_text(m, "Line Address Order: LCD Refresh", val8, 4,
			     "Bottom to Top", "Top to Bottom");
		seq_bit_text(m, "RGB/BGR Order:", val8, 3, "BGR", "RGB");
		seq_bit_text(m, "Display Data Latch Data Order: LCD Refresh",
			     val8, 2, "Right to Left", "Left to Right");
		seq_bit_reserved(m, val8, 1, 0);
	}

	if (mipi_dbi_debugfs_readreg(m, reg, MIPI_DCS_GET_PIXEL_FORMAT,
				     "Pixel format", &val8, 1)) {
		u8 dpi = (val8 >> 4) & 0x7;
		u8 dbi = val8 & 0x7;

		seq_bit_reserved(m, val8, 7, 7);
		seq_printf(m, "    D[6:4]=%u: DPI: %s\n", dpi,
			   mipi_pixel_format_str(dpi));
		seq_bit_reserved(m, val8, 3, 3);
		seq_printf(m, "    D[2:0]=%u: DBI: %s\n", dbi,
			   mipi_pixel_format_str(dbi));
	}

	if (mipi_dbi_debugfs_readreg(m, reg, MIPI_DCS_GET_DISPLAY_MODE,
				     "Image Mode", &val8, 1)) {
		u8 gc = val8 & 0x7;

		seq_bit_on_off(m, "Vertical Scrolling Status:", val8, 7);
		seq_bit_reserved(m, val8, 6, 6);
		seq_bit_on_off(m, "Inversion:", val8, 5);
		seq_bit_reserved(m, val8, 4, 3);
		seq_printf(m, "    D[2:0]=%u: Gamma Curve Selection: ", gc);
		if (gc < 4)
			seq_printf(m, "GC%u\n", gc);
		else
			seq_puts(m, "Reserved\n");
	}

	if (mipi_dbi_debugfs_readreg(m, reg, MIPI_DCS_GET_SIGNAL_MODE,
				     "Signal Mode", &val8, 1)) {
		seq_bit_on_off(m, "Tearing Effect Line:", val8, 7);
		seq_bit_text(m, "Tearing Effect Line Output Mode: Mode",
			     val8, 6, "2", "1");
		seq_bit_reserved(m, val8, 5, 0);
	}

	if (mipi_dbi_debugfs_readreg(m, reg, MIPI_DCS_GET_DIAGNOSTIC_RESULT,
				     "Diagnostic result", &val8, 1)) {
		seq_bit_text(m, "Register Loading Detection:", val8, 7,
			     "OK", "Fault or reset");
		seq_bit_text(m, "Functionality Detection:", val8, 6,
			     "OK", "Fault or reset");
		seq_bit_text(m, "Chip Attachment Detection:", val8, 5,
			     "Fault", "OK or unimplemented");
		seq_bit_text(m, "Display Glass Break Detection:", val8, 4,
			     "Fault", "OK or unimplemented");
		seq_bit_reserved(m, val8, 3, 0);
	}

	return 0;
}

static const struct drm_info_list mipi_dbi_debugfs_list[] = {
	{ "mipi",   mipi_dbi_debugfs_show, 0 },
};

int mipi_dbi_debugfs_init(struct drm_minor *minor)
{
	int ret;

	ret = tinydrm_debugfs_init(minor);
	if (ret)
		return ret;

	return drm_debugfs_create_files(mipi_dbi_debugfs_list,
					ARRAY_SIZE(mipi_dbi_debugfs_list),
					minor->debugfs_root, minor);
}
EXPORT_SYMBOL(mipi_dbi_debugfs_init);

void mipi_dbi_debugfs_cleanup(struct drm_minor *minor)
{
	tinydrm_debugfs_cleanup(minor);
	drm_debugfs_remove_files(mipi_dbi_debugfs_list,
				 ARRAY_SIZE(mipi_dbi_debugfs_list), minor);
}
EXPORT_SYMBOL(mipi_dbi_debugfs_cleanup);

#endif

MODULE_LICENSE("GPL");
