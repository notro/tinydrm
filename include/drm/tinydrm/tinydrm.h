


#include <drm/drmP.h>
#include <drm/drm_crtc.h>

struct tinydrm_framebuffer {
	struct drm_framebuffer base;
struct drm_gem_object *obj;
	struct drm_gem_cma_object *cma_obj;
};

struct tinydrm_device {
	struct drm_device *base;
struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_fbdev_cma *fbdev_cma;
	bool enabled;
	u32 width, height;
	void *dev_private;

	int (*enable)(struct tinydrm_device *tdev);
	int (*disable)(struct tinydrm_device *tdev);

	int (*dirty)(struct drm_framebuffer *fb,
		     struct drm_gem_cma_object *cma_obj,
		     unsigned flags, unsigned color,
		     struct drm_clip_rect *clips, unsigned num_clips);

};
