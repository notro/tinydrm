/*
 * ILI9340 LCD controller
 *
 * Copyright 2015 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __LINUX_ILI9340_H
#define __LINUX_ILI9340_H


#define ILI9340_NOP        0x00
#define ILI9340_SWRESET    0x01
#define ILI9340_RDDIDIF    0x04
#define ILI9340_RDDST      0x09
#define ILI9340_RDDPM      0x0A
#define ILI9340_RDDMADCTL  0x0B
#define ILI9340_RDDCOLMOD  0x0C
#define ILI9340_RDDIM      0x0D
#define ILI9340_RDDSM      0x0E
#define ILI9340_RDDSDR     0x0F

#define ILI9340_SLPIN      0x10
#define ILI9340_SLPOUT     0x11
#define ILI9340_PTLON      0x12
#define ILI9340_NORON      0x13

#define ILI9340_DINVOFF    0x20
#define ILI9340_DINVON     0x21
#define ILI9340_GAMSET     0x26
#define ILI9340_DISPOFF    0x28
#define ILI9340_DISPON     0x29
#define ILI9340_CASET      0x2A
#define ILI9340_PASET      0x2B
#define ILI9340_RAMWR      0x2C
#define ILI9340_RGBSET     0x2D
#define ILI9340_RAMRD      0x2E

#define ILI9340_PLTAR      0x30
#define ILI9340_VSCRDEF    0x33
#define ILI9340_TEOFF      0x34
#define ILI9340_TEON       0x35
#define ILI9340_MADCTL     0x36
#define ILI9340_VSCRSADD   0x37
#define ILI9340_IDMOFF     0x38
#define ILI9340_IDMON      0x39
#define ILI9340_PIXSET     0x3A

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

#define ILI9340_MADCTL_MV  BIT(5)
#define ILI9340_MADCTL_MX  BIT(6)
#define ILI9340_MADCTL_MY  BIT(7)


#endif /* __LINUX_ILI9340_H */
