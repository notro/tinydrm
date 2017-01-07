#define DEBUG
/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drmP.h>
#include <drm/tinydrm/tinydrm.h>
#include <drm/tinydrm/tinydrm-helpers.h>
#include <drm/tinydrm/tinydrm-regmap.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>

static unsigned int spi_max;
module_param(spi_max, uint, 0400);
MODULE_PARM_DESC(spi_max, "Set a lower SPI max transfer size");

#if IS_ENABLED(CONFIG_SPI)

/**
 * tinydrm_spi_max_transfer_size - Determine max SPI transfer size
 * @spi: SPI device
 * @max_len: Maximum buffer size needed (optional)
 *
 * This function returns the maximum size to use for SPI transfers. It checks
 * the SPI master, the optional @max_len and the module parameter spi_max and
 * returns the smallest.
 *
 * Returns:
 * Maximum size for SPI transfers
 */
size_t tinydrm_spi_max_transfer_size(struct spi_device *spi, size_t max_len)
{
	size_t ret;

	ret = min(spi_max_transfer_size(spi), spi->master->max_dma_len);
	if (max_len)
		ret = min(ret, max_len);
	if (spi_max)
		ret = min_t(size_t, ret, spi_max);
	ret &= ~0x3;
	if (ret < 4)
		ret = 4;

	return ret;
}
EXPORT_SYMBOL(tinydrm_spi_max_transfer_size);

/**
 * tinydrm_spi_bpw_supported - Check if bits per word is supported
 * @spi: SPI device
 * @bpw: Bits per word
 *
 * This function checks to see if the SPI master driver supports @bpw.
 *
 * Returns:
 * True if @bpw is supported, false otherwise.
 */
bool tinydrm_spi_bpw_supported(struct spi_device *spi, u8 bpw)
{
	u32 bpw_mask = spi->master->bits_per_word_mask;

	if (bpw == 8)
		return true;

	if (!bpw_mask) {
		dev_warn_once(&spi->dev,
			      "bits_per_word_mask not set, assume 8-bit only\n");
		return false;
	}

	if (bpw_mask & SPI_BPW_MASK(bpw))
		return true;

	return false;
}
EXPORT_SYMBOL(tinydrm_spi_bpw_supported);

/* hexdump that can do u16, useful on little endian where bytes are swapped */
static void tinydrm_hexdump(char *linebuf, size_t linebuflen, const void *buf,
			    size_t len, size_t bpw, size_t max)
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
		hex_dump_to_buffer(buf, len, max, 1, linebuf, linebuflen,
				   false);
	}
}

/* called through TINYDRM_DEBUG_REG_WRITE() */
void tinydrm_debug_reg_write(const void *reg, size_t reg_len, const void *val,
			     size_t val_len, size_t val_width)
{
	unsigned int regnr;

	if (reg_len != 1 && reg_len != 2)
		return;

	regnr = (reg_len == 1) ? *(u8 *)reg : *(u16 *)reg;

	if (val && val_len) {
		char linebuf[3 * 32];

		tinydrm_hexdump(linebuf, ARRAY_SIZE(linebuf), val, val_len,
				val_width, 16);
		drm_printk(KERN_DEBUG, DRM_UT_CORE,
			   "regnr=0x%0*x, data(%zu)= %s%s\n",
			   reg_len == 1 ? 2 : 4, regnr,
			   val_len, linebuf, val_len > 32 ? " ..." : "");
	} else {
		drm_printk(KERN_DEBUG, DRM_UT_CORE,
			   "regnr=0x%0*x\n",
			   reg_len == 1 ? 2 : 4, regnr);
	}
}
EXPORT_SYMBOL(tinydrm_debug_reg_write);

