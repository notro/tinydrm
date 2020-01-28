// SPDX-License-Identifier: GPL-2.0
// fb_hx8340bn: Copyright (C) 2013 Noralf Trønnes
// fb_hx8353d": Copyright (c) 2014 Petr Olivka
// fb_hx8357d": Copyright (C) 2015 Adafruit Industries
// fb_ili9340": Copyright (C) 2013 Noralf Trønnes
// fb_ili9341": Copyright (C) 2013 Christian Vogelgsang
// fb_ili9481": Copyright (c) 2014 Petr Olivka
// fb_ili9486": Copyright (C) 2014 Noralf Trønnes
// fb_s6d02a1": Copyright (C) 2014 Wolfgang Buening
// fb_st7735r": Copyright (C) 2013 Noralf Trønnes
// fb_st7789v": Copyright (C) 2015 Dennis Menschel
// fb_tinylcd": Copyright (C) 2013 Noralf Trønnes

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/property.h>
#include <linux/spi/spi.h>
#include <video/mipi_display.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_mipi_dbi.h>
#include <drm/drm_modeset_helper.h>

#include "tinydrm-fbtft.h"

enum fb_mipi_dbi_variant {
	MIPI_DBI_FB_HX8340BN,
	MIPI_DBI_FB_HX8353D,
	MIPI_DBI_FB_HX8357D,
//	MIPI_DBI_FB_ILI9163, // This one has a custom set_addr_win()
	MIPI_DBI_FB_ILI9340,
	MIPI_DBI_FB_ILI9341,
	MIPI_DBI_FB_ILI9481,
	MIPI_DBI_FB_ILI9486,
	MIPI_DBI_FB_S6D02A1,
	MIPI_DBI_FB_ST7735R,
	MIPI_DBI_FB_ST7789V,
	MIPI_DBI_FB_TINYLCD,
};

struct fb_mipi_dbi {
	struct mipi_dbi_dev dbidev;
	enum fb_mipi_dbi_variant variant;
};

#define MADCTL_MY	BIT(7) /* MY row address order */
#define MADCTL_MX	BIT(6) /* MX column address order */
#define MADCTL_MV	BIT(5) /* MV row / column exchange */
#define MADCTL_ML	BIT(4) /* ML vertical refresh order */
#define MADCTL_BGR	BIT(3)
#define MADCTL_MH	BIT(2) /* MH horizontal refresh order */

#define ILI9481_HFLIP	BIT(0)
#define ILI9481_VFLIP	BIT(1)

static void fb_mipi_dbi_rotate(struct mipi_dbi_dev *dbidev, u8 rotate0, u8 rotate90, u8 rotate180, u8 rotate270)
{
	bool bgr = device_property_present(dbidev->drm.dev, "bgr");
	u8 addr_mode;

	switch (dbidev->rotation) {
	default:
		addr_mode = rotate0;
		break;
	case 90:
		addr_mode = rotate90;
		break;
	case 180:
		addr_mode = rotate180;
		break;
	case 270:
		addr_mode = rotate270;
		break;
	}
	if (bgr)
		addr_mode |= MADCTL_BGR;
	mipi_dbi_command(&dbidev->dbi, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);
}

static void fb_hx8340bn_enable(struct mipi_dbi *dbi)
{
	DRM_DEBUG_KMS("\n");

	/* BTL221722-276L startup sequence, from datasheet */

	/*
	 * SETEXTCOM: Set extended command set (C1h)
	 * This command is used to set extended command set access enable.
	 * Enable: After command (C1h), must write: ffh,83h,40h
	 */
	mipi_dbi_command(dbi, 0xC1, 0xFF, 0x83, 0x40);

	/*
	 * Sleep out
	 * This command turns off sleep mode.
	 * In this mode the DC/DC converter is enabled, Internal oscillator
	 * is started, and panel scanning is started.
	 */
	mipi_dbi_command(dbi, 0x11);
	msleep(150);

	/* Undoc'd register? */
	mipi_dbi_command(dbi, 0xCA, 0x70, 0x00, 0xD9);

	/*
	 * SETOSC: Set Internal Oscillator (B0h)
	 * This command is used to set internal oscillator related settings
	 *	OSC_EN: Enable internal oscillator
	 *	Internal oscillator frequency: 125% x 2.52MHz
	 */
	mipi_dbi_command(dbi, 0xB0, 0x01, 0x11);

	/* Drive ability setting */
	mipi_dbi_command(dbi, 0xC9, 0x90, 0x49, 0x10, 0x28, 0x28, 0x10, 0x00, 0x06);
	msleep(20);

	/*
	 * SETPWCTR5: Set Power Control 5(B5h)
	 * This command is used to set VCOM Low and VCOM High Voltage
	 * VCOMH 0110101 :  3.925
	 * VCOML 0100000 : -1.700
	 * 45h=69  VCOMH: "VMH" + 5d   VCOML: "VMH" + 5d
	 */
	mipi_dbi_command(dbi, 0xB5, 0x35, 0x20, 0x45);

	/*
	 * SETPWCTR4: Set Power Control 4(B4h)
	 *	VRH[4:0]:	Specify the VREG1 voltage adjusting.
	 *			VREG1 voltage is for gamma voltage setting.
	 *	BT[2:0]:	Switch the output factor of step-up circuit 2
	 *			for VGH and VGL voltage generation.
	 */
	mipi_dbi_command(dbi, 0xB4, 0x33, 0x25, 0x4C);
	msleep(10);

	/*
	 * Interface Pixel Format (3Ah)
	 * This command is used to define the format of RGB picture data,
	 * which is to be transfer via the system and RGB interface.
	 * RGB interface: 16 Bit/Pixel
	 */
	mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT, MIPI_DCS_PIXEL_FMT_16BIT);

	/*
	 * Display on (29h)
	 * This command is used to recover from DISPLAY OFF mode.
	 * Output from the Frame Memory is enabled.
	 */
	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
	msleep(10);

	mipi_dbi_command(dbi, MIPI_DCS_SET_GAMMA_CURVE, 0x01);
	mipi_dbi_command(dbi, 0xc2, 0x60, 0x71, 0x01, 0x0e, 0x05, 0x02, 0x09, 0x31, 0x0a);
	mipi_dbi_command(dbi, 0xc3, 0x67, 0x30, 0x61, 0x17, 0x48, 0x07, 0x05, 0x33);
}

