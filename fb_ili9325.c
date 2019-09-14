/*
 * FB driver for the ILI9325 LCD ili9325
 *
 * Copyright (C) 2013 Noralf Tronnes
 *
 * Based on ili9325.c by Jeroen Domburg
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

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>

#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_vblank.h>
#include <drm/tinydrm/tinydrm-ili9325.h>
#include <drm/tinydrm/tinydrm-regmap.h>

#include "tinydrm-fbtft.h"

#define FB_ILI9325_DEFAULT_GAMMA	"0F 00 7 2 0 0 6 5 4 1\n" \
					"04 16 2 7 6 3 2 1 7 7"

#define FB_ILI9320_DEFAULT_GAMMA	"07 07 6 0 0 0 5 5 4 0\n" \
					"07 08 4 7 5 1 2 0 7 7"

static bool no_rotation;
module_param(no_rotation, bool, 0444);
MODULE_PARM_DESC(no_rotation, "Don't set rotation register");

static unsigned int bt = 6; /* VGL=Vci*4 , VGH=Vci*4 */
module_param(bt, uint, 0444);
MODULE_PARM_DESC(bt, "Sets the factor used in the step-up circuits (ili9325)");

static unsigned int vc = 0x03; /* Vci1=Vci*0.80 */
module_param(vc, uint, 0444);
MODULE_PARM_DESC(vc, "Sets the ratio factor of Vci to generate the reference voltages Vci1 (ili9325)");

static unsigned int vrh = 0x0d; /* VREG1OUT=Vci*1.85 */
module_param(vrh, uint, 0444);
MODULE_PARM_DESC(vrh, "Set the amplifying rate (1.6 ~ 1.9) of Vci applied to output the VREG1OUT (ili9325)");

static unsigned int vdv = 0x12; /* VCOMH amplitude=VREG1OUT*0.98 */
module_param(vdv, uint, 0444);
MODULE_PARM_DESC(vdv, "Select the factor of VREG1OUT to set the amplitude of Vcom (ili9325)");

static unsigned int vcm = 0x0a; /* VCOMH=VREG1OUT*0.735 */
module_param(vcm, uint, 0444);
MODULE_PARM_DESC(vcm, "Set the internal VcomH voltage (ili9325)");

/*
 * Verify that this configuration is within the Voltage limits
 *
 * Display module configuration: Vcc = IOVcc = Vci = 3.3V
 *
 * Voltages
 * ----------
 * Vci                                =   3.3
 * Vci1           =  Vci * 0.80       =   2.64
 * DDVDH          =  Vci1 * 2         =   5.28
 * VCL            = -Vci1             =  -2.64
 * VREG1OUT       =  Vci * 1.85       =   4.88
 * VCOMH          =  VREG1OUT * 0.735 =   3.59
 * VCOM amplitude =  VREG1OUT * 0.98  =   4.79
 * VGH            =  Vci * 4          =  13.2
 * VGL            = -Vci * 4          = -13.2
 *
 * Limits
 * --------
 * Power supplies
 * 1.65 < IOVcc < 3.30   =>  1.65 < 3.3 < 3.30
 * 2.40 < Vcc   < 3.30   =>  2.40 < 3.3 < 3.30
 * 2.50 < Vci   < 3.30   =>  2.50 < 3.3 < 3.30
 *
 * Source/VCOM power supply voltage
 *  4.50 < DDVDH < 6.0   =>  4.50 <  5.28 <  6.0
 * -3.0  < VCL   < -2.0  =>  -3.0 < -2.64 < -2.0
 * VCI - VCL < 6.0       =>  5.94 < 6.0
 *
 * Gate driver output voltage
 *  10  < VGH   < 20     =>   10 <  13.2  < 20
 * -15  < VGL   < -5     =>  -15 < -13.2  < -5
 * VGH - VGL < 32        =>   26.4 < 32
 *
 * VCOM driver output voltage
 * VCOMH - VCOML < 6.0   =>  4.79 < 6.0
 */

