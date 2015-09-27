/*
 * Copyright (C) 2015 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifdef CONFIG_DRM_KMS_FB_HELPER
int tinydrm_fbdev_init(struct tinydrm_device *tdev);
void tinydrm_fbdev_fini(struct tinydrm_device *tdev);
#else
static inline int tinydrm_fbdev_init(struct tinydrm_device *tdev)
{
	return 0;
}

static inline void tinydrm_fbdev_fini(struct tinydrm_device *tdev) { }
#endif
