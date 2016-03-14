ccflags-y += -I$(src)/../include

obj-$(CONFIG_DRM_TINYDRM)		+= tinydrm.o
tinydrm-y				+= tinydrm-core.o
tinydrm-y				+= tinydrm-crtc.o
tinydrm-y				+= tinydrm-framebuffer.o
tinydrm-y				+= tinydrm-plane.o
tinydrm-y				+= tinydrm-helpers.o
tinydrm-y				+= tinydrm-deferred.o
tinydrm-$(CONFIG_DRM_KMS_FB_HELPER)	+= tinydrm-fbdev.o