static void tinydrm_ili9325_reset(struct tinydrm_ili9325 *controller)
{
	if (IS_ERR_OR_NULL(controller->reset))
		return;

	gpiod_set_value_cansleep(controller->reset, 0);
	msleep(1);
	gpiod_set_value_cansleep(controller->reset, 1);
	msleep(10);
}

static void tinydrm_ili9325_set_rotation(struct tinydrm_ili9325 *ili9325)
{
	struct device *dev = ili9325->tinydrm.drm->dev;
	struct regmap *reg = ili9325->reg;
	bool bgr;

	if (no_rotation)
		return;

	bgr = device_property_read_bool(dev, "bgr");

	switch (ili9325->rotation) {
	/* AM: GRAM update direction */
	case 0:
		regmap_write(reg, 0x0003, 0x0030 | (bgr << 12));
		break;
	case 180:
		regmap_write(reg, 0x0003, 0x0000 | (bgr << 12));
		break;
	case 270:
		regmap_write(reg, 0x0003, 0x0028 | (bgr << 12));
		break;
	case 90:
		regmap_write(reg, 0x0003, 0x0018 | (bgr << 12));
		break;
	}
}

/*
 * Gamma string format:
 *  VRP0 VRP1 RP0 RP1 KP0 KP1 KP2 KP3 KP4 KP5
 *  VRN0 VRN1 RN0 RN1 KN0 KN1 KN2 KN3 KN4 KN5
 */
static void tinydrm_ili9325_set_gamma(struct tinydrm_ili9325 *ili9325,
				      u16 *curves)
{
	struct regmap *reg = ili9325->reg;
	u16 vrp[2] = { curves[0] & 0x1f, curves[1] & 0x1f };
	u16 rp[2] = { curves[2] & 0x07, curves[3] & 0x07 };
	u16 kp[6];
	u16 vrn[2] = { curves[10] & 0x1f, curves[11] & 0x1f };
	u16 rn[2] = { curves[12] & 0x07, curves[13] & 0x07 };
	u16 kn[6];
	int i;

	for (i = 0; i < 6; i++) {
		kp[i] = curves[i + 4] & 0x7;
		kn[i] = curves[i + 4 + 10] & 0x7;
	}

	regmap_write(reg, 0x0030, kp[1] << 8 | kp[0]);
	regmap_write(reg, 0x0031, kp[3] << 8 | kp[2]);
	regmap_write(reg, 0x0032, kp[5] << 8 | kp[4]);
	regmap_write(reg, 0x0035, rp[1] << 8 | rp[0]);
	regmap_write(reg, 0x0036, vrp[1] << 8 | vrp[0]);

	regmap_write(reg, 0x0037, kn[1] << 8 | kn[0]);
	regmap_write(reg, 0x0038, kn[3] << 8 | kn[2]);
	regmap_write(reg, 0x0039, kn[5] << 8 | kn[4]);
	regmap_write(reg, 0x003C, rn[1] << 8 | rn[0]);
	regmap_write(reg, 0x003D, vrn[1] << 8 | vrn[0]);
}

