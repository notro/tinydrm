//#define VERBOSE_DEBUG
//#define DEBUG
/*
 * Copyright (C) 2015 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <asm/unaligned.h>
#include <drm/tinydrm/lcdreg.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/spi/spi.h>

static unsigned txlen;
module_param(txlen, uint, 0);
MODULE_PARM_DESC(txlen, "Transmit chunk length");

static unsigned long bpwm;
module_param(bpwm, ulong, 0);
MODULE_PARM_DESC(bpwm, "Override SPI master bits_per_word_mask");

struct lcdreg_spi {
	struct lcdreg reg;
	enum lcdreg_spi_mode mode;
unsigned txbuflen;
	void *txbuf_dc;
	unsigned id;
	u8 (*startbyte)(struct lcdreg *reg, struct lcdreg_transfer *tr,
			bool read);
	struct gpio_desc *dc;
	struct gpio_desc *reset;
};

static inline struct lcdreg_spi *to_lcdreg_spi(struct lcdreg *reg)
{
	return reg ? container_of(reg, struct lcdreg_spi, reg) : NULL;
}

#ifdef VERBOSE_DEBUG
static void lcdreg_vdbg_dump_spi(const struct device *dev, struct spi_message *m, u8 *startbyte)
{
	struct spi_transfer *tmp;
	struct list_head *pos;
	int i = 0;

	if (startbyte)
		dev_dbg(dev, "spi_message: startbyte=0x%02X\n", startbyte[0]);
	else
		dev_dbg(dev, "spi_message:\n");

	list_for_each(pos, &m->transfers) {
		tmp = list_entry(pos, struct spi_transfer, transfer_list);
		if (tmp->tx_buf)
			pr_debug("    tr%i: bpw=%i, len=%u, tx_buf(%p)=[%*ph]\n", i, tmp->bits_per_word, tmp->len, tmp->tx_buf, tmp->len > 64 ? 64 : tmp->len, tmp->tx_buf);
		if (tmp->rx_buf)
			pr_debug("    tr%i: bpw=%i, len=%u, rx_buf(%p)=[%*ph]\n", i, tmp->bits_per_word, tmp->len, tmp->rx_buf, tmp->len > 64 ? 64 : tmp->len, tmp->rx_buf);
		i++;
	}
}
#else
static void lcdreg_vdbg_dump_spi(const struct device *dev, struct spi_message *m, u8 *startbyte)
{
}
#endif

static int lcdreg_spi_do_transfer(struct lcdreg *reg,
				  struct lcdreg_transfer *transfer)
{
	struct spi_device *sdev = to_spi_device(reg->dev);
	struct lcdreg_spi *spi = to_lcdreg_spi(reg);
	void *buf = transfer->buf;
	size_t len = transfer->count * lcdreg_bytes_per_word(transfer->width);
	size_t max = txlen ? : sdev->master->max_dma_len;
	size_t room_left_in_page = PAGE_SIZE - offset_in_page(buf);
	size_t chunk = min_t(size_t, len, max);
	struct spi_message m;
	struct spi_transfer *tr;
	u8 *startbuf = NULL;
	int ret, i;

	dev_dbg(reg->dev, "%s: index=%u, count=%u, width=%u\n",
		__func__, transfer->index, transfer->count, transfer->width);
	lcdreg_dbg_transfer_buf(transfer);

	tr = kzalloc(2 * sizeof(*tr), GFP_KERNEL);
	if (!tr)
		return -ENOMEM;

	/* slow down commands? */
	if (!transfer->index && (reg->quirks & LCDREG_SLOW_INDEX0_WRITE))
		for (i = 0; i < 2; i++)
			tr[i].speed_hz = min_t(u32, 2000000,
					       sdev->max_speed_hz / 2);

	if (spi->mode == LCDREG_SPI_STARTBYTE) {
		startbuf = kmalloc(1, GFP_KERNEL);
		if (!startbuf) {
			ret = -ENOMEM;
			goto out;
		}
		*startbuf = spi->startbyte(reg, transfer, false);
	}

	/*
	 * transfer->buf can be unaligned to the page boundary for partial
	 * updates when videomem is sent directly (no buffering).
	 * Spi core can sg map the buffer for dma and relies on vmalloc'ed
	 * memory to be page aligned.
	 */
