ccflags-y += -I$(src)/../include

obj-$(CONFIG_LCDREG)                   += lcdreg.o
lcdreg-y                               += lcdreg-core.o
lcdreg-$(CONFIG_DEBUG_FS)              += lcdreg-debugfs.o

obj-$(CONFIG_LCDREG_I2C)               += lcdreg-i2c.o
obj-$(CONFIG_LCDREG_SPI)               += lcdreg-spi.o