static void fb_ili9325_pipe_enable(struct drm_simple_display_pipe *pipe,
				   struct drm_crtc_state *crtc_state,
				   struct drm_plane_state *plane_state)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct tinydrm_ili9325 *ili9325 = tinydrm_to_ili9325(tdev);
	struct drm_framebuffer *fb = pipe->plane.fb;
	struct regmap *reg = ili9325->reg;
	struct device *dev = tdev->drm->dev;
	u16 gamma_curves[2 * 10];
	unsigned int devcode;
	int ret;

	ret = tinydrm_fbtft_get_gamma(dev, gamma_curves,
				      FB_ILI9325_DEFAULT_GAMMA, 2, 10);
	if (ret) {
		dev_err(dev, "Failed to get gamma\n");
		return;
	}

	tinydrm_ili9325_reset(ili9325);

	if (!regmap_read(reg, 0x0000, &devcode))
		DRM_DEBUG_DRIVER("devcode=%x\n", devcode);

	ret = tinydrm_fbtft_init(dev, reg);
	if (!ret) {
		goto set_rotation;
	} else if (ret != -ENOENT) {
		dev_err(dev, "tinydrm_fbtft_init failed\n");
		return;
	}

	bt &= 0x07;
	vc &= 0x07;
	vrh &= 0x0f;
	vdv &= 0x1f;
	vcm &= 0x3f;

	/* Initialization sequence from ILI9325 Application Notes */

	ret = regmap_write(reg, 0x00E3, 0x3008); /* Set internal timing */
	if (ret) {
		dev_err(dev, "Failed to write register\n");
		return;
	}

	regmap_write(reg, 0x00E7, 0x0012); /* Set internal timing */
	regmap_write(reg, 0x00EF, 0x1231); /* Set internal timing */
	regmap_write(reg, 0x0001, 0x0100); /* set SS and SM bit */
	regmap_write(reg, 0x0002, 0x0700); /* set 1 line inversion */
	regmap_write(reg, 0x0004, 0x0000); /* Resize register */
	regmap_write(reg, 0x0008, 0x0207); /* set the back porch and front porch */
	regmap_write(reg, 0x0009, 0x0000); /* set non-display area refresh cycle */
	regmap_write(reg, 0x000A, 0x0000); /* FMARK function */
	regmap_write(reg, 0x000C, 0x0000); /* RGB interface setting */
	regmap_write(reg, 0x000D, 0x0000); /* Frame marker Position */
	regmap_write(reg, 0x000F, 0x0000); /* RGB interface polarity */

	/* ----------- Power On sequence ----------- */
	regmap_write(reg, 0x0010, 0x0000); /* SAP, BT[3:0], AP, DSTB, SLP, STB */
	regmap_write(reg, 0x0011, 0x0007); /* DC1[2:0], DC0[2:0], VC[2:0] */
	regmap_write(reg, 0x0012, 0x0000); /* VREG1OUT voltage */
	regmap_write(reg, 0x0013, 0x0000); /* VDV[4:0] for VCOM amplitude */
	msleep(200); /* Dis-charge capacitor power voltage */
	regmap_write(reg, 0x0010, /* SAP, BT[3:0], AP, DSTB, SLP, STB */
		(1 << 12) | (bt << 8) | (1 << 7) | (0x01 << 4));
	regmap_write(reg, 0x0011, 0x220 | vc); /* DC1[2:0], DC0[2:0], VC[2:0] */
	msleep(50);
	regmap_write(reg, 0x0012, vrh); /* Internal reference voltage= Vci; */
	msleep(50);
	regmap_write(reg, 0x0013, vdv << 8); /* Set VDV[4:0] for VCOM amplitude */
	regmap_write(reg, 0x0029, vcm); /* Set VCM[5:0] for VCOMH */
	regmap_write(reg, 0x002B, 0x000C); /* Set Frame Rate */
	msleep(50);
	regmap_write(reg, 0x0020, 0x0000); /* GRAM horizontal Address */
	regmap_write(reg, 0x0021, 0x0000); /* GRAM Vertical Address */

	/*------------------ Set GRAM area --------------- */
	regmap_write(reg, 0x0050, 0x0000); /* Horizontal GRAM Start Address */
	regmap_write(reg, 0x0051, 0x00EF); /* Horizontal GRAM End Address */
	regmap_write(reg, 0x0052, 0x0000); /* Vertical GRAM Start Address */
	regmap_write(reg, 0x0053, 0x013F); /* Vertical GRAM Start Address */
	regmap_write(reg, 0x0060, 0xA700); /* Gate Scan Line */
	regmap_write(reg, 0x0061, 0x0001); /* NDL,VLE, REV */
	regmap_write(reg, 0x006A, 0x0000); /* set scrolling line */

	/*-------------- Partial Display Control --------- */
	regmap_write(reg, 0x0080, 0x0000);
	regmap_write(reg, 0x0081, 0x0000);
	regmap_write(reg, 0x0082, 0x0000);
	regmap_write(reg, 0x0083, 0x0000);
	regmap_write(reg, 0x0084, 0x0000);
	regmap_write(reg, 0x0085, 0x0000);

	/*-------------- Panel Control ------------------- */
	regmap_write(reg, 0x0090, 0x0010);
	regmap_write(reg, 0x0092, 0x0600);
	regmap_write(reg, 0x0007, 0x0133); /* 262K color and display ON */

