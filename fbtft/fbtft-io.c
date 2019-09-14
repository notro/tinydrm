#include <linux/export.h>
#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/spi/spi.h>
#include "fbtft.h"

int fbtft_write_spi(struct fbtft_par *par, void *buf, size_t len)
{
	struct spi_transfer t = {
		.tx_buf = buf,
		.len = len,
	};
	struct spi_message m;

	fbtft_par_dbg_hex(DEBUG_WRITE, par, par->info->device, u8, buf, len,
		"%s(len=%d): ", __func__, len);

	if (!par->spi) {
		dev_err(par->info->device,
			"%s: par->spi is unexpectedly NULL\n", __func__);
		return -1;
	}

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	return spi_sync(par->spi, &m);
}
EXPORT_SYMBOL(fbtft_write_spi);

/**
 * fbtft_write_spi_emulate_9() - write SPI emulating 9-bit
 * @par: Driver data
 * @buf: Buffer to write
 * @len: Length of buffer (must be divisible by 8)
 *
 * When 9-bit SPI is not available, this function can be used to emulate that.
 * par->extra must hold a transformation buffer used for transfer.
 */
int fbtft_write_spi_emulate_9(struct fbtft_par *par, void *buf, size_t len)
{
	u16 *src = buf;
	u8 *dst = par->extra;
	size_t size = len / 2;
	size_t added = 0;
	int bits, i, j;
	u64 val, dc, tmp;

	fbtft_par_dbg_hex(DEBUG_WRITE, par, par->info->device, u8, buf, len,
		"%s(len=%d): ", __func__, len);

	if (!par->extra) {
		dev_err(par->info->device, "%s: error: par->extra is NULL\n",
			__func__);
		return -EINVAL;
	}
	if ((len % 8) != 0) {
		dev_err(par->info->device,
			"error: len=%zu must be divisible by 8\n", len);
		return -EINVAL;
	}

	for (i = 0; i < size; i += 8) {
		tmp = 0;
		bits = 63;
		for (j = 0; j < 7; j++) {
			dc = (*src & 0x0100) ? 1 : 0;
			val = *src & 0x00FF;
			tmp |= dc << bits;
			bits -= 8;
			tmp |= val << bits--;
			src++;
		}
		tmp |= ((*src & 0x0100) ? 1 : 0);
		*(u64 *)dst = cpu_to_be64(tmp);
		dst += 8;
		*dst++ = (u8)(*src++ & 0x00FF);
		added++;
	}

	return spi_write(par->spi, par->extra, size + added);
}
EXPORT_SYMBOL(fbtft_write_spi_emulate_9);

int fbtft_read_spi(struct fbtft_par *par, void *buf, size_t len)
{
	int ret;
	u8 txbuf[32] = { 0, };
	struct spi_transfer	t = {
			.speed_hz = 2000000,
			.rx_buf		= buf,
			.len		= len,
		};
	struct spi_message	m;

	if (!par->spi) {
		dev_err(par->info->device,
			"%s: par->spi is unexpectedly NULL\n", __func__);
		return -ENODEV;
	}

	if (par->startbyte) {
		if (len > 32) {
			dev_err(par->info->device,
				"len=%zu can't be larger than 32 when using 'startbyte'\n",
				len);
			return -EINVAL;
		}
		txbuf[0] = par->startbyte | 0x3;
		t.tx_buf = txbuf;
		fbtft_par_dbg_hex(DEBUG_READ, par, par->info->device, u8,
			txbuf, len, "%s(len=%d) txbuf => ", __func__, len);
	}

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	ret = spi_sync(par->spi, &m);
	fbtft_par_dbg_hex(DEBUG_READ, par, par->info->device, u8, buf, len,
		"%s(len=%d) buf <= ", __func__, len);

	return ret;
}
EXPORT_SYMBOL(fbtft_read_spi);

static void tinydrm_i80_write_value(struct gpio_descs *db,
				    struct gpio_desc *wr, u32 value,
				    u32 *prev_value)
{
if (prev_value && *prev_value == value) {
	gpiod_set_value_cansleep(wr, 0);
	gpiod_set_value_cansleep(wr, 0);
	gpiod_set_value_cansleep(wr, 1);

} else {
	unsigned long value_bitmap = value;

	gpiod_set_value_cansleep(wr, 0);
	gpiod_set_array_value_cansleep(db->ndescs, db->desc, NULL, &value_bitmap);
	gpiod_set_value_cansleep(wr, 1);


}
	if (prev_value)
		*prev_value = value;
}

static
void tinydrm_i80_write_buf(struct gpio_descs *db, struct gpio_desc *wr,
			   void *buf, size_t len)
{
	unsigned int width = db->ndescs;
	size_t i;

	if (width == 8) {
		u8 *buf8 = buf;

		for (i = 0; i < len; i++)
			tinydrm_i80_write_value(db, wr, *buf8++, NULL);
	} else if (width == 16) {
		u16 *buf16 = buf;

		for (i = 0; i < (len / 2); i++)
			tinydrm_i80_write_value(db, wr, *buf16++, NULL);
	} else {
		WARN_ON_ONCE(1);
	}
}

int fbtft_write_gpio8_wr(struct fbtft_par *par, void *buf, size_t len)
{
	fbtft_par_dbg_hex(DEBUG_WRITE, par, par->info->device, u8, buf, len,
		"%s(len=%d): ", __func__, len);

	tinydrm_i80_write_buf(par->gpio.db, par->gpio.wr, buf, len);

	return 0;
}
EXPORT_SYMBOL(fbtft_write_gpio8_wr);

int fbtft_write_gpio16_wr(struct fbtft_par *par, void *buf, size_t len)
{
#if 0
	u16 data;
	int i;
#ifndef DO_NOT_OPTIMIZE_FBTFT_WRITE_GPIO
	static u16 prev_data;
#endif

	fbtft_par_dbg_hex(DEBUG_WRITE, par, par->info->device, u8, buf, len,
		"%s(len=%d): ", __func__, len);

	while (len) {
		data = *(u16 *)buf;

		/* Start writing by pulling down /WR */
		gpio_set_value(par->gpio.wr, 0);

		/* Set data */
#ifndef DO_NOT_OPTIMIZE_FBTFT_WRITE_GPIO
		if (data == prev_data) {
			gpio_set_value(par->gpio.wr, 0); /* used as delay */
		} else {
			for (i = 0; i < 16; i++) {
				if ((data & 1) != (prev_data & 1))
					gpio_set_value(par->gpio.db[i],
								data & 1);
				data >>= 1;
				prev_data >>= 1;
			}
		}
#else
		for (i = 0; i < 16; i++) {
			gpio_set_value(par->gpio.db[i], data & 1);
			data >>= 1;
		}
#endif

		/* Pullup /WR */
		gpio_set_value(par->gpio.wr, 1);

#ifndef DO_NOT_OPTIMIZE_FBTFT_WRITE_GPIO
		prev_data = *(u16 *)buf;
#endif
		buf += 2;
		len -= 2;
	}
#endif
	return 0;
}
EXPORT_SYMBOL(fbtft_write_gpio16_wr);