static void fb_hx8353d_enable(struct mipi_dbi *dbi)
{
	DRM_DEBUG_KMS("\n");

	/* SETEXTC */
	mipi_dbi_command(dbi, 0xB9, 0xFF, 0x83, 0x53);

	/* RADJ */
	mipi_dbi_command(dbi, 0xB0, 0x3C, 0x01);

	/* VCOM */
	mipi_dbi_command(dbi, 0xB6, 0x94, 0x6C, 0x50);

	/* PWR */
	mipi_dbi_command(dbi, 0xB1, 0x00, 0x01, 0x1B, 0x03, 0x01, 0x08, 0x77, 0x89);

	/* COLMOD */
	mipi_dbi_command(dbi, 0x3A, 0x05);

	/* MEM ACCESS */
	mipi_dbi_command(dbi, MIPI_DCS_SET_ADDRESS_MODE, 0xC0);

	/* SLPOUT - Sleep out & booster on */
	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(150);

	/* DISPON - Display On */
	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);

	/* RGBSET */
	mipi_dbi_command(dbi, MIPI_DCS_WRITE_LUT,
		  0,  2,  4,  6,  8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30,
		32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62,
		 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
		16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
		32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
		48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
		 0,  2,  4,  6,  8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30,
		32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62);

	mipi_dbi_command(dbi, 0xE0, 0x50, 0x77, 0x40, 0x08, 0xBF, 0x00, 0x03, 0x0F, 0x00, 0x01,
			     0x73, 0x00, 0x72, 0x03, 0xB0, 0x0F, 0x08, 0x00, 0x0F);
}

#define HX8357_SETOSC 0xB0
#define HX8357_SETPWR1 0xB1
#define HX8357_SETRGB 0xB3
#define HX8357D_SETCOM  0xB6
#define HX8357D_SETCYC  0xB4
#define HX8357D_SETC 0xB9
#define HX8357D_SETSTBA 0xC0
#define HX8357_SETPANEL  0xCC
#define HX8357D_SETGAMMA 0xE0

static void fb_hx8357d_enable(struct mipi_dbi *dbi)
{
	DRM_DEBUG_KMS("\n");

	/* setextc */
	mipi_dbi_command(dbi, HX8357D_SETC, 0xFF, 0x83, 0x57);
	msleep(150);

	/* setRGB which also enables SDO */
	mipi_dbi_command(dbi, HX8357_SETRGB, 0x00, 0x00, 0x06, 0x06);

	/* -1.52V */
	mipi_dbi_command(dbi, HX8357D_SETCOM, 0x25);

	/* Normal mode 70Hz, Idle mode 55 Hz */
	mipi_dbi_command(dbi, HX8357_SETOSC, 0x68);

	/* Set Panel - BGR, Gate direction swapped */
	mipi_dbi_command(dbi, HX8357_SETPANEL, 0x05);

	mipi_dbi_command(dbi, HX8357_SETPWR1,
		  0x00,  /* Not deep standby */
		  0x15,  /* BT */
		  0x1C,  /* VSPR */
		  0x1C,  /* VSNR */
		  0x83,  /* AP */
		  0xAA);  /* FS */

	mipi_dbi_command(dbi, HX8357D_SETSTBA,
		  0x50,  /* OPON normal */
		  0x50,  /* OPON idle */
		  0x01,  /* STBA */
		  0x3C,  /* STBA */
		  0x1E,  /* STBA */
		  0x08);  /* GEN */

	mipi_dbi_command(dbi, HX8357D_SETCYC,
		  0x02,  /* NW 0x02 */
		  0x40,  /* RTN */
		  0x00,  /* DIV */
		  0x2A,  /* DUM */
		  0x2A,  /* DUM */
		  0x0D,  /* GDON */
		  0x78);  /* GDOFF */

	mipi_dbi_command(dbi, HX8357D_SETGAMMA,
		  0x02, 0x0A, 0x11, 0x1d, 0x23, 0x35, 0x41, 0x4b, 0x4b, 0x42, 0x3A, 0x27, 0x1B, 0x08, 0x09, 0x03, 0x02,
		  0x0A, 0x11, 0x1d, 0x23, 0x35, 0x41, 0x4b, 0x4b, 0x42, 0x3A, 0x27, 0x1B, 0x08, 0x09, 0x03, 0x00, 0x01);

	/* 16 bit */
	mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT, 0x55);

	mipi_dbi_command(dbi, MIPI_DCS_SET_ADDRESS_MODE, 0xC0);

	/* TE off */
	mipi_dbi_command(dbi, MIPI_DCS_SET_TEAR_ON, 0x00);

	/* tear line */
	mipi_dbi_command(dbi, MIPI_DCS_SET_TEAR_SCANLINE, 0x00, 0x02);

	/* Exit Sleep */
	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(150);

	/* display on */
	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
	usleep_range(5000, 7000);
}

