ccflags-y += -I$(src)/../include

tinydrm-y := tinydrm-core.o tinydrm-display-pipe.o tinydrm-framebuffer.o \
	     tinydrm-helpers.o

tinydrm-$(CONFIG_DEBUG_FS) += tinydrm-debugfs.o

obj-$(CONFIG_DRM_TINYDRM) += tinydrm.o
