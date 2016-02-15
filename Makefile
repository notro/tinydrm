ccflags-y += -I$(src)/include

obj-$(CONFIG_DRM_TINYDRM)		+= core/
obj-$(CONFIG_LCDREG)			+= lcdreg/

obj-$(CONFIG_TINYDRM_MIPI_DBI)		+= mipi-dbi.o

obj-$(CONFIG_TINYDRM_ADAFRUIT_TFT)	+= adafruit-tft.o