static void fb_ili9340_enable(struct mipi_dbi *dbi)
{
	DRM_DEBUG_KMS("\n");

	mipi_dbi_command(dbi, 0xEF, 0x03, 0x80, 0x02);
	mipi_dbi_command(dbi, 0xCF, 0x00, 0XC1, 0X30);
	mipi_dbi_command(dbi, 0xED, 0x64, 0x03, 0X12, 0X81);
	mipi_dbi_command(dbi, 0xE8, 0x85, 0x00, 0x78);
	mipi_dbi_command(dbi, 0xCB, 0x39, 0x2C, 0x00, 0x34, 0x02);
	mipi_dbi_command(dbi, 0xF7, 0x20);
	mipi_dbi_command(dbi, 0xEA, 0x00, 0x00);

	/* Power Control 1 */
	mipi_dbi_command(dbi, 0xC0, 0x23);

	/* Power Control 2 */
	mipi_dbi_command(dbi, 0xC1, 0x10);

	/* VCOM Control 1 */
	mipi_dbi_command(dbi, 0xC5, 0x3e, 0x28);

	/* VCOM Control 2 */
	mipi_dbi_command(dbi, 0xC7, 0x86);

	/* COLMOD: Pixel Format Set */
	/* 16 bits/pixel */
	mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT, 0x55);

	/* Frame Rate Control */
	/* Division ratio = fosc, Frame Rate = 79Hz */
	mipi_dbi_command(dbi, 0xB1, 0x00, 0x18);

	/* Display Function Control */
	mipi_dbi_command(dbi, 0xB6, 0x08, 0x82, 0x27);

	/* Gamma Function Disable */
	mipi_dbi_command(dbi, 0xF2, 0x00);

	/* Gamma curve selection */
	mipi_dbi_command(dbi, MIPI_DCS_SET_GAMMA_CURVE, 0x01);

	/* Positive Gamma Correction */
	mipi_dbi_command(dbi, 0xE0,
		  0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
		  0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00);

	/* Negative Gamma Correction */
	mipi_dbi_command(dbi, 0xE1,
		  0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
		  0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F);

	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);

	msleep(120);

	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
}

static void fb_ili9341_enable(struct mipi_dbi *dbi)
{
	DRM_DEBUG_KMS("\n");

	/* startup sequence for MI0283QT-9A */
	mipi_dbi_command(dbi, MIPI_DCS_SOFT_RESET);
	msleep(5);
	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_OFF);
	/* --------------------------------------------------------- */
	mipi_dbi_command(dbi, 0xCF, 0x00, 0x83, 0x30);
	mipi_dbi_command(dbi, 0xED, 0x64, 0x03, 0x12, 0x81);
	mipi_dbi_command(dbi, 0xE8, 0x85, 0x01, 0x79);
	mipi_dbi_command(dbi, 0xCB, 0x39, 0X2C, 0x00, 0x34, 0x02);
	mipi_dbi_command(dbi, 0xF7, 0x20);
	mipi_dbi_command(dbi, 0xEA, 0x00, 0x00);
	/* ------------power control-------------------------------- */
	mipi_dbi_command(dbi, 0xC0, 0x26);
	mipi_dbi_command(dbi, 0xC1, 0x11);
	/* ------------VCOM --------- */
	mipi_dbi_command(dbi, 0xC5, 0x35, 0x3E);
	mipi_dbi_command(dbi, 0xC7, 0xBE);
	/* ------------memory access control------------------------ */
	mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT, 0x55); /* 16bit pixel */
	/* ------------frame rate----------------------------------- */
	mipi_dbi_command(dbi, 0xB1, 0x00, 0x1B);
	/* ------------Gamma---------------------------------------- */
	/* mipi_dbi_command(dbi, 0xF2, 0x08); */ /* Gamma Function Disable */
	mipi_dbi_command(dbi, MIPI_DCS_SET_GAMMA_CURVE, 0x01);
	/* ------------display-------------------------------------- */
	mipi_dbi_command(dbi, 0xB7, 0x07); /* entry mode set */
	mipi_dbi_command(dbi, 0xB6, 0x0A, 0x82, 0x27, 0x00);
	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(100);
	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
	msleep(20);

	mipi_dbi_command(dbi, 0xe0, 0x1f, 0x1a, 0x18, 0x0a, 0x0f, 0x06, 0x45, 0x87, 0x32, 0x0a, 0x07, 0x02, 0x07, 0x05, 0x00);
	mipi_dbi_command(dbi, 0xe1, 0x00, 0x25, 0x27, 0x05, 0x10, 0x09, 0x3a, 0x78, 0x4d, 0x05, 0x18, 0x0d, 0x38, 0x3a, 0x1f);
}

static void fb_ili9481_enable(struct mipi_dbi *dbi)
{
	DRM_DEBUG_KMS("\n");

	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(50);
	/* Power setting */
	mipi_dbi_command(dbi, 0xD0, 0x07, 0x42, 0x18);
	/* VCOM */
	mipi_dbi_command(dbi, 0xD1, 0x00, 0x07, 0x10);
	/* Power setting for norm. mode */
	mipi_dbi_command(dbi, 0xD2, 0x01, 0x02);
	/* Panel driving setting */
	mipi_dbi_command(dbi, 0xC0, 0x10, 0x3B, 0x00, 0x02, 0x11);
	/* Frame rate & inv. */
	mipi_dbi_command(dbi, 0xC5, 0x03);
	/* Pixel format */
	mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT, 0x55);
	/* Gamma */
	mipi_dbi_command(dbi, 0xC8, 0x00, 0x32, 0x36, 0x45, 0x06, 0x16,
		       0x37, 0x75, 0x77, 0x54, 0x0C, 0x00);
	/* DISP_ON */
	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
}