//pr_debug("%s: PAGE_ALIGNED=%d, len > room_left_in_page= %d > %d = %d, chunk=%zu\n", __func__, PAGE_ALIGNED(buf), len, room_left_in_page, len > room_left_in_page, chunk);
	if (!PAGE_ALIGNED(buf) && len > room_left_in_page) {
//size_t chunk0 = chunk;

		if (chunk >= room_left_in_page) {
			chunk = room_left_in_page;
//pr_debug("%s: chunk: %zu -> %zu, room_left_in_page=%zu\n\n", __func__, chunk0, chunk, room_left_in_page);
		} else {
			chunk = room_left_in_page % chunk ? : chunk;
//pr_debug("%s: chunk: %zu -> %zu, room_left_in_page=%zu, room_left_in_page %% chunk=%zu\n\n", __func__, chunk0, chunk, room_left_in_page, room_left_in_page % chunk);
		}
	}

	do {
		i = 0;
		spi_message_init(&m);

		if (spi->mode == LCDREG_SPI_STARTBYTE) {
			tr[i].tx_buf = startbuf;
			tr[i].len = 1;
			tr[i].bits_per_word = 8;
			spi_message_add_tail(&tr[i++], &m);
		}

		tr[i].tx_buf = buf;
		tr[i].len = chunk;
		tr[i].bits_per_word = transfer->width;
		buf += chunk;
		len -= chunk;
		spi_message_add_tail(&tr[i], &m);

		lcdreg_vdbg_dump_spi(&sdev->dev, &m, startbuf);
		ret = spi_sync(sdev, &m);
		if (ret)
			goto out;

		chunk = min_t(size_t, len, max);
	} while (len);

out:
	kfree(tr);
	kfree(startbuf);

	return ret;
}

static int lcdreg_spi_transfer_emulate9(struct lcdreg *reg,
					struct lcdreg_transfer *transfer)
{
	struct lcdreg_spi *spi = to_lcdreg_spi(reg);
	struct lcdreg_transfer tr = {
		.index = transfer->index,
		.width = 8,
		.count = transfer->count,
	};
	u16 *src = transfer->buf;
	unsigned added = 0;
	int i, ret;
	u8 *dst;

	if (transfer->count % 8) {
		dev_err_once(reg->dev,
			     "transfer->count=%u must be divisible by 8\n",
			     transfer->count);
		return -EINVAL;
	}

	dst = kzalloc(spi->txbuflen, GFP_KERNEL);
	if (!dst)
		return -ENOMEM;

	tr.buf = dst;

	for (i = 0; i < transfer->count; i += 8) {
		u64 tmp = 0;
		int j, bits = 63;

		for (j = 0; j < 7; j++) {
			u64 bit9 = (*src & 0x100) ? 1 : 0;
			u64 val = *src++ & 0xFF;

			tmp |= bit9 << bits;
			bits -= 8;
			tmp |= val << bits--;
		}
		tmp |= ((*src & 0x100) ? 1 : 0);
		*(u64 *)dst = cpu_to_be64(tmp);
		dst += 8;
		*dst++ = *src++ & 0xFF;
		added++;
	}
	tr.count += added;
	ret = lcdreg_spi_do_transfer(reg, &tr);
	kfree(tr.buf);

	return ret;
}

static int lcdreg_spi_transfer_emulate16(struct lcdreg *reg,
					 struct lcdreg_transfer *transfer)
{
	struct lcdreg_spi *spi = to_lcdreg_spi(reg);
	unsigned to_copy, remain = transfer->count;
	struct lcdreg_transfer tr = {
		.index = transfer->index,
		.width = 8,
	};
	u16 *data16 = transfer->buf;
	u16 *txbuf16;
	int i, ret = 0;

	txbuf16 = kzalloc(spi->txbuflen, GFP_KERNEL);
	if (!txbuf16)
		return -ENOMEM;

	tr.buf = txbuf16;

