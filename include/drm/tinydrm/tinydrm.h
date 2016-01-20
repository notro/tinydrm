


#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_panel.h>

struct tinydrm_framebuffer {
	struct drm_framebuffer base;
struct drm_gem_object *obj;
	struct drm_gem_cma_object *cma_obj;
};

struct tinydrm_device {
	struct drm_device *base;
	struct drm_panel panel;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_fbdev_cma *fbdev_cma;
	bool enabled;
	u32 width, height;
	void *dev_private;

	int (*dirty)(struct drm_framebuffer *fb,
		     struct drm_gem_cma_object *cma_obj,
		     unsigned flags, unsigned color,
		     struct drm_clip_rect *clips, unsigned num_clips);
};

int tinydrm_register(struct device *dev, struct tinydrm_device *tdev);
void tinydrm_release(struct tinydrm_device *tdev);