static void fb_ili9486_enable(struct mipi_dbi *dbi)
{
	DRM_DEBUG_KMS("\n");

	/* Interface Mode Control */
	mipi_dbi_command(dbi, 0xb0, 0x0);
	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(250);
	/* Interface Pixel Format */
	mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT, 0x55);
	/* Power Control 3 */
	mipi_dbi_command(dbi, 0xC2, 0x44);
	/* VCOM Control 1 */
	mipi_dbi_command(dbi, 0xC5, 0x00, 0x00, 0x00, 0x00);
	/* PGAMCTRL(Positive Gamma Control) */
	mipi_dbi_command(dbi, 0xE0, 0x0F, 0x1F, 0x1C, 0x0C, 0x0F, 0x08, 0x48, 0x98,
			     0x37, 0x0A, 0x13, 0x04, 0x11, 0x0D, 0x00);
	/* NGAMCTRL(Negative Gamma Control) */
	mipi_dbi_command(dbi, 0xE1, 0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75,
			     0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00);
	/* Digital Gamma Control 1 */
	mipi_dbi_command(dbi, 0xE2, 0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75,
			     0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00);
	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
}

static void fb_s6d02a1_enable(struct mipi_dbi *dbi)
{
	DRM_DEBUG_KMS("\n");

	mipi_dbi_command(dbi, 0xf0, 0x5a, 0x5a);

	mipi_dbi_command(dbi, 0xfc, 0x5a, 0x5a);

	mipi_dbi_command(dbi, 0xfa, 0x02, 0x1f, 0x00, 0x10, 0x22, 0x30, 0x38, 0x3A, 0x3A, 0x3A, 0x3A, 0x3A, 0x3d, 0x02, 0x01);

	mipi_dbi_command(dbi, 0xfb, 0x21, 0x00, 0x02, 0x04, 0x07, 0x0a, 0x0b, 0x0c, 0x0c, 0x16, 0x1e, 0x30, 0x3f, 0x01, 0x02);

	/* power setting sequence */
	mipi_dbi_command(dbi, 0xfd, 0x00, 0x00, 0x00, 0x17, 0x10, 0x00, 0x01, 0x01, 0x00, 0x1f, 0x1f);

	mipi_dbi_command(dbi, 0xf4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x3f, 0x07, 0x00, 0x3C, 0x36, 0x00, 0x3C, 0x36, 0x00);

	mipi_dbi_command(dbi, 0xf5, 0x00, 0x70, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6d, 0x66, 0x06);

	mipi_dbi_command(dbi, 0xf6, 0x02, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x02, 0x00, 0x06, 0x01, 0x00);

	mipi_dbi_command(dbi, 0xf2, 0x00, 0x01, 0x03, 0x08, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x04, 0x08, 0x08);

	mipi_dbi_command(dbi, 0xf8, 0x11);

	mipi_dbi_command(dbi, 0xf7, 0xc8, 0x20, 0x00, 0x00);

	mipi_dbi_command(dbi, 0xf3, 0x00, 0x00);

	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(50);

	mipi_dbi_command(dbi, 0xf3, 0x00, 0x01);
	msleep(50);
	mipi_dbi_command(dbi, 0xf3, 0x00, 0x03);
	msleep(50);
	mipi_dbi_command(dbi, 0xf3, 0x00, 0x07);
	msleep(50);
	mipi_dbi_command(dbi, 0xf3, 0x00, 0x0f);
	msleep(50);

	mipi_dbi_command(dbi, 0xf4, 0x00, 0x04, 0x00, 0x00, 0x00, 0x3f, 0x3f, 0x07, 0x00, 0x3C, 0x36, 0x00, 0x3C, 0x36, 0x00);
	msleep(50);

	mipi_dbi_command(dbi, 0xf3, 0x00, 0x1f);
	msleep(50);
	mipi_dbi_command(dbi, 0xf3, 0x00, 0x7f);
	msleep(50);

	mipi_dbi_command(dbi, 0xf3, 0x00, 0xff);
	msleep(50);

	mipi_dbi_command(dbi, 0xfd, 0x00, 0x00, 0x00, 0x17, 0x10, 0x00, 0x00, 0x01, 0x00, 0x16, 0x16);

	mipi_dbi_command(dbi, 0xf4, 0x00, 0x09, 0x00, 0x00, 0x00, 0x3f, 0x3f, 0x07, 0x00, 0x3C, 0x36, 0x00, 0x3C, 0x36, 0x00);

	/* initializing sequence */

	mipi_dbi_command(dbi, MIPI_DCS_SET_ADDRESS_MODE, 0x08);

	mipi_dbi_command(dbi, MIPI_DCS_SET_TEAR_ON, 0x00);

	mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT, 0x05);

	/* gamma setting - possible values 0x01, 0x02, 0x04, 0x08 */
	mipi_dbi_command(dbi, MIPI_DCS_SET_GAMMA_CURVE, 0x01);

	msleep(150);
	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
	mipi_dbi_command(dbi, MIPI_DCS_WRITE_MEMORY_START);
}