	while (remain) {
		to_copy = min(remain, spi->txbuflen / 2);
		dev_dbg(reg->dev, "    to_copy=%zu, remain=%zu\n",
					to_copy, remain - to_copy);

		for (i = 0; i < to_copy; i++)
			txbuf16[i] = swab16(data16[i]);

		data16 = data16 + to_copy;
		tr.count = to_copy * 2;
		ret = lcdreg_spi_do_transfer(reg, &tr);
		if (ret < 0)
			goto out;
		remain -= to_copy;
	}

out:
	kfree(tr.buf);

	return ret;
}

static int lcdreg_spi_transfer(struct lcdreg *reg,
			       struct lcdreg_transfer *transfer)
{
	struct lcdreg_spi *spi = to_lcdreg_spi(reg);
	bool mach_little_endian;

#ifdef __LITTLE_ENDIAN
	mach_little_endian = true;
#endif
	if (spi->dc)
		gpiod_set_value_cansleep(spi->dc, transfer->index);

	if (lcdreg_bpw_supported(reg, transfer->width))
		return lcdreg_spi_do_transfer(reg, transfer);

	if (transfer->width == 9)
		return lcdreg_spi_transfer_emulate9(reg, transfer);

	if ((mach_little_endian == reg->little_endian) &&
	    (transfer->width % 8 == 0)) {
		/* the byte order matches */
		transfer->count *= transfer->width / 8;
		transfer->width = 8;
		return lcdreg_spi_do_transfer(reg, transfer);
	}

	if (mach_little_endian != reg->little_endian && transfer->width == 16)
		return lcdreg_spi_transfer_emulate16(reg, transfer);

	dev_err_once(reg->dev, "width=%u is not supported (%u:%u)\n",
		     transfer->width, mach_little_endian, reg->little_endian);

	return -EINVAL;
}

static int lcdreg_spi_write_9bit_dc(struct lcdreg *reg,
				    struct lcdreg_transfer *transfer)
{
	struct lcdreg_spi *spi = to_lcdreg_spi(reg);
	struct lcdreg_transfer tr = {
		.index = transfer->index,
	};
	u8 *data8 = transfer->buf;
	u16 *data16 = transfer->buf;
	unsigned width;
	u16 *txbuf16;
	unsigned remain;
	unsigned tx_array_size;
	unsigned to_copy;
	int pad, i, ret;

width = transfer->width;

	if (width != 8 && width != 16) {
		dev_err(reg->dev, "transfer width %u is not supported\n",
								width);
		return -EINVAL;
	}

	if (!spi->txbuf_dc) {
		spi->txbuf_dc = devm_kzalloc(reg->dev, spi->txbuflen,
							GFP_KERNEL);
		if (!spi->txbuf_dc)
			return -ENOMEM;
		dev_info(reg->dev, "allocated %u KiB 9-bit dc buffer\n",
						spi->txbuflen / 1024);
	}

	tr.buf = spi->txbuf_dc;
	txbuf16 = spi->txbuf_dc;
	remain = transfer->count;
	if (width == 8)
		tx_array_size = spi->txbuflen / 2;
	else
		tx_array_size = spi->txbuflen / 4;

	/* If we're emulating 9-bit, the buffer has to be divisible by 8.
	   Pad with no-ops if necessary (assuming here that zero is a no-op)
	   FIX: If video buf isn't divisible by 8, it will break.
	 */
	if (!lcdreg_bpw_supported(reg, 9) && width == 8 &&
						remain < tx_array_size) {
		pad = (transfer->count % 8) ? 8 - (transfer->count % 8) : 0;
		if (transfer->index == 0)
			for (i = 0; i < pad; i++)
				*txbuf16++ = 0x000;
		for (i = 0; i < remain; i++) {
			*txbuf16 = *data8++;
			if (transfer->index)
				*txbuf16++ |= 0x0100;
		}
		if (transfer->index == 1)
			for (i = 0; i < pad; i++)
				*txbuf16++ = 0x000;
		tr.width = 9;
		tr.count = pad + remain;
		return lcdreg_spi_transfer(reg, &tr);
	}

