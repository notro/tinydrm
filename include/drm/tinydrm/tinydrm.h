


#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_panel.h>

struct lcdreg;

struct tinydrm_framebuffer {
	struct drm_framebuffer base;
struct drm_gem_object *obj;
	struct drm_gem_cma_object *cma_obj;
};

struct tinydrm_dirty {
	struct drm_framebuffer *fb;
	struct drm_gem_cma_object *cma_obj;
	struct delayed_work deferred_work;
	unsigned defer_ms;
	spinlock_t lock;
	struct drm_clip_rect clip;
};

struct tinydrm_device {
	struct drm_device *base;
	struct drm_panel panel;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_fbdev_cma *fbdev_cma;
	struct tinydrm_dirty dirty;
	struct lcdreg *lcdreg;
	struct backlight_device *backlight;
	bool prepared;
	bool enabled;
	u32 width, height;
	void *dev_private;

	int (*update)(struct tinydrm_device *tdev);
};

int devm_tinydrm_register(struct device *dev, struct tinydrm_device *tdev);
int tinydrm_register(struct device *dev, struct tinydrm_device *tdev);
void tinydrm_release(struct tinydrm_device *tdev);

static inline struct tinydrm_device *tinydrm_from_panel(struct drm_panel *panel)
{
	return panel->connector->dev->dev_private;
}

static inline bool tinydrm_is_full_clip(struct drm_clip_rect *clip, u32 width, u32 height)
{
	return clip->x1 == 0 && clip->x2 >= (width - 1) &&
	       clip->y1 == 0 && clip->y2 >= (height -1);
}

static inline void tinydrm_reset_clip(struct drm_clip_rect *clip)
{
	clip->x1 = ~0;
	clip->x2 = 0;
	clip->y1 = ~0;
	clip->y2 = 0;
}

void tinydrm_merge_clips(struct drm_clip_rect *dst, struct drm_clip_rect *clips, unsigned num_clips, unsigned flags, u32 width, u32 height);
struct backlight_device *tinydrm_of_find_backlight(struct device *dev);