/* called through tinydrm_dbg_spi_message() */
void _tinydrm_dbg_spi_message(struct spi_device *spi, struct spi_message *m)
{
	struct spi_master *master = spi->master;
	struct spi_transfer *tmp;
	struct list_head *pos;
	char linebuf[3 * 32];
	int i = 0;

	list_for_each(pos, &m->transfers) {
		tmp = list_entry(pos, struct spi_transfer, transfer_list);

		if (tmp->tx_buf) {
			bool dma = false;

			if (master->can_dma)
				dma = master->can_dma(master, spi, tmp);

			tinydrm_hexdump(linebuf, ARRAY_SIZE(linebuf),
					tmp->tx_buf, tmp->len,
					tmp->bits_per_word, 16);
			printk(KERN_DEBUG "    tr[%i]: bpw=%i, dma=%u, len=%u, tx_buf(%p)=[%s%s]\n",
			       i, tmp->bits_per_word, dma, tmp->len,
			       tmp->tx_buf, linebuf,
			       tmp->len > 16 ? " ..." : "");
		}
		if (tmp->rx_buf) {
			tinydrm_hexdump(linebuf, ARRAY_SIZE(linebuf),
					tmp->rx_buf, tmp->len,
					tmp->bits_per_word, 16);
			printk(KERN_DEBUG "    tr[%i]: bpw=%i,        len=%u, rx_buf(%p)=[%s%s]\n",
			       i, tmp->bits_per_word, tmp->len, tmp->rx_buf,
			       linebuf, tmp->len > 16 ? " ..." : "");
		}
		i++;
	}
}
EXPORT_SYMBOL(_tinydrm_dbg_spi_message);

/**
 * tinydrm_spi_transfer - SPI transfer helper
 * @spi: SPI device
 * @speed_hz: Override speed (optional)
 * @header: Optional header transfer
 * @bpw: Bits per word
 * @buf: Buffer to transfer
 * @len: Buffer length
 * @swap_buf: Swap buffer used on Little Endian when 16 bpw is not supported
 * @max_chunk: Break up buffer into chunks of this size (optional)
 *
 * This SPI transfer helper breaks up the transfer of @buf into @max_chunk
 * chunks. If the machine is Little Endian and the SPI master driver doesn't
 * support @bpw=16, it swaps the bytes using @swap_buf and does a 8-bit
 * transfer. If @header is set, it is prepended to each SPI message.
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int tinydrm_spi_transfer(struct spi_device *spi, u32 speed_hz,
			 struct spi_transfer *header, u8 bpw, const void *buf,
			 size_t len, u16 *swap_buf, size_t max_chunk)
{
	struct spi_transfer tr = {
		.bits_per_word = bpw,
		.speed_hz = speed_hz,
	};
	struct spi_message m;
	bool swap = false;
	size_t chunk;
	int ret = 0;

	if (WARN_ON_ONCE(bpw != 8 && bpw != 16))
		return -EINVAL;

	max_chunk = tinydrm_spi_max_transfer_size(spi, max_chunk);

	if (drm_debug & DRM_UT_CORE)
		pr_debug("[drm:%s] bpw=%u, max_chunk=%zu, transfers:\n",
			 __func__, bpw, max_chunk);

	if (tinydrm_get_machine_endian() == REGMAP_ENDIAN_LITTLE &&
	    bpw == 16 && !tinydrm_spi_bpw_supported(spi, 16)) {
		if (!swap_buf)
			return -EINVAL;

		swap = true;
		tr.bits_per_word = 8;
	}

	spi_message_init(&m);
	if (header)
		spi_message_add_tail(header, &m);
	spi_message_add_tail(&tr, &m);

	while (len) {
		chunk = min(len, max_chunk);

		tr.tx_buf = buf;
		tr.len = chunk;

		if (swap) {
			const u16 *buf16 = buf;
			unsigned int i;

			for (i = 0; i < chunk / 2; i++)
				swap_buf[i] = swab16(buf16[i]);

			tr.tx_buf = swap_buf;
		}

		buf += chunk;
		len -= chunk;

		tinydrm_dbg_spi_message(spi, &m);
		ret = spi_sync(spi, &m);
		if (ret)
			return ret;
	};

	return 0;
}
EXPORT_SYMBOL(tinydrm_spi_transfer);

#endif /* CONFIG_SPI */