	while (remain) {
		to_copy = remain > tx_array_size ? tx_array_size : remain;
		remain -= to_copy;
		dev_dbg(reg->dev, "    to_copy=%zu, remain=%zu\n",
					to_copy, remain);

		if (width == 8) {
			for (i = 0; i < to_copy; i++) {
				txbuf16[i] = *data8++;
				if (transfer->index)
					txbuf16[i] |= 0x0100;
			}
		} else {
			for (i = 0; i < (to_copy * 2); i += 2) {
				txbuf16[i]     = *data16 >> 8;
				txbuf16[i + 1] = *data16++ & 0xFF;
				if (transfer->index) {
					txbuf16[i]     |= 0x0100;
					txbuf16[i + 1] |= 0x0100;
				}
			}
		}
		tr.buf = spi->txbuf_dc;
		tr.width = 9;
		tr.count = to_copy * 2;
		ret = lcdreg_spi_transfer(reg, &tr);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int lcdreg_spi_write(struct lcdreg *reg, unsigned regnr,
			    struct lcdreg_transfer *transfer)
{
	struct lcdreg_spi *spi = to_lcdreg_spi(reg);
	struct lcdreg_transfer tr = {
		.width = reg->def_width,
		.count = 1,
	};
	int ret;

	tr.buf = kmalloc(sizeof(u32), GFP_KERNEL);
	if (!tr.buf)
		return -ENOMEM;

	if (reg->def_width <= 8)
		((u8 *)tr.buf)[0] = regnr;
	else
		((u16 *)tr.buf)[0] = regnr;

	if (spi->mode == LCDREG_SPI_3WIRE)
		ret = lcdreg_spi_write_9bit_dc(reg, &tr);
	else
		ret = lcdreg_spi_transfer(reg, &tr);
	kfree(tr.buf);
	if (ret || !transfer || !transfer->count)
		return ret;

	if (!transfer->width)
		transfer->width = reg->def_width;
	if (spi->mode == LCDREG_SPI_3WIRE)
		ret = lcdreg_spi_write_9bit_dc(reg, transfer);
	else
		ret = lcdreg_spi_transfer(reg, transfer);

	return ret;
}

/*
   <CMD> <DM> <PA>
   CMD = Command
   DM = Dummy read
   PA = Parameter or display data

   ST7735R read:
     Parallel: <CMD> <DM> <PA>
     SPI: 8-bit plain, 24- and 32-bit needs 1 dummy clock cycle

   ILI9320:
     Parallel: no dummy read, page 51 in datasheet
     SPI (startbyte): One byte of invalid dummy data read after the start byte.

   ILI9340:
     Parallel: no info about dummy read
     SPI: same as ST7735R

   ILI9341:
     Parallel: no info about dummy read
     SPI: same as ST7735R

   SSD1289:
     Parallel: 1 dummy read

 */

static int lcdreg_spi_read_startbyte(struct lcdreg *reg, unsigned regnr,
				     struct lcdreg_transfer *transfer)
{
	struct spi_device *sdev = to_spi_device(reg->dev);
	struct lcdreg_spi *spi = to_lcdreg_spi(reg);
	struct spi_message m;
	struct spi_transfer trtx = {
		.speed_hz = min_t(u32, 2000000, sdev->max_speed_hz / 2),
		.bits_per_word = 8,
		.len = 1,
	};
	struct spi_transfer trrx = {
		.speed_hz = trtx.speed_hz,
		.bits_per_word = 8,
		.len = (transfer->count * 2) + 1,
	};
	u8 *txbuf, *rxbuf;
	int i, ret;

	if (!reg->readable)
		return -EACCES;

	if (!transfer || !transfer->count)
		return -EINVAL;

	transfer->width = transfer->width ? : reg->def_width;
	if (WARN_ON(transfer->width != 16))
		return -EINVAL;

	ret = lcdreg_writereg(reg, regnr);
	if (ret)
		return ret;

	txbuf = kzalloc(1, GFP_KERNEL);
	if (!txbuf)
		return -ENOMEM;

	rxbuf = kzalloc(trrx.len, GFP_KERNEL);
	if (!rxbuf) {
		kfree(txbuf);
		return -ENOMEM;
	}

	*txbuf = spi->startbyte(reg, transfer, true);

	trtx.tx_buf = txbuf;
	trrx.rx_buf = rxbuf;
	spi_message_init(&m);
	spi_message_add_tail(&trtx, &m);
	spi_message_add_tail(&trrx, &m);
	ret = spi_sync(sdev, &m);
	lcdreg_vdbg_dump_spi(&sdev->dev, &m, txbuf);
	kfree(txbuf);
	if (ret) {
		kfree(rxbuf);
		return ret;
	}

	rxbuf++;
	for (i = 0; i < transfer->count; i++) {
		((u16 *)transfer->buf)[i] = get_unaligned_be16(rxbuf);
		rxbuf += 2;
	}
	kfree(trrx.rx_buf);

	return 0;
}

static int lcdreg_spi_read(struct lcdreg *reg, unsigned regnr,
			   struct lcdreg_transfer *transfer)
{
	struct lcdreg_spi *spi = to_lcdreg_spi(reg);
	struct spi_device *sdev = to_spi_device(reg->dev);
	struct spi_message m;
	struct spi_transfer trtx = {
		.speed_hz = min_t(u32, 2000000, sdev->max_speed_hz / 2),
		.bits_per_word = reg->def_width,
		.len = 1,
	};
	struct spi_transfer trrx = {
		.speed_hz = trtx.speed_hz,
		.rx_buf = transfer->buf,
		.len = transfer->count,
	};
	void *txbuf = NULL;
	int i, ret;

	transfer->width = transfer->width ? : reg->def_width;
	if (WARN_ON(transfer->width != reg->def_width || !transfer->count))
		return -EINVAL;

	if (!reg->readable)
		return -EACCES;

	txbuf = kzalloc(16, GFP_KERNEL);
	if (!txbuf)
		return -ENOMEM;

	spi_message_init(&m);
	trtx.tx_buf = txbuf;
	trrx.bits_per_word = transfer->width;

	if (spi->mode == LCDREG_SPI_4WIRE) {
		if (trtx.bits_per_word == 8) {
			*(u8 *)txbuf = regnr;
		} else if (trtx.bits_per_word == 16) {
			if (lcdreg_bpw_supported(reg, trtx.bits_per_word)) {
				*(u16 *)txbuf = regnr;
			} else {
				*(u16 *)txbuf = cpu_to_be16(regnr);
				trtx.bits_per_word = 8;
				trtx.len = 2;
			}
		} else {
			return -EINVAL;
		}
		gpiod_set_value_cansleep(spi->dc, 0);
	} else if (spi->mode == LCDREG_SPI_3WIRE) {
		if (lcdreg_bpw_supported(reg, 9)) {
			trtx.bits_per_word = 9;
			*(u16 *)txbuf = regnr; /* dc=0 */
		} else {
			/* 8x 9-bit words, pad with leading zeroes (no-ops) */
			((u8 *)txbuf)[8] = regnr;
		}
	} else {
		kfree(txbuf);
		return -EINVAL;
	}
	spi_message_add_tail(&trtx, &m);

	if (spi->mode == LCDREG_SPI_4WIRE && transfer->index) {
		trtx.cs_change = 1; /* not always supported */
		lcdreg_vdbg_dump_spi(&sdev->dev, &m, NULL);
		ret = spi_sync(sdev, &m);
		if (ret) {
			kfree(txbuf);
			return ret;
		}
		gpiod_set_value_cansleep(spi->dc, 1);
		spi_message_init(&m);
	}

	spi_message_add_tail(&trrx, &m);
	ret = spi_sync(sdev, &m);
	lcdreg_vdbg_dump_spi(&sdev->dev, &m, NULL);
	kfree(txbuf);
	if (ret)
		return ret;

	if (!lcdreg_bpw_supported(reg, trrx.bits_per_word) &&
						(trrx.bits_per_word == 16))
		for (i = 0; i < transfer->count; i++)
			((u16 *)transfer->buf)[i] = be16_to_cpu(((u16 *)transfer->buf)[i]);

	return 0;
}

static void lcdreg_spi_reset(struct lcdreg *reg)
{
	struct lcdreg_spi *spi = to_lcdreg_spi(reg);

	if (!spi->reset)
		return;

	dev_info(reg->dev, "%s()\n", __func__);
	gpiod_set_value_cansleep(spi->reset, 0);
	msleep(20);
	gpiod_set_value_cansleep(spi->reset, 1);
	msleep(120);
}

/* Default startbyte implementation: | 0 | 1 | 1 | 1 | 0 | ID | RS | RW | */
static u8 lcdreg_spi_startbyte(struct lcdreg *reg, struct lcdreg_transfer *tr,
			       bool read)
{
	struct lcdreg_spi *spi = to_lcdreg_spi(reg);

	return 0x70 | (!!spi->id << 2) | (!!tr->index << 1) | read;
}

int devm_lcdreg_spi_of_parse(struct device *dev, struct lcdreg_spi_config *cfg)
{
	char *dc_name = cfg->dc_name ? : "dc";
	int ret;

	cfg->reset = devm_gpiod_get_optional(dev, "reset",
					     GPIOD_OUT_HIGH);
	if (IS_ERR(cfg->reset))
		return PTR_ERR(cfg->reset);

	switch (cfg->mode) {
	case LCDREG_SPI_4WIRE:
		cfg->dc = devm_gpiod_get_optional(dev, dc_name,
						  GPIOD_OUT_LOW);
		if (IS_ERR(cfg->dc))
			return PTR_ERR(cfg->dc);
		break;
	case LCDREG_SPI_STARTBYTE:
		ret = of_property_read_u32(dev->of_node, "id", &cfg->id);
		if (ret && ret != -EINVAL) {
			dev_err(dev, "error reading property 'id': %i\n", ret);
			return ret;
		}
		break;
	default:
		break;
	}

	return 0;
}
EXPORT_SYMBOL(devm_lcdreg_spi_of_parse);

struct lcdreg *devm_lcdreg_spi_init(struct spi_device *sdev,
				    const struct lcdreg_spi_config *config)
{
	struct device *dev = &sdev->dev;
	struct lcdreg_spi *spi;
	struct lcdreg *reg;

	if (txlen) {
		if (txlen < PAGE_SIZE) {
			txlen = rounddown_pow_of_two(txlen);
			if (txlen < 64)
				txlen = 64;
		} else {
			txlen &= PAGE_MASK;
		}
	}
dev_info(dev, "txlen: %u\n", txlen);

	spi = devm_kzalloc(dev, sizeof(*spi), GFP_KERNEL);
	if (!spi)
		return ERR_PTR(-ENOMEM);

	reg = &spi->reg;
	if (bpwm) {
		reg->bits_per_word_mask = bpwm;
	} else {
		if (sdev->master->bits_per_word_mask)
			reg->bits_per_word_mask = sdev->master->bits_per_word_mask;
		else
			reg->bits_per_word_mask = SPI_BPW_MASK(8);
	}
	dev_dbg(dev, "bits_per_word_mask: 0x%04x", reg->bits_per_word_mask);
	spi->mode = config->mode;
	reg->def_width = config->def_width;
	reg->readable = config->readable;
	reg->reset = lcdreg_spi_reset;
	reg->write = lcdreg_spi_write;
	if (spi->mode == LCDREG_SPI_STARTBYTE) {
		spi->startbyte = config->startbyte ? : lcdreg_spi_startbyte;
		reg->read = lcdreg_spi_read_startbyte;
	} else {
		reg->read = lcdreg_spi_read;
	}

	if (!spi->txbuflen)
		spi->txbuflen = PAGE_SIZE;

	spi->id = config->id;
	spi->reset = config->reset;
	spi->dc = config->dc;
	if (spi->mode == LCDREG_SPI_4WIRE && !spi->dc) {
		dev_err(dev, "missing 'dc' gpio\n");
		return ERR_PTR(-EINVAL);
	}

	pr_debug("spi->reg.def_width: %u\n", reg->def_width);
	if (spi->reset)
		pr_debug("spi->reset: %i\n", desc_to_gpio(spi->reset));
	if (spi->dc)
		pr_debug("spi->dc: %i\n", desc_to_gpio(spi->dc));
	pr_debug("spi->mode: %u\n", spi->mode);

	return devm_lcdreg_init(dev, reg);
}
EXPORT_SYMBOL_GPL(devm_lcdreg_spi_init);

MODULE_LICENSE("GPL");