set_rotation:
	tinydrm_ili9325_set_rotation(ili9325);
	tinydrm_ili9325_set_gamma(ili9325, gamma_curves);

	ili9325->enabled = true;
	fb->funcs->dirty(fb, NULL, 0, 0, NULL, 0);

	backlight_enable(ili9325->backlight);
}

static void fb_ili9325_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct tinydrm_ili9325 *ili9325 = tinydrm_to_ili9325(tdev);

	ili9325->enabled = false;
}

static void fb_ili9325_pipe_update(struct drm_simple_display_pipe *pipe,
				   struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_rect rect;

	if (drm_atomic_helper_damage_merged(old_state, state, &rect))
		tinydrm_ili9325_fb_dirty(state->fb, &rect);

	if (crtc->state->event) {
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		spin_unlock_irq(&crtc->dev->event_lock);
		crtc->state->event = NULL;
	}
}

static const struct drm_simple_display_pipe_funcs fb_ili9325_funcs = {
	.enable =  fb_ili9325_pipe_enable,
	.disable = fb_ili9325_pipe_disable,
	.update = fb_ili9325_pipe_update,
	.prepare_fb = drm_gem_fb_simple_display_pipe_prepare_fb,
};

static void fb_ili9320_pipe_enable(struct drm_simple_display_pipe *pipe,
				   struct drm_crtc_state *crtc_state,
				   struct drm_plane_state *plane_state)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct tinydrm_ili9325 *ili9325 = tinydrm_to_ili9325(tdev);
	struct drm_framebuffer *fb = pipe->plane.fb;
	struct regmap *reg = ili9325->reg;
	struct device *dev = tdev->drm->dev;
	u16 gamma_curves[2 * 10];
	unsigned int devcode;
	int ret;

	ret = tinydrm_fbtft_get_gamma(dev, gamma_curves,
				      FB_ILI9320_DEFAULT_GAMMA, 2, 10);
	if (ret) {
		dev_err(dev, "Failed to get gamma\n");
		return;
	}

	tinydrm_ili9325_reset(ili9325);

	if (!regmap_read(reg, 0x0000, &devcode))
		DRM_DEBUG_DRIVER("devcode=%x\n", devcode);

	ret = tinydrm_fbtft_init(dev, reg);
	if (!ret) {
		goto set_rotation;
	} else if (ret != -ENOENT) {
		dev_err(dev, "tinydrm_fbtft_init failed\n");
		return;
	}

	/* Initialization sequence from ILI9320 Application Notes */

	/* Set the Vcore voltage and this setting is must. */
	regmap_write(reg, 0x00E5, 0x8000);

	/* Start internal OSC. */
	regmap_write(reg, 0x0000, 0x0001);

	/* set SS and SM bit */
	regmap_write(reg, 0x0001, 0x0100);

	/* set 1 line inversion */
	regmap_write(reg, 0x0002, 0x0700);

	/* Resize register */
	regmap_write(reg, 0x0004, 0x0000);

	/* set the back and front porch */
	regmap_write(reg, 0x0008, 0x0202);

	/* set non-display area refresh cycle */
	regmap_write(reg, 0x0009, 0x0000);

	/* FMARK function */
	regmap_write(reg, 0x000A, 0x0000);

	/* RGB interface setting */
	regmap_write(reg, 0x000C, 0x0000);

	/* Frame marker Position */
	regmap_write(reg, 0x000D, 0x0000);

	/* RGB interface polarity */
	regmap_write(reg, 0x000F, 0x0000);

	/* ***********Power On sequence *************** */
	/* SAP, BT[3:0], AP, DSTB, SLP, STB */
	regmap_write(reg, 0x0010, 0x0000);

	/* DC1[2:0], DC0[2:0], VC[2:0] */
	regmap_write(reg, 0x0011, 0x0007);

	/* VREG1OUT voltage */
	regmap_write(reg, 0x0012, 0x0000);

	/* VDV[4:0] for VCOM amplitude */
	regmap_write(reg, 0x0013, 0x0000);

	/* Dis-charge capacitor power voltage */
	mdelay(200);

	/* SAP, BT[3:0], AP, DSTB, SLP, STB */
	regmap_write(reg, 0x0010, 0x17B0);

	/* R11h=0x0031 at VCI=3.3V DC1[2:0], DC0[2:0], VC[2:0] */
	regmap_write(reg, 0x0011, 0x0031);
	mdelay(50);

	/* R12h=0x0138 at VCI=3.3V VREG1OUT voltage */
	regmap_write(reg, 0x0012, 0x0138);
	mdelay(50);

	/* R13h=0x1800 at VCI=3.3V VDV[4:0] for VCOM amplitude */
	regmap_write(reg, 0x0013, 0x1800);

	/* R29h=0x0008 at VCI=3.3V VCM[4:0] for VCOMH */
	regmap_write(reg, 0x0029, 0x0008);
	mdelay(50);

	/* GRAM horizontal Address */
	regmap_write(reg, 0x0020, 0x0000);

	/* GRAM Vertical Address */
	regmap_write(reg, 0x0021, 0x0000);

	/* ------------------ Set GRAM area --------------- */
	/* Horizontal GRAM Start Address */
	regmap_write(reg, 0x0050, 0x0000);

	/* Horizontal GRAM End Address */
	regmap_write(reg, 0x0051, 0x00EF);

	/* Vertical GRAM Start Address */
	regmap_write(reg, 0x0052, 0x0000);

	/* Vertical GRAM End Address */
	regmap_write(reg, 0x0053, 0x013F);

	/* Gate Scan Line */
	regmap_write(reg, 0x0060, 0x2700);

	/* NDL,VLE, REV */
	regmap_write(reg, 0x0061, 0x0001);

	/* set scrolling line */
	regmap_write(reg, 0x006A, 0x0000);

	/* -------------- Partial Display Control --------- */
	regmap_write(reg, 0x0080, 0x0000);
	regmap_write(reg, 0x0081, 0x0000);
	regmap_write(reg, 0x0082, 0x0000);
	regmap_write(reg, 0x0083, 0x0000);
	regmap_write(reg, 0x0084, 0x0000);
	regmap_write(reg, 0x0085, 0x0000);

	/* -------------- Panel Control ------------------- */
	regmap_write(reg, 0x0090, 0x0010);
	regmap_write(reg, 0x0092, 0x0000);
	regmap_write(reg, 0x0093, 0x0003);
	regmap_write(reg, 0x0095, 0x0110);
	regmap_write(reg, 0x0097, 0x0000);
	regmap_write(reg, 0x0098, 0x0000);
	regmap_write(reg, 0x0007, 0x0173); /* 262K color and display ON */

