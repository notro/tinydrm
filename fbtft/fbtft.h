/*
 * Copyright (C) 2013 Noralf Tronnes
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

#ifndef __LINUX_FBTFT_H
#define __LINUX_FBTFT_H

#include "../include/drm/tinydrm/tinydrm.h"
#include "../include/drm/tinydrm/tinydrm-helpers.h"

#include <linux/fb.h>
#include <linux/spinlock.h>
#include <linux/spi/spi.h>
#include <linux/platform_device.h>

#define FBTFT_ONBOARD_BACKLIGHT 2

#define FBTFT_GPIO_NO_MATCH		0xFFFF
#define FBTFT_GPIO_NAME_SIZE	32
#define FBTFT_MAX_INIT_SEQUENCE      512
#define FBTFT_GAMMA_MAX_VALUES_TOTAL 128

#define FBTFT_OF_INIT_CMD	BIT(24)
#define FBTFT_OF_INIT_DELAY	BIT(25)

/**
 * struct fbtft_gpio - Structure that holds one pinname to gpio mapping
 * @name: pinname (reset, dc, etc.)
 * @gpio: GPIO number
 *
 */
struct fbtft_gpio {
	char name[FBTFT_GPIO_NAME_SIZE];
	unsigned int gpio;
};

struct fbtft_par;

/**
 * struct fbtft_ops - FBTFT operations structure
 * @write: Writes to interface bus
 * @read: Reads from interface bus
 * @write_vmem: Writes video memory to display
 * @write_reg: Writes to controller register
 * @set_addr_win: Set the GRAM update window
 * @reset: Reset the LCD controller
 * @init_display: Initializes the display
 * @blank: Blank the display (optional)
 * @verify_gpios: Verify that necessary gpios is present (optional)
 * @register_backlight: Used to register backlight device (optional)
 * @unregister_backlight: Unregister backlight device (optional)
 * @set_var: Configure LCD with values from variables like @rotate and @bgr
 *           (optional)
 * @set_gamma: Set Gamma curve (optional)
 *
 * Most of these operations have default functions assigned to them in
 *     fbtft_framebuffer_alloc()
 */
struct fbtft_ops {
	int (*write)(struct fbtft_par *par, void *buf, size_t len);
	int (*read)(struct fbtft_par *par, void *buf, size_t len);
	int (*write_vmem)(struct fbtft_par *par, size_t offset, size_t len);
	void (*write_register)(struct fbtft_par *par, int len, ...);

	void (*set_addr_win)(struct fbtft_par *par,
		int xs, int ys, int xe, int ye);
	void (*reset)(struct fbtft_par *par);
	int (*init_display)(struct fbtft_par *par);
	int (*blank)(struct fbtft_par *par, bool on);

	/* Dummy, kept for fb_watterott */
	int (*verify_gpios)(struct fbtft_par *par);

	void (*register_backlight)(struct fbtft_par *par);
	void (*unregister_backlight)(struct fbtft_par *par);

	int (*set_var)(struct fbtft_par *par);
	int (*set_gamma)(struct fbtft_par *par, unsigned long *curves);
};

struct fbtft_display {
	unsigned int width;
	unsigned int height;
	unsigned int regwidth;
	unsigned int buswidth;
	unsigned int backlight;
	struct fbtft_ops fbtftops;
	unsigned int bpp;
	unsigned int fps;
	int txbuflen;
	s16 *init_sequence;
	char *gamma;
	int gamma_num;
	int gamma_len;
};

/* Needed by fb_uc1611 and fb_ssd1351 */
struct fbtft_platform_data {
	struct fbtft_display display;
};

struct fbtft_fb_var_screeninfo {
	u32 xres;
	u32 yres;
	u32 rotate;
};

struct fbtft_fb_fix_screeninfo {
	u32 line_length;
};

struct fbtft_fb_info {
	struct device *device;
	struct fbtft_par *par;
	void *screen_buffer;
	struct backlight_device *bl_dev;
	struct fbtft_fb_var_screeninfo var;
	struct fbtft_fb_fix_screeninfo fix;
};

struct fbtft_par {
	struct tinydrm_device tinydrm;
	struct spi_device *spi;
	struct platform_device *pdev;
	struct fbtft_display display;
	struct fbtft_fb_info *info;
	struct fbtft_platform_data *pdata;
	u16 *ssbuf;
	u32 pseudo_palette[16];
	struct {
		void *buf;
		unsigned int len;
	} txbuf;
	u8 *buf;
	u8 startbyte;
	struct fbtft_ops fbtftops;
	spinlock_t dirty_lock;
	unsigned int dirty_lines_start;
	unsigned int dirty_lines_end;
	struct {
		int reset;
		int dc;
		int rd;
		int wr;
		int cs;
		int db[16];
		int led[16];
	} gpio;
	s16 *init_sequence;
	struct {
		struct mutex lock;
		unsigned long *curves;
		int num_values;
		int num_curves;
	} gamma;

	/* Used in fb_ra8875, fb_ssd1331 */
	unsigned long debug;

	bool bgr;
	void *extra;
};

#define NUMARGS(...)  (sizeof((int[]){__VA_ARGS__})/sizeof(int))

#define write_reg(par, ...)                                              \
	par->fbtftops.write_register(par, NUMARGS(__VA_ARGS__), __VA_ARGS__)

/* fbtft-core.c */
void fbtft_dbg_hex(const struct device *dev, int groupsize,
		   void *buf, size_t len, const char *fmt, ...);
int fbtft_probe_common(struct fbtft_display *display, struct spi_device *sdev,
		       struct platform_device *pdev);
int fbtft_remove_common(struct device *dev, struct fbtft_par *par);

