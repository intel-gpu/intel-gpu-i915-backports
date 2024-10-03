#ifndef _BACKPORT_DRM_FRAMEBUFFER_H_
#define _BACKPORT_DRM_FRAMEBUFFER_H_

#include_next <drm/drm_framebuffer.h>

#ifdef BPM_DRM_FRAMEBUFFER_PLANE_HEIGHT_NOT_PRESENT
static inline int drm_framebuffer_plane_height(int height,
						const struct drm_framebuffer *fb, int plane)
{
	return drm_format_info_plane_height(fb->format, height, plane);
}

static inline int drm_framebuffer_plane_width(int width,
						const struct drm_framebuffer *fb, int plane)
{
	return drm_format_info_plane_width(fb->format, width, plane );
}

#define fb_plane_height(height, info, i) \
		drm_format_info_plane_height(info, height, i)
#define fb_plane_width(width, info, i) \
		drm_format_info_plane_height(info, width, i)
#endif

#endif /*_BACKPORT_DRM_FRAMEBUFFER_H_*/