set_rotation:
	tinydrm_ili9325_set_rotation(ili9325);
	tinydrm_ili9325_set_gamma(ili9325, gamma_curves);

	ili9325->enabled = true;
	fb->funcs->dirty(fb, NULL, 0, 0, NULL, 0);

	backlight_enable(ili9325->backlight);
}

static const struct drm_simple_display_pipe_funcs fb_ili9320_funcs = {
	.enable =  fb_ili9320_pipe_enable,
	.disable = fb_ili9325_pipe_disable,
	.update = fb_ili9325_pipe_update,
	.prepare_fb = drm_gem_fb_simple_display_pipe_prepare_fb,
};

static const struct drm_display_mode fb_ili9325_mode = {
	TINYDRM_MODE(240, 320, 0, 0),
};

static struct drm_driver fb_ili9325_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME |
				  DRIVER_ATOMIC,
	DRM_GEM_CMA_VMAP_DRIVER_OPS,
	.debugfs_init		= tinydrm_ili9325_debugfs_init,
	.name			= "fb_ili9325",
	.desc			= "fb_ili9325",
	.date			= "20170207",
	.major			= 1,
	.minor			= 0,
};

static struct tinydrm_ili9325 *
fb_ili9325_probe_common(struct device *dev, struct regmap *reg,
			const struct drm_simple_display_pipe_funcs *funcs)
{
	struct tinydrm_ili9325 *ili9325;
	struct tinydrm_device *tdev;
	u32 rotation = 0;
	int ret;

	ili9325 = devm_kzalloc(dev, sizeof(*ili9325), GFP_KERNEL);
	if (!ili9325)
		return ERR_PTR(-ENOMEM);

	ili9325->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ili9325->reset)) {
		dev_err(dev, "Failed to get gpio 'reset'\n");
		return ERR_CAST(ili9325->reset);
	}

	ili9325->backlight = tinydrm_fbtft_get_backlight(dev);
	if (IS_ERR(ili9325->backlight))
		return ERR_CAST(ili9325->backlight);

	tinydrm_fbtft_get_rotation(dev, &rotation);

	ret = tinydrm_ili9325_init(dev, ili9325, funcs, reg, &fb_ili9325_driver,
				   &fb_ili9325_mode, rotation);
	if (ret)
		return ERR_PTR(ret);

	tdev = &ili9325->tinydrm;
	ret = devm_tinydrm_register(tdev);
	if (ret)
		return ERR_PTR(ret);

	return ili9325;
}

