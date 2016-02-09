ccflags-y += -I$(src)/../include

obj-$(CONFIG_DRM_TINYDRM)		+= tinydrm.o
tinydrm-y				+= tinydrm-core.o tinydrm-deferred.o
tinydrm-$(CONFIG_DRM_KMS_FB_HELPER)	+= tinydrm-fbdev.o
