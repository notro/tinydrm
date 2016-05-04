ccflags-y += -I$(src)/../include

obj-$(CONFIG_DRM_TINYDRM)		+= tinydrm.o
tinydrm-y				+= tinydrm-core.o
tinydrm-y				+= tinydrm-display-pipe.o
tinydrm-y				+= tinydrm-framebuffer.o
tinydrm-y				+= tinydrm-helpers.o
tinydrm-$(CONFIG_DRM_KMS_FB_HELPER)	+= tinydrm-fbdev.o
tinydrm-$(CONFIG_DEBUG_FS)		+= tinydrm-debugfs.o
