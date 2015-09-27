ccflags-y += -I$(src)/include

obj-$(CONFIG_DRM_TINYDRM)		+= core/

#obj-$(CONFIG_LCDREG)			+= lcdreg/
#obj-$(CONFIG_LCDCTRL)			+= lcdctrl/

#obj-$(CONFIG_TINYDRM_ADA_MIPI)		+= ada-mipifb.o
#obj-$(CONFIG_TINYDRM_ADA_SSD1306)	+= ada-ssd1306fb.o
#obj-$(CONFIG_TINYDRM_HY28)		+= hy28fb.o
