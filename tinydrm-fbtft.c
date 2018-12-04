/*
 * Copyright 2017 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <drm/tinydrm/tinydrm.h>
#include <drm/tinydrm/tinydrm-helpers2.h>

#define FBTFT_INIT_CMD		BIT(24)
#define FBTFT_INIT_DELAY	BIT(25)


/*
 * These functions provides backward Device Tree support for drivers converted
 * from drivers/staging/fbtft.
 *
 * They should NOT be used by new drivers.
 */

/*
 * tinydrm_fbtft_dt_init - Initialize from device property
 * @dev: Device
 * @reg: Register map
 *
 * If the 'init' property exists, apply the register settings.
 *
 * Returns:
 * Zero on success, -ENOENT if the property doesn't exist, negative error code
 * on other failures.
 */
int tinydrm_fbtft_init(struct device *dev, struct regmap *reg)
{
	u32 *vals, regvals[64], *regnr = NULL;
	int ret, num_vals, i, j;
	void *buf;

	if (!device_property_present(dev, "init"))
		return -ENOENT;

	DRM_DEBUG_DRIVER("\n");

	num_vals = device_property_read_u32_array(dev, "init", NULL, 0);
	if (num_vals <= 0)
		return num_vals;

	buf = kcalloc(num_vals, sizeof(u32), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	vals = buf;
	ret = device_property_read_u32_array(dev, "init", vals, num_vals);
	if (ret < 0)
		goto err_free;

	for (i = 0; i < num_vals; i++) {
		if (vals[i] & FBTFT_INIT_DELAY) {
			vals[i] &= 0xffff;
			DRM_DEBUG_DRIVER("init: msleep(%u)\n", vals[i]);
			msleep(vals[i]);
		} else if (vals[i] & FBTFT_INIT_CMD) {
			vals[i] &= 0xffff;
			regnr = &vals[i];
			j = 0;
		} else if (!regnr) {
			dev_err(dev, "init: illegal value 0x%X\n", vals[i]);
			ret = -EINVAL;
			goto err_free;
		} else {
			if (j > 63) {
				dev_err(dev, "init: Maximum register values exceeded\n");
				ret = -EINVAL;
				goto err_free;
			}
			regvals[j++] = vals[i];
		}

		/* write values if this is the last entry or regvalue */
		if (regnr &&
		    (i == (num_vals - 1) || vals[i + 1] & 0xffff0000)) {
			/* FIXME support mipi dcs */
			if (WARN_ON_ONCE(j != 1))
				return -EINVAL;

			ret = regmap_write(reg, *regnr, regvals[0]);
			if (ret)
				goto err_free;

			regnr = NULL;
		}
	}

err_free:
	kfree(buf);

	return ret;
}
EXPORT_SYMBOL(tinydrm_fbtft_init);

static int get_next_ulong(char **str_p, unsigned long *val, char *sep, int base)
{
	char *p_val;

	if (!str_p || !(*str_p))
		return -EINVAL;

	p_val = strsep(str_p, sep);

	if (!p_val)
		return -EINVAL;

	return kstrtoul(p_val, base, val);
}

/*
 * tinydrm_fbtft_get_gamma - Get gamma curve
 * @dev - Device struct
 * @curves: Store curve(s) here
 * @gamma_str: Text representation of gamma curve
 * @num_curves: Number of gamma curves
 * @num_values: Number of values in each curve
 *
 * If the device property 'gamma' exists, then this is used.
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int tinydrm_fbtft_get_gamma(struct device *dev, u16 *curves,
			    const char *gamma_str, size_t num_curves,
			    size_t num_values)
{
	char *str_p, *curve_p = NULL;
	char *tmp;
	unsigned long val = 0;
	int ret = 0;
	size_t curve_counter, value_counter;

	if (device_property_present(dev, "gamma")) {
		ret = device_property_read_string(dev, "gamma", &gamma_str);
		if (ret)
			return ret;
	}

	DRM_DEBUG_DRIVER("gamma='%s'\n", gamma_str);

	tmp = kstrdup(gamma_str, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	/* replace optional separators */
	str_p = tmp;
	while (*str_p) {
		if (*str_p == ',')
			*str_p = ' ';
		if (*str_p == ';')
			*str_p = '\n';
		str_p++;
	}

	str_p = strim(tmp);

	curve_counter = 0;
	while (str_p) {
		if (curve_counter == num_curves) {
			DRM_ERROR("Gamma: Too many curves\n");
			ret = -EINVAL;
			goto out;
		}
		curve_p = strsep(&str_p, "\n");
		value_counter = 0;
		while (curve_p) {
			if (value_counter == num_values) {
				DRM_ERROR("Gamma: Too many values\n");
				ret = -EINVAL;
				goto out;
			}
			ret = get_next_ulong(&curve_p, &val, " ", 16);
			if (ret)
				goto out;
			curves[curve_counter * num_values + value_counter] = val;
			value_counter++;
		}
		if (value_counter != num_values) {
			DRM_ERROR("Gamma: Too few values\n");
			ret = -EINVAL;
			goto out;
		}
		curve_counter++;
	}
	if (curve_counter != num_curves) {
		DRM_ERROR("Gamma: Too few curves\n");
		ret = -EINVAL;
		goto out;
	}

out:
	kfree(tmp);

	return ret;
}
EXPORT_SYMBOL(tinydrm_fbtft_get_gamma);

