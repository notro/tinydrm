/*
 * ILI9340 LCD controller
 *
 * Copyright 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __LINUX_ILI9340_H
#define __LINUX_ILI9340_H

#define ILI9340_FRMCTR1    0xB1
#define ILI9340_FRMCTR2    0xB2
#define ILI9340_FRMCTR3    0xB3
#define ILI9340_INVTR      0xB4
#define ILI9340_DISCTRL    0xB6

#define ILI9340_PWCTRL1    0xC0
#define ILI9340_PWCTRL2    0xC1
#define ILI9340_PWCTRL3    0xC2
#define ILI9340_PWCTRL4    0xC3
#define ILI9340_PWCTRL5    0xC4
#define ILI9340_VMCTRL1    0xC5
#define ILI9340_VMCTRL2    0xC7

#define ILI9340_RDID1      0xDA
#define ILI9340_RDID2      0xDB
#define ILI9340_RDID3      0xDC
#define ILI9340_RDID4      0xDD

#define ILI9340_PGAMCTRL   0xE0
#define ILI9340_NGAMCTRL   0xE1

#define ILI9340_IFCTL      0xF6

#define ILI9340_MADCTL_MH  BIT(2)
#define ILI9340_MADCTL_BGR BIT(3)
#define ILI9340_MADCTL_ML  BIT(4)
#define ILI9340_MADCTL_MV  BIT(5)
#define ILI9340_MADCTL_MX  BIT(6)
#define ILI9340_MADCTL_MY  BIT(7)


#endif /* __LINUX_ILI9340_H */
