ccflags-y += -I$(src)/include

obj-$(CONFIG_DRM_TINYDRM)		+= core/

obj-$(CONFIG_TINYDRM_FB_TFT)		+= fbtft/

# Controllers
obj-$(CONFIG_TINYDRM_MIPI_DBI)		+= mipi-dbi.o

# Displays
obj-$(CONFIG_TINYDRM_MI0283QT)		+= mi0283qt.o
obj-$(CONFIG_TINYDRM_ADAFRUIT_TFT)	+= adafruit-tft.o