/*
 * tinydrm_fbtft_get_rotation - Get fbtft compatible rotation value
 * @dev: Device structure
 * @rotation: Returned rotation value
 *
 * Get 'rotation' property if it exist, fall back to 'rotate' for backwards
 * compatibility,
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int tinydrm_fbtft_get_rotation(struct device *dev, u32 *rotation)
{
	if (device_property_present(dev, "rotation"))
		return device_property_read_u32(dev, "rotation", rotation);

	if (device_property_present(dev, "rotate"))
		return device_property_read_u32(dev, "rotate", rotation);

	return 0;
}
EXPORT_SYMBOL(tinydrm_fbtft_get_rotation);

#if IS_ENABLED(CONFIG_BACKLIGHT_CLASS_DEVICE)
static int tinydrm_fbtft_backlight_update_status(struct backlight_device *bl)
{
	struct gpio_desc *led = bl_get_data(bl);
	int brightness = bl->props.brightness;

	if (bl->props.power != FB_BLANK_UNBLANK ||
	    bl->props.fb_blank != FB_BLANK_UNBLANK ||
	    bl->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
		brightness = 0;

	gpiod_set_value_cansleep(led, !brightness);

	return 0;
}

static int tinydrm_fbtft_backlight_get_brightness(struct backlight_device *bl)
{
	return bl->props.brightness;
}

static const struct backlight_ops tinydrm_fbtft_bl_ops = {
	.get_brightness	= tinydrm_fbtft_backlight_get_brightness,
	.update_status	= tinydrm_fbtft_backlight_update_status,
};

/*
 * tinydrm_fbtft_get_backlight - Get fbtft compatible backlight
 * @dev: Device structure
 *
 * This function tries tinydrm_of_find_backlight() first to check for
 * backlight, if that doesn't exist, it falls back to setting up a backlight
 * device for the 'leds' gpio if it exist.
 * Due to a bug the original fbtft code, the active state of the leds-gpio is
 * inverted.
 *
 * Returns:
 * NULL if no backlight is found, &backlight_device if found, and PTR_ERR on
 * failure.
 */
struct backlight_device *tinydrm_fbtft_get_backlight(struct device *dev)
{
	struct backlight_properties bl_props = {
		.type = BACKLIGHT_RAW,
		.brightness = 1,
		.state = BL_CORE_FBBLANK,
	};
	struct backlight_device *bl;
	struct gpio_desc *led;

	bl = devm_of_find_backlight(dev);
	if (bl)
		return bl;

	led = devm_gpiod_get_optional(dev, "led", GPIOD_OUT_HIGH);
	if (IS_ERR(led)) {
		dev_err(dev, "Failed to get gpio 'led'\n");
		return ERR_CAST(led);
	}

	if (!led)
		return NULL;

	return devm_backlight_device_register(dev, dev_driver_string(dev), dev,
					      led, &tinydrm_fbtft_bl_ops,
					      &bl_props);
}
EXPORT_SYMBOL(tinydrm_fbtft_get_backlight);

#endif