static void fb_st7735r_enable(struct mipi_dbi *dbi)
{
	DRM_DEBUG_KMS("\n");

	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(500);

	/* FRMCTR1 - frame rate control: normal mode
	 * frame rate = fosc / (1 x 2 + 40) * (LINE + 2C + 2D)
	 */
	mipi_dbi_command(dbi, 0xB1, 0x01, 0x2C, 0x2D);

	/* FRMCTR2 - frame rate control: idle mode
	 * frame rate = fosc / (1 x 2 + 40) * (LINE + 2C + 2D)
	 */
	mipi_dbi_command(dbi, 0xB2, 0x01, 0x2C, 0x2D);

	/* FRMCTR3 - frame rate control - partial mode
	 * dot inversion mode, line inversion mode
	 */
	mipi_dbi_command(dbi, 0xB3, 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D);

	/* INVCTR - display inversion control
	 * no inversion
	 */
	mipi_dbi_command(dbi, 0xB4, 0x07);

	/* PWCTR1 - Power Control
	 * -4.6V, AUTO mode
	 */
	mipi_dbi_command(dbi, 0xC0, 0xA2, 0x02, 0x84);

	/* PWCTR2 - Power Control
	 * VGH25 = 2.4C VGSEL = -10 VGH = 3 * AVDD
	 */
	mipi_dbi_command(dbi, 0xC1, 0xC5);

	/* PWCTR3 - Power Control
	 * Opamp current small, Boost frequency
	 */
	mipi_dbi_command(dbi, 0xC2, 0x0A, 0x00);

	/* PWCTR4 - Power Control
	 * BCLK/2, Opamp current small & Medium low
	 */
	mipi_dbi_command(dbi, 0xC3, 0x8A, 0x2A);

	/* PWCTR5 - Power Control */
	mipi_dbi_command(dbi, 0xC4, 0x8A, 0xEE);

	/* VMCTR1 - Power Control */
	mipi_dbi_command(dbi, 0xC5, 0x0E);

	mipi_dbi_command(dbi, MIPI_DCS_EXIT_INVERT_MODE);

	mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT, MIPI_DCS_PIXEL_FMT_16BIT);

	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
	msleep(100);

	mipi_dbi_command(dbi, MIPI_DCS_ENTER_NORMAL_MODE);
	msleep(10);

	mipi_dbi_command(dbi, 0xe0, 0x02, 0x1c, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2d, 0x29, 0x25, 0x2b, 0x39, 0x00, 0x01, 0x03, 0x10);
	mipi_dbi_command(dbi, 0xe1, 0x03, 0x1d, 0x07, 0x06, 0x2e, 0x2c, 0x29, 0x2d, 0x2e, 0x2e, 0x37, 0x3f, 0x00, 0x00, 0x02, 0x10);
}

#define ST7789V_PORCTRL		0xB2
#define ST7789V_GCTRL		0xB7
#define ST7789V_VCOMS		0xBB
#define ST7789V_VDVVRHEN	0xC2
#define ST7789V_VRHS		0xC3
#define ST7789V_VDVS		0xC4
#define ST7789V_VCMOFSET	0xC5
#define ST7789V_PWCTRL1		0xD0
#define ST7789V_PVGAMCTRL	0xE0
#define ST7789V_NVGAMCTRL	0xE1

static void fb_st7789v_enable(struct mipi_dbi *dbi)
{
	DRM_DEBUG_KMS("\n");

	/* turn off sleep mode */
	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(120);

	/* set pixel format to RGB-565 */
	mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT, MIPI_DCS_PIXEL_FMT_16BIT);

	mipi_dbi_command(dbi, ST7789V_PORCTRL, 0x08, 0x08, 0x00, 0x22, 0x22);

	/*
	 * VGH = 13.26V
	 * VGL = -10.43V
	 */
	mipi_dbi_command(dbi, ST7789V_GCTRL, 0x35);

	/*
	 * VDV and VRH register values come from command write
	 * (instead of NVM)
	 */
	mipi_dbi_command(dbi, ST7789V_VDVVRHEN, 0x01, 0xFF);

	/*
	 * VAP =  4.1V + (VCOM + VCOM offset + 0.5 * VDV)
	 * VAN = -4.1V + (VCOM + VCOM offset + 0.5 * VDV)
	 */
	mipi_dbi_command(dbi, ST7789V_VRHS, 0x0B);

	/* VDV = 0V */
	mipi_dbi_command(dbi, ST7789V_VDVS, 0x20);

	/* VCOM = 0.9V */
	mipi_dbi_command(dbi, ST7789V_VCOMS, 0x20);

	/* VCOM offset = 0V */
	mipi_dbi_command(dbi, ST7789V_VCMOFSET, 0x20);

	/*
	 * AVDD = 6.8V
	 * AVCL = -4.8V
	 * VDS = 2.3V
	 */
	mipi_dbi_command(dbi, ST7789V_PWCTRL1, 0xA4, 0xA1);

	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);

	mipi_dbi_command(dbi, 0xe0, 0xd0, 0x00, 0x14, 0x15, 0x13, 0x2c, 0x42, 0x43, 0x4e, 0x09, 0x16, 0x14, 0x18, 0x21);
	mipi_dbi_command(dbi, 0xe1, 0xd0, 0x00, 0x14, 0x15, 0x13, 0x0b, 0x43, 0x55, 0x53, 0x0c, 0x17, 0x14, 0x23, 0x20);
}

