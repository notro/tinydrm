/*
 * Copyright (C) 2015 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __LINUX_LCDREG_H
#define __LINUX_LCDREG_H

#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/spi/spi.h>

/**
 * struct lcdreg_transfer - LCD register transfer
 *
 * @index: register index (address)
 *         Known under the following names:
 *         D/C (command=0, data=1)
 *         RS (register selection: index=0, data=1)
 *         D/I (data/index: index=0, data=1)
 * @buf: data array to transfer
 * @count: number of items in array
 * @width: override default regwidth
 */
struct lcdreg_transfer {
	unsigned index;
	void *buf;
	unsigned count;
	unsigned width;
};

/**
 * struct lcdreg - interface to LCD register
 *
 * @dev: device interface
 * @lock: mutex for register access locking
 * @def_width: default register width
 * @readable: register is readable
 * @little_endian: register has little endian byte order
 * @write: write to register
 * @read: read from register (optional)
 * @reset: reset controller (optional)
 * @quirks: Deviations from the MIPI DBI standard
 */
struct lcdreg {
	struct device *dev;
	struct mutex lock;
	unsigned def_width;
	bool readable;
	bool little_endian;

	int (*write)(struct lcdreg *reg, unsigned regnr,
		     struct lcdreg_transfer *transfer);
	int (*read)(struct lcdreg *reg, unsigned regnr,
		    struct lcdreg_transfer *transfer);
	void (*reset)(struct lcdreg *reg);

	u64 quirks;
/* slowdown command (index=0) */
#define LCDREG_SLOW_INDEX0_WRITE	BIT(0)
/*
 * The MIPI DBI spec states that D/C should be HIGH during register reading.
 * However, not all SPI master drivers support cs_change on last transfer and
 * there are LCD controllers that ignore D/C on read.
 */
#define LCDREG_INDEX0_ON_READ		BIT(1)

#ifdef CONFIG_DEBUG_FS
	struct dentry *debugfs;
	u32 debugfs_read_width;
	u32 debugfs_read_reg;
	char *debugfs_read_result;
#endif
};

struct lcdreg *devm_lcdreg_init(struct device *dev, struct lcdreg *reg);
int lcdreg_write(struct lcdreg *reg, unsigned regnr,
		 struct lcdreg_transfer *transfer);
int lcdreg_write_buf32(struct lcdreg *reg, unsigned regnr, const u32 *data,
		       unsigned count);

#define lcdreg_writereg(lcdreg, regnr, seq...) \
({\
	u32 d[] = { seq };\
	lcdreg_write_buf32(lcdreg, regnr, d, ARRAY_SIZE(d));\
})

int lcdreg_read(struct lcdreg *reg, unsigned regnr,
		struct lcdreg_transfer *transfer);
int lcdreg_readreg_buf32(struct lcdreg *reg, unsigned regnr, u32 *buf,
			 unsigned count);

static inline void lcdreg_reset(struct lcdreg *reg)
{
	if (reg->reset)
		reg->reset(reg);
}

static inline bool lcdreg_is_readable(struct lcdreg *reg)
{
	return reg->readable;
}

static inline unsigned lcdreg_bytes_per_word(unsigned bits_per_word)
{
	if (bits_per_word <= 8)
		return 1;
	else if (bits_per_word <= 16)
		return 2;
	else /* bits_per_word <= 32 */
		return 4;
}

struct lcdreg *devm_lcdreg_i2c_init(struct i2c_client *client);

enum lcdreg_spi_mode {
	LCDREG_SPI_4WIRE, /* 8-bit + D/CX line, MIPI DBI Type C option 3 */
	LCDREG_SPI_3WIRE, /* 9-bit inc. D/CX bit, MIPI DBI Type C option 1 */
	LCDREG_SPI_STARTBYTE,
};

struct lcdreg_spi_config {
	enum lcdreg_spi_mode mode;
	unsigned def_width;
	bool readable;
	u32 id;
	char *dc_name;
	struct gpio_desc *dc;
	struct gpio_desc *reset;
	u8 (*startbyte)(struct lcdreg *reg, struct lcdreg_transfer *tr,
			bool read);
};

struct lcdreg *devm_lcdreg_spi_init(struct spi_device *sdev,
				    const struct lcdreg_spi_config *config);
int devm_lcdreg_spi_of_parse(struct device *dev,
			     struct lcdreg_spi_config *cfg);

static inline struct lcdreg *devm_lcdreg_spi_init_of(struct spi_device *sdev,
						     enum lcdreg_spi_mode mode)
{
	struct lcdreg_spi_config cfg = {
		.mode = mode,
	};
	int ret;

	ret = devm_lcdreg_spi_of_parse(&sdev->dev, &cfg);
	if (ret)
		return ERR_PTR(ret);

	return devm_lcdreg_spi_init(sdev, &cfg);
}

#if defined(DEBUG) || defined(CONFIG_DYNAMIC_DEBUG)
static inline void lcdreg_dbg_transfer_buf(struct lcdreg_transfer *tr)
{
	int groupsize = lcdreg_bytes_per_word(tr->width);
	size_t len = min_t(size_t, 32, tr->count * groupsize);

	print_hex_dump_debug("    buf=", DUMP_PREFIX_NONE, 32, groupsize,
			     tr->buf, len, false);
}
#else
static inline void lcdreg_dbg_transfer_buf(struct lcdreg_transfer *tr) { }
#endif /* DEBUG || CONFIG_DYNAMIC_DEBUG */

#endif /* __LINUX_LCDREG_H */