#if 0
static void fb_ili9325_shutdown(struct spi_device *spi)
{
	struct tinydrm_regmap *treg = to_tinydrm_regmap(tdev);

	tinydrm_shutdown(&treg->tinydrm);
}

#endif

static const struct of_device_id fb_ili9325_of_match[] = {
	{ .compatible = "ilitek,ili9320", .data = &fb_ili9320_funcs },
	{ .compatible = "ilitek,ili9325", .data = &fb_ili9325_funcs },
	{},
};
MODULE_DEVICE_TABLE(of, fb_ili9325_of_match);

static int fb_ili9325_probe_spi(struct spi_device *spi)
{
	const struct drm_simple_display_pipe_funcs *funcs;
	const struct spi_device_id *spi_id;
	const struct of_device_id *match;
	struct tinydrm_ili9325 *ili9325;
	struct device *dev = &spi->dev;
	struct regmap *reg;
	int ret;

	match = of_match_device(fb_ili9325_of_match, dev);
	if (match) {
		funcs = match->data;
	} else {
		spi_id = spi_get_device_id(spi);
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

	reg = tinydrm_ili9325_spi_init(spi, 0);
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	ili9325 = fb_ili9325_probe_common(dev, reg, funcs);
	if (IS_ERR(ili9325))
		return PTR_ERR(ili9325);

	spi_set_drvdata(spi, ili9325->tinydrm.drm);

	DRM_DEV_DEBUG_DRIVER(dev, "Initialized @%uMHz on minor %d\n",
			     spi->max_speed_hz / 1000000,
			     ili9325->tinydrm.drm->primary->index);

	return 0;
}

static int fb_ili9325_probe_pdev(struct platform_device *pdev)
{
	const struct drm_simple_display_pipe_funcs *funcs;
	const struct platform_device_id *pdev_id;
	const struct of_device_id *match;
	struct device *dev = &pdev->dev;
	struct tinydrm_ili9325 *ili9325;
	struct gpio_desc *cs, *dc, *wr;
	struct gpio_descs *db;
	struct regmap *reg;

	match = of_match_device(fb_ili9325_of_match, dev);
	if (match) {
		funcs = match->data;
	} else {
		pdev_id = platform_get_device_id(pdev);
		funcs = (const struct drm_simple_display_pipe_funcs *)pdev_id->driver_data;
	}

	cs = devm_gpiod_get_optional(dev, "cs", GPIOD_OUT_HIGH);
	if (IS_ERR(cs)) {
		dev_err(dev, "Failed to get gpio 'cs'\n");
		return PTR_ERR(cs);
	}

	dc = devm_gpiod_get(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(dc)) {
		dev_err(dev, "Failed to get gpio 'dc'\n");
		return PTR_ERR(dc);
	}

	wr = devm_gpiod_get(dev, "wr", GPIOD_OUT_HIGH);
	if (IS_ERR(wr)) {
		dev_err(dev, "Failed to get gpio 'wr'\n");
		return PTR_ERR(wr);
	}

	db = devm_gpiod_get_array(dev, "db", GPIOD_OUT_LOW);
	if (IS_ERR(db)) {
		dev_err(dev, "Failed to get gpios 'db'\n");
		return PTR_ERR(db);
	}

	reg = tinydrm_i80_init(dev, 16, cs, dc, wr, db);
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	ili9325 = fb_ili9325_probe_common(dev, reg, funcs);
	if (IS_ERR(ili9325))
		return PTR_ERR(ili9325);

	platform_set_drvdata(pdev, ili9325->tinydrm.drm);

	DRM_DEV_DEBUG_DRIVER(dev, "Initialized on minor %d\n",
			     ili9325->tinydrm.drm->primary->index);

	return 0;
}

static const struct spi_device_id fb_ili9325_spi_ids[] = {
	{ "fb_ili9320", (unsigned long)&fb_ili9320_funcs },
	{ "fb_ili9325", (unsigned long)&fb_ili9325_funcs },
	{ },
};
MODULE_DEVICE_TABLE(spi, fb_ili9325_spi_ids);

static struct spi_driver fb_ili9325_spi_driver = {
	.driver = {
		.name   = "fb_ili9325",
		.owner  = THIS_MODULE,
		.of_match_table = of_match_ptr(fb_ili9325_of_match),
	},
	.id_table = fb_ili9325_spi_ids,
	.probe = fb_ili9325_probe_spi,
};

static const struct platform_device_id fb_ili9325_platform_ids[] = {
	{ "fb_ili9320", (unsigned long)&fb_ili9320_funcs },
	{ "fb_ili9325", (unsigned long)&fb_ili9325_funcs },
	{ },
};
MODULE_DEVICE_TABLE(platform, fb_ili9325_platform_ids);

static struct platform_driver fb_ili9325_platform_driver = {
	.driver = {
		.name   = "fb_ili9325",
		.owner  = THIS_MODULE,
		.of_match_table = of_match_ptr(fb_ili9325_of_match),
	},
	.id_table = fb_ili9325_platform_ids,
	.probe = fb_ili9325_probe_pdev,
};

static int __init fb_ili9325_module_init(void)
{
	int ret;

	ret = spi_register_driver(&fb_ili9325_spi_driver);
	if (ret < 0)
		return ret;
	return platform_driver_register(&fb_ili9325_platform_driver);
}
module_init(fb_ili9325_module_init);

static void __exit fb_ili9325_module_exit(void)
{
	spi_unregister_driver(&fb_ili9325_spi_driver);
	platform_driver_unregister(&fb_ili9325_platform_driver);
}
module_exit(fb_ili9325_module_exit);

MODULE_ALIAS("spi:ili9320");
MODULE_ALIAS("platform:ili9320");
MODULE_ALIAS("spi:ili9325");
MODULE_ALIAS("platform:ili9325");

MODULE_DESCRIPTION("DRM driver for the ILI9325 LCD ili9325");
MODULE_AUTHOR("Noralf Trønnes");
MODULE_LICENSE("GPL");