static void fb_tinylcd_enable(struct mipi_dbi *dbi)
{
	DRM_DEBUG_KMS("\n");

	mipi_dbi_command(dbi, 0xB0, 0x80);
	mipi_dbi_command(dbi, 0xC0, 0x0A, 0x0A);
	mipi_dbi_command(dbi, 0xC1, 0x45, 0x07);
	mipi_dbi_command(dbi, 0xC2, 0x33);
	mipi_dbi_command(dbi, 0xC5, 0x00, 0x42, 0x80);
	mipi_dbi_command(dbi, 0xB1, 0xD0, 0x11);
	mipi_dbi_command(dbi, 0xB4, 0x02);
	mipi_dbi_command(dbi, 0xB6, 0x00, 0x22, 0x3B);
	mipi_dbi_command(dbi, 0xB7, 0x07);
	mipi_dbi_command(dbi, MIPI_DCS_SET_ADDRESS_MODE, 0x58);
	mipi_dbi_command(dbi, 0xF0, 0x36, 0xA5, 0xD3);
	mipi_dbi_command(dbi, 0xE5, 0x80);
	mipi_dbi_command(dbi, 0xE5, 0x01);
	mipi_dbi_command(dbi, 0xB3, 0x00);
	mipi_dbi_command(dbi, 0xE5, 0x00);
	mipi_dbi_command(dbi, 0xF0, 0x36, 0xA5, 0x53);
	mipi_dbi_command(dbi, 0xE0, 0x00, 0x35, 0x33, 0x00, 0x00, 0x00,
			     0x00, 0x35, 0x33, 0x00, 0x00, 0x00);
	mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT, 0x55);
	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(50);
	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);

	mipi_dbi_command(dbi, 0xB6, 0x00, 0x22, 0x3B);
}


#define FBTFT_INIT_CMD		BIT(24)
#define FBTFT_INIT_DELAY	BIT(25)

static int fb_mipi_dbi_init_display_dt(struct mipi_dbi_dev *dbidev)
{
	struct device *dev = dbidev->drm.dev;
	struct mipi_dbi *dbi = &dbidev->dbi;
	struct property *prop;
	const __be32 *p;
	unsigned int i;
	u8 buf[64];
	u32 val;

	prop = of_find_property(dev->of_node, "init", NULL);
	p = of_prop_next_u32(prop, NULL, &val);
	if (!p)
		return 0;

	DRM_DEBUG_KMS("\n");

	while (p) {
		if (val & FBTFT_INIT_CMD) {
			val &= 0xffff;
			i = 0;
			while (p && !(val & 0xffff0000)) {
				if (i > 63) {
					dev_err(dev, "%s: Maximum register values exceeded\n", __func__);
					return -EINVAL;
				}
				buf[i++] = val;
				p = of_prop_next_u32(prop, p, &val);
			}

			mipi_dbi_command_buf(dbi, buf[0], &buf[1], i - 1);

		} else if (val & FBTFT_INIT_DELAY) {
			val &= 0xffff;
			DRM_DEBUG_DRIVER("msleep(%u)\n", val);
			msleep(val);
			p = of_prop_next_u32(prop, p, &val);
		} else {
			dev_err(dev, "illegal init value 0x%X\n", val);
			return -EINVAL;
		}
	}

	return 1;
}

static void fb_mipi_dbi_enable(struct drm_simple_display_pipe *pipe,
			       struct drm_crtc_state *crtc_state,
			       struct drm_plane_state *plane_state)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);
	struct fb_mipi_dbi *fbdbi = container_of(dbidev, struct fb_mipi_dbi, dbidev);
	struct mipi_dbi *dbi = &dbidev->dbi;
	int ret;

	DRM_DEBUG_KMS("\n");

	ret = mipi_dbi_poweron_reset(dbidev);
	if (ret < 0)
		return;

	ret = fb_mipi_dbi_init_display_dt(dbidev);
	if (ret < 0)
		return;
	if (ret)
		goto out_flush;

	switch (fbdbi->variant) {
	case MIPI_DBI_FB_HX8340BN:
		fb_hx8340bn_enable(dbi);
		fb_mipi_dbi_rotate(dbidev,
					0,
					MADCTL_MY | MADCTL_MV,
					MADCTL_MX | MADCTL_MY,
					MADCTL_MX | MADCTL_MV);
		break;
	case MIPI_DBI_FB_HX8353D:
		fb_hx8353d_enable(dbi);
		fb_mipi_dbi_rotate(dbidev,
					MADCTL_MX | MADCTL_MY,
					MADCTL_MX | MADCTL_MV,
					0,
					MADCTL_MY | MADCTL_MV);
		break;
	case MIPI_DBI_FB_HX8357D:
		fb_hx8357d_enable(dbi);
		fb_mipi_dbi_rotate(dbidev,
					MADCTL_MX | MADCTL_MY,
					MADCTL_MY | MADCTL_MV,
					0,
					MADCTL_MX | MADCTL_MV);
	case MIPI_DBI_FB_ILI9340:
		fb_ili9340_enable(dbi);
		fb_mipi_dbi_rotate(dbidev,
					MADCTL_MX,
					MADCTL_MV | MADCTL_MY | MADCTL_MX,
					MADCTL_MY,
					MADCTL_MV);
		break;
	case MIPI_DBI_FB_ILI9341:
		fb_ili9341_enable(dbi);
		fb_mipi_dbi_rotate(dbidev,
					MADCTL_MX,
					MADCTL_MV | MADCTL_MY | MADCTL_MX,
					MADCTL_MY,
					MADCTL_MV | MADCTL_ML);
		break;
	case MIPI_DBI_FB_ILI9481:
		fb_ili9481_enable(dbi);
		fb_mipi_dbi_rotate(dbidev,
					ILI9481_HFLIP,
					MADCTL_MV,
					ILI9481_VFLIP,
					MADCTL_MV | ILI9481_VFLIP | ILI9481_HFLIP);
		break;
	case MIPI_DBI_FB_ILI9486:
		fb_ili9486_enable(dbi);
		fb_mipi_dbi_rotate(dbidev,
					MADCTL_MY,
					MADCTL_MV,
					MADCTL_MX,
					MADCTL_MY | MADCTL_MX | MADCTL_MV);
		break;
	case MIPI_DBI_FB_S6D02A1:
		fb_s6d02a1_enable(dbi);
		fb_mipi_dbi_rotate(dbidev,
					MADCTL_MX | MADCTL_MY,
					MADCTL_MX | MADCTL_MV,
					0,
					MADCTL_MY | MADCTL_MV);
		break;
	case MIPI_DBI_FB_ST7735R:
		fb_st7735r_enable(dbi);
		fb_mipi_dbi_rotate(dbidev,
					MADCTL_MX | MADCTL_MY,
					MADCTL_MX | MADCTL_MV,
					0,
					MADCTL_MY | MADCTL_MV);
		break;
	case MIPI_DBI_FB_ST7789V:
		fb_st7789v_enable(dbi);
		fb_mipi_dbi_rotate(dbidev,
					0,
					MADCTL_MY | MADCTL_MV,
					MADCTL_MX | MADCTL_MY,
					MADCTL_MX | MADCTL_MV);
		break;
	case MIPI_DBI_FB_TINYLCD:
		fb_tinylcd_enable(dbi);
		fb_mipi_dbi_rotate(dbidev,
					0x08,
					0x38,
					0x58,
					0x28);
		break;
	};

