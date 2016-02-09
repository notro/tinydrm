#include <drm/tinydrm/tinydrm.h>

void tinydrm_deferred_begin(struct tinydrm_device *tdev,
			    struct tinydrm_fb_clip *fb_clip)
{
	struct tinydrm_deferred *deferred = tdev->deferred;

	spin_lock(&deferred->lock);
	*fb_clip = deferred->fb_clip;
	tinydrm_reset_clip(&deferred->fb_clip.clip);
	deferred->fb_clip.cma_obj = NULL;
	deferred->fb_clip.fb = NULL;
	spin_unlock(&deferred->lock);

	/* On first update make sure to do the entire framebuffer */
	if (!tdev->enabled) {
		fb_clip->clip.x1 = 0;
		fb_clip->clip.x2 = fb_clip->fb->width - 1;
		fb_clip->clip.y1 = 0;
		fb_clip->clip.y2 = fb_clip->fb->height - 1;
	}

	/* TODO: support partial updates */
	fb_clip->clip.x1 = 0;
	fb_clip->clip.x2 = fb_clip->fb->width - 1;
	fb_clip->clip.y1 = 0;
	fb_clip->clip.y2 = fb_clip->fb->height - 1;
}
EXPORT_SYMBOL(tinydrm_deferred_begin);

void tinydrm_deferred_end(struct tinydrm_device *tdev)
{
	if (tdev->prepared && !tdev->enabled) {
		drm_panel_enable(&tdev->panel);
		tdev->enabled = true;
	}
}
EXPORT_SYMBOL(tinydrm_deferred_end);

int tinydrm_fb_dirty(struct drm_framebuffer *fb,
		     struct drm_gem_cma_object *cma_obj, unsigned flags,
		     unsigned color, struct drm_clip_rect *clips,
		     unsigned num_clips)
{
	struct tinydrm_device *tdev = fb->dev->dev_private;

	struct tinydrm_deferred *deferred = tdev->deferred;
	struct tinydrm_fb_clip *fb_clip = &tdev->deferred->fb_clip;

	bool no_delay = deferred->no_delay;
	unsigned long delay;

	dev_dbg(tdev->base->dev, "%s(fb = %p, cma_obj = %p, clips = %p, num_clips = %u, no_delay = %u)\n", __func__, fb, cma_obj, clips, num_clips, no_delay);

	if (!cma_obj || !fb)
		return -EINVAL;

	spin_lock(&deferred->lock);
	fb_clip->fb = fb;
	fb_clip->cma_obj = cma_obj;
	tinydrm_merge_clips(&fb_clip->clip, clips, num_clips, flags,
			    fb->width, fb->height);
	if (tinydrm_is_full_clip(&fb_clip->clip, fb->width, fb->height))
		no_delay = true;
	spin_unlock(&deferred->lock);

	delay = no_delay ? 0 : msecs_to_jiffies(deferred->defer_ms);

	if (schedule_delayed_work(&deferred->dwork, delay))
		dev_dbg(tdev->base->dev, "%s: Already scheduled\n", __func__);

	return 0;
}
EXPORT_SYMBOL(tinydrm_fb_dirty);

void tinydrm_merge_clips(struct drm_clip_rect *dst,
			 struct drm_clip_rect *clips, unsigned num_clips,
			 unsigned flags, u32 width, u32 height)
{
	struct drm_clip_rect full_clip = {
		.x1 = 0,
		.x2 = width - 1,
		.y1 = 0,
		.y2 = height - 1,
	};
	int i;

	if (!clips) {
		clips = &full_clip;
		num_clips = 1;
	}

	for (i = 0; i < num_clips; i++) {
		dst->x1 = min(dst->x1, clips[i].x1);
		dst->x2 = max(dst->x2, clips[i].x2);
		dst->y1 = min(dst->y1, clips[i].y1);
		dst->y2 = max(dst->y2, clips[i].y2);

		if (flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY) {
			i++;
			dst->x2 = max(dst->x2, clips[i].x2);
			dst->y2 = max(dst->y2, clips[i].y2);
		}
	}

	dst->x2 = min_t(u32, dst->x2, width - 1);
	dst->y2 = min_t(u32, dst->y2, height - 1);
}
EXPORT_SYMBOL(tinydrm_merge_clips);