#ifdef CONFIG_BACKLIGHT_CLASS_DEVICE
void fbtft_register_backlight(struct fbtft_par *par);
void fbtft_unregister_backlight(struct fbtft_par *par);
#else
static inline void fbtft_register_backlight(struct fbtft_par *par)
{
}
static inline void fbtft_unregister_backlight(struct fbtft_par *par)
{
}
#endif

/* fbtft-io.c */
int fbtft_write_spi(struct fbtft_par *par, void *buf, size_t len);
int fbtft_write_spi_emulate_9(struct fbtft_par *par, void *buf, size_t len);
int fbtft_read_spi(struct fbtft_par *par, void *buf, size_t len);
int fbtft_write_gpio8_wr(struct fbtft_par *par, void *buf, size_t len);
int fbtft_write_gpio16_wr(struct fbtft_par *par, void *buf, size_t len);
int fbtft_write_gpio16_wr_latched(struct fbtft_par *par, void *buf, size_t len);

/* fbtft-bus.c */
int fbtft_write_vmem16_bus16(struct fbtft_par *par, size_t offset, size_t len);
int fbtft_write_vmem16_bus8(struct fbtft_par *par, size_t offset, size_t len);
int fbtft_write_vmem16_bus9(struct fbtft_par *par, size_t offset, size_t len);
void fbtft_write_reg8_bus8(struct fbtft_par *par, int len, ...);
void fbtft_write_reg8_bus9(struct fbtft_par *par, int len, ...);
void fbtft_write_reg16_bus8(struct fbtft_par *par, int len, ...);
void fbtft_write_reg16_bus16(struct fbtft_par *par, int len, ...);

#define FBTFT_REGISTER_DRIVER(_name, _compatible, _display)                \
									   \
static int fbtft_driver_probe_spi(struct spi_device *spi)                  \
{                                                                          \
	return fbtft_probe_common(_display, spi, NULL);                    \
}                                                                          \
									   \
static int fbtft_driver_remove_spi(struct spi_device *spi)                 \
{                                                                          \
	struct fbtft_par *par = spi_get_drvdata(spi);                      \
									   \
	return fbtft_remove_common(&spi->dev, par);                        \
}                                                                          \
									   \
static int fbtft_driver_probe_pdev(struct platform_device *pdev)           \
{                                                                          \
	return fbtft_probe_common(_display, NULL, pdev);                   \
}                                                                          \
									   \
static int fbtft_driver_remove_pdev(struct platform_device *pdev)          \
{                                                                          \
	struct fbtft_par *par = platform_get_drvdata(pdev);                \
									   \
	return fbtft_remove_common(&pdev->dev, par);                       \
}                                                                          \
									   \
static const struct of_device_id dt_ids[] = {                              \
	{ .compatible = _compatible },                                     \
	{},                                                                \
};                                                                         \
									   \
MODULE_DEVICE_TABLE(of, dt_ids);                                           \
									   \
									   \
static struct spi_driver fbtft_driver_spi_driver = {                       \
	.driver = {                                                        \
		.name   = _name,                                           \
		.of_match_table = of_match_ptr(dt_ids),                    \
	},                                                                 \
	.probe  = fbtft_driver_probe_spi,                                  \
	.remove = fbtft_driver_remove_spi,                                 \
};                                                                         \
									   \
static struct platform_driver fbtft_driver_platform_driver = {             \
	.driver = {                                                        \
		.name   = _name,                                           \
		.owner  = THIS_MODULE,                                     \
		.of_match_table = of_match_ptr(dt_ids),                    \
	},                                                                 \
	.probe  = fbtft_driver_probe_pdev,                                 \
	.remove = fbtft_driver_remove_pdev,                                \
};                                                                         \
									   \
static int __init fbtft_driver_module_init(void)                           \
{                                                                          \
	int ret;                                                           \
									   \
	ret = spi_register_driver(&fbtft_driver_spi_driver);               \
	if (ret < 0)                                                       \
		return ret;                                                \
	return platform_driver_register(&fbtft_driver_platform_driver);    \
}                                                                          \
									   \
static void __exit fbtft_driver_module_exit(void)                          \
{                                                                          \
	spi_unregister_driver(&fbtft_driver_spi_driver);                   \
	platform_driver_unregister(&fbtft_driver_platform_driver);         \
}                                                                          \
									   \
module_init(fbtft_driver_module_init);                                     \
module_exit(fbtft_driver_module_exit);

/* Debug macros */

#define DEBUG_DRIVER_INIT_FUNCTIONS (1<<3)

/* fbtftops */
#define DEBUG_BACKLIGHT             (1<<17)
#define DEBUG_READ                  (1<<18)
#define DEBUG_WRITE                 (1<<19)
#define DEBUG_WRITE_VMEM            (1<<20)
#define DEBUG_WRITE_REGISTER        (1<<21)
#define DEBUG_SET_ADDR_WIN          (1<<22)
#define DEBUG_RESET                 (1<<23)

#define DEBUG_INIT_DISPLAY          (1<<26)
#define DEBUG_BLANK                 (1<<27)
#define DEBUG_REQUEST_GPIOS         (1<<28)

#define DEBUG_VERIFY_GPIOS          (1<<31)

/* used in flexfb */
#define fbtft_init_dbg(dev, format, arg...)

#define fbtft_par_dbg(level, par, format, arg...) \
	DRM_DEV_DEBUG_DRIVER(par->info->device, format, ##arg)

#define fbtft_par_dbg_hex(level, par, dev, type, buf, num, format, arg...) \
do {                                                                       \
	if (drm_debug & DRM_UT_DRIVER)                                     \
		fbtft_dbg_hex(dev, sizeof(type), buf, num * sizeof(type), format, ##arg); \
} while (0)

#endif /* __LINUX_FBTFT_H */