out_flush:
	mipi_dbi_enable_flush(dbidev, crtc_state, plane_state);
}

static const struct drm_simple_display_pipe_funcs fb_mipi_dbi_funcs = {
	.enable = fb_mipi_dbi_enable,
	.disable = mipi_dbi_pipe_disable,
	.update = mipi_dbi_pipe_update,
	.prepare_fb = drm_gem_fb_simple_display_pipe_prepare_fb,
};

static void
fb_mipi_dbi_set_mode(struct drm_display_mode *mode, unsigned int width, unsigned int height)
{
	struct drm_display_mode set_mode = {
		DRM_SIMPLE_MODE(width, height, 0, 0),
	};

	*mode = set_mode;
}

DEFINE_DRM_GEM_CMA_FOPS(fb_mipi_dbi_fops);

static struct drm_driver fb_mipi_dbi_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &fb_mipi_dbi_fops,
	.release		= mipi_dbi_release,
	DRM_GEM_CMA_VMAP_DRIVER_OPS,
	.debugfs_init		= mipi_dbi_debugfs_init,
	.name			= "fb_mipi_dbi",
	.desc			= "MIPI DBI fbtft compatible driver",
	.date			= "20180413",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id fb_mipi_dbi_of_match[] = {
	{ .compatible = "himax,hx8340bn", .data = (void *)MIPI_DBI_FB_HX8340BN },
	{ .compatible = "himax,hx8353d", .data = (void *)MIPI_DBI_FB_HX8353D },
	{ .compatible = "himax,hx8357d", .data = (void *)MIPI_DBI_FB_HX8357D },
	{ .compatible = "ilitek,ili9340", .data = (void *)MIPI_DBI_FB_ILI9340 },
	{ .compatible = "ilitek,ili9341", .data = (void *)MIPI_DBI_FB_ILI9341 },
	{ .compatible = "ilitek,ili9481", .data = (void *)MIPI_DBI_FB_ILI9481 },
	{ .compatible = "ilitek,ili9486", .data = (void *)MIPI_DBI_FB_ILI9486 },
	{ .compatible = "samsung,s6d02a1", .data = (void *)MIPI_DBI_FB_S6D02A1 },
	{ .compatible = "sitronix,st7735r", .data = (void *)MIPI_DBI_FB_ST7735R },
	{ .compatible = "sitronix,st7789v", .data = (void *)MIPI_DBI_FB_ST7789V },
	{ .compatible = "neosec,tinylcd", .data = (void *)MIPI_DBI_FB_TINYLCD },
	{ },
};
MODULE_DEVICE_TABLE(of, fb_mipi_dbi_of_match);

static const struct spi_device_id fb_mipi_dbi_id[] = {
	{ "hx8340bn", (unsigned long)MIPI_DBI_FB_HX8340BN },
	{ "hx8353d", (unsigned long)MIPI_DBI_FB_HX8353D },
	{ "hx8357d", (unsigned long)MIPI_DBI_FB_HX8357D },
	{ "ili9340", (unsigned long)MIPI_DBI_FB_ILI9340 },
	{ "ili9341", (unsigned long)MIPI_DBI_FB_ILI9341 },
	{ "ili9481", (unsigned long)MIPI_DBI_FB_ILI9481 },
	{ "ili9486", (unsigned long)MIPI_DBI_FB_ILI9486 },
	{ "s6d02a1", (unsigned long)MIPI_DBI_FB_S6D02A1 },
	{ "st7735r", (unsigned long)MIPI_DBI_FB_ST7735R },
	{ "st7789v", (unsigned long)MIPI_DBI_FB_ST7789V },
	{ "tinylcd", (unsigned long)MIPI_DBI_FB_TINYLCD },
	{ },
};
MODULE_DEVICE_TABLE(spi, fb_mipi_dbi_id);

static void fb_mipi_dbi_prop_not_supported(struct device *dev, const char *propname)
{
	if (device_property_present(dev, propname))
		DRM_DEBUG_KMS("property not supported: %s\n", propname);
}

static int fb_mipi_dbi_probe(struct spi_device *spi)
{
	const struct spi_device_id *spi_id;
	const struct of_device_id *match;
	struct device *dev = &spi->dev;
	struct drm_display_mode mode;
	struct mipi_dbi_dev *dbidev;
	struct fb_mipi_dbi *fbdbi;
	struct drm_device *drm;
	u32 val, rotation = 0;
	struct mipi_dbi *dbi;
	struct gpio_desc *dc;
	int ret;

	fbdbi = kzalloc(sizeof(*fbdbi), GFP_KERNEL);
	if (!fbdbi)
		return -ENOMEM;

	dbidev = &fbdbi->dbidev;
	dbi = &dbidev->dbi;
	drm = &dbidev->drm;
	ret = devm_drm_dev_init(dev, drm, &fb_mipi_dbi_driver);
	if (ret) {
		kfree(fbdbi);
		return ret;
	}

	drm_mode_config_init(drm);

	match = of_match_device(fb_mipi_dbi_of_match, dev);
	if (match) {
		fbdbi->variant = (enum fb_mipi_dbi_variant)match->data;
	} else {
		spi_id = spi_get_device_id(spi);
		fbdbi->variant = (enum fb_mipi_dbi_variant)spi_id->driver_data;
	}

	switch (fbdbi->variant) {
	case MIPI_DBI_FB_HX8340BN:
		fb_mipi_dbi_set_mode(&mode, 176, 220);
		break;
	case MIPI_DBI_FB_HX8353D:
		fb_mipi_dbi_set_mode(&mode, 128, 160);
		break;
	case MIPI_DBI_FB_HX8357D:
		fb_mipi_dbi_set_mode(&mode, 320, 480);
		break;
	case MIPI_DBI_FB_ILI9340:
		fb_mipi_dbi_set_mode(&mode, 240, 320);
		break;
	case MIPI_DBI_FB_ILI9341:
		fb_mipi_dbi_set_mode(&mode, 240, 320);
		break;
	case MIPI_DBI_FB_ILI9481:
		fb_mipi_dbi_set_mode(&mode, 320, 480);
		break;
	case MIPI_DBI_FB_ILI9486:
		fb_mipi_dbi_set_mode(&mode, 320, 480);
		break;
	case MIPI_DBI_FB_S6D02A1:
		fb_mipi_dbi_set_mode(&mode, 128, 160);
		break;
	case MIPI_DBI_FB_ST7735R:
		fb_mipi_dbi_set_mode(&mode, 128, 160);
		break;
	case MIPI_DBI_FB_ST7789V:
		fb_mipi_dbi_set_mode(&mode, 240, 320);
		break;
	case MIPI_DBI_FB_TINYLCD:
		fb_mipi_dbi_set_mode(&mode, 320, 480);
		break;
	};

	if (!device_property_read_u32(dev, "width", &val)) {
		unsigned int width = val;

		if (!device_property_read_u32(dev, "height", &val))
			fb_mipi_dbi_set_mode(&mode, width, val);
	}

	if (!device_property_read_u32(dev, "debug", &val) && !drm_debug && val > 3)
		drm_debug = DRM_UT_CORE | DRM_UT_DRIVER | DRM_UT_KMS;

	fb_mipi_dbi_prop_not_supported(dev, "regwidth");
	fb_mipi_dbi_prop_not_supported(dev, "buswidth");
	fb_mipi_dbi_prop_not_supported(dev, "fps");
	fb_mipi_dbi_prop_not_supported(dev, "startbyte");
	fb_mipi_dbi_prop_not_supported(dev, "gamma");
	fb_mipi_dbi_prop_not_supported(dev, "txbuflen");

	dbi->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(dbi->reset)) {
		if (PTR_ERR(dbi->reset) != -EPROBE_DEFER)
			DRM_DEV_ERROR(dev, "Failed to get gpio 'reset'\n");
		return PTR_ERR(dbi->reset);
	}

	dc = devm_gpiod_get_optional(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(dc)) {
		if (PTR_ERR(dc) != -EPROBE_DEFER)
			DRM_DEV_ERROR(dev, "Failed to get gpio 'dc'\n");
		return PTR_ERR(dc);
	}

	dbidev->backlight = tinydrm_fbtft_get_backlight(dev);
	if (IS_ERR(dbidev->backlight))
		return PTR_ERR(dbidev->backlight);

	tinydrm_fbtft_get_rotation(dev, &rotation);

	ret = mipi_dbi_spi_init(spi, dbi, dc);
	if (ret)
		return ret;

	ret = mipi_dbi_dev_init(dbidev, &fb_mipi_dbi_funcs, &mode, rotation);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	spi_set_drvdata(spi, drm);

	drm_fbdev_generic_setup(drm, 16);

	return 0;
}

static int fb_mipi_dbi_remove(struct spi_device *spi)
{
	struct drm_device *drm = spi_get_drvdata(spi);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);

	return 0;
}

static void fb_mipi_dbi_shutdown(struct spi_device *spi)
{
	drm_atomic_helper_shutdown(spi_get_drvdata(spi));
}

static struct spi_driver fb_mipi_dbi_spi_driver = {
	.driver = {
		.name = "fb_mipi_dbi",
		.owner = THIS_MODULE,
		.of_match_table = fb_mipi_dbi_of_match,
	},
	.id_table = fb_mipi_dbi_id,
	.probe = fb_mipi_dbi_probe,
	.remove = fb_mipi_dbi_remove,
	.shutdown = fb_mipi_dbi_shutdown,
};
module_spi_driver(fb_mipi_dbi_spi_driver);

MODULE_DESCRIPTION("MIPI DBI fbtft compatible driver");
MODULE_AUTHOR("Noralf Trønnes");
MODULE_LICENSE("GPL");
