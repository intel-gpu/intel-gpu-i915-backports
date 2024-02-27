/*
 * Copyright © 2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *   Jesse Barnes <jbarnes@virtuousgeek.org>
 *
 * New plane/sprite handling.
 *
 * The older chips had a separate interface for programming plane related
 * registers; newer ones are much simpler and we can use the new DRM plane
 * support.
 */

#include <linux/string_helpers.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_crtc.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_rect.h>

#include "i915_drv.h"
#include "intel_atomic_plane.h"
#include "intel_crtc.h"
#include "intel_de.h"
#include "intel_display_types.h"
#include "intel_fb.h"
#include "intel_frontbuffer.h"
#include "intel_sprite.h"
#include "intel_vrr.h"

int intel_plane_check_src_coordinates(struct intel_plane_state *plane_state)
{
	struct drm_i915_private *i915 = to_i915(plane_state->uapi.plane->dev);
	const struct drm_framebuffer *fb = plane_state->hw.fb;
	struct drm_rect *src = &plane_state->uapi.src;
	u32 src_x, src_y, src_w, src_h, hsub, vsub;
	bool rotated = drm_rotation_90_or_270(plane_state->hw.rotation);

	/*
	 * FIXME hsub/vsub vs. block size is a mess. Pre-tgl CCS
	 * abuses hsub/vsub so we can't use them here. But as they
	 * are limited to 32bpp RGB formats we don't actually need
	 * to check anything.
	 */
	if (fb->modifier == I915_FORMAT_MOD_Y_TILED_CCS ||
	    fb->modifier == I915_FORMAT_MOD_Yf_TILED_CCS)
		return 0;

	/*
	 * Hardware doesn't handle subpixel coordinates.
	 * Adjust to (macro)pixel boundary, but be careful not to
	 * increase the source viewport size, because that could
	 * push the downscaling factor out of bounds.
	 */
	src_x = src->x1 >> 16;
	src_w = drm_rect_width(src) >> 16;
	src_y = src->y1 >> 16;
	src_h = drm_rect_height(src) >> 16;

	drm_rect_init(src, src_x << 16, src_y << 16,
		      src_w << 16, src_h << 16);

	if (fb->format->format == DRM_FORMAT_RGB565 && rotated) {
		hsub = 2;
		vsub = 2;
	} else {
		hsub = fb->format->hsub;
		vsub = fb->format->vsub;
	}

	if (rotated)
		hsub = vsub = max(hsub, vsub);

	if (src_x % hsub || src_w % hsub) {
		drm_dbg_kms(&i915->drm, "src x/w (%u, %u) must be a multiple of %u (rotated: %s)\n",
			    src_x, src_w, hsub, str_yes_no(rotated));
		return -EINVAL;
	}

	if (src_y % vsub || src_h % vsub) {
		drm_dbg_kms(&i915->drm, "src y/h (%u, %u) must be a multiple of %u (rotated: %s)\n",
			    src_y, src_h, vsub, str_yes_no(rotated));
		return -EINVAL;
	}

	return 0;
}

static void ivb_plane_ratio(const struct intel_crtc_state *crtc_state,
			    const struct intel_plane_state *plane_state,
			    unsigned int *num, unsigned int *den)
{
	u8 active_planes = crtc_state->active_planes & ~BIT(PLANE_CURSOR);
	const struct drm_framebuffer *fb = plane_state->hw.fb;
	unsigned int cpp = fb->format->cpp[0];

	if (hweight8(active_planes) == 2) {
		switch (cpp) {
		case 8:
			*num = 10;
			*den = 8;
			break;
		case 4:
			*num = 17;
			*den = 16;
			break;
		default:
			*num = 1;
			*den = 1;
			break;
		}
	} else {
		switch (cpp) {
		case 8:
			*num = 9;
			*den = 8;
			break;
		default:
			*num = 1;
			*den = 1;
			break;
		}
	}
}

static void ivb_plane_ratio_scaling(const struct intel_crtc_state *crtc_state,
				    const struct intel_plane_state *plane_state,
				    unsigned int *num, unsigned int *den)
{
	const struct drm_framebuffer *fb = plane_state->hw.fb;
	unsigned int cpp = fb->format->cpp[0];

	switch (cpp) {
	case 8:
		*num = 12;
		*den = 8;
		break;
	case 4:
		*num = 19;
		*den = 16;
		break;
	case 2:
		*num = 33;
		*den = 32;
		break;
	default:
		*num = 1;
		*den = 1;
		break;
	}
}

int ivb_plane_min_cdclk(const struct intel_crtc_state *crtc_state,
			const struct intel_plane_state *plane_state)
{
	unsigned int pixel_rate;
	unsigned int num, den;

	/*
	 * Note that crtc_state->pixel_rate accounts for both
	 * horizontal and vertical panel fitter downscaling factors.
	 * Pre-HSW bspec tells us to only consider the horizontal
	 * downscaling factor here. We ignore that and just consider
	 * both for simplicity.
	 */
	pixel_rate = crtc_state->pixel_rate;

	ivb_plane_ratio(crtc_state, plane_state, &num, &den);

	return DIV_ROUND_UP(pixel_rate * num, den);
}

static int ivb_sprite_min_cdclk(const struct intel_crtc_state *crtc_state,
				const struct intel_plane_state *plane_state)
{
	unsigned int src_w, dst_w, pixel_rate;
	unsigned int num, den;

	/*
	 * Note that crtc_state->pixel_rate accounts for both
	 * horizontal and vertical panel fitter downscaling factors.
	 * Pre-HSW bspec tells us to only consider the horizontal
	 * downscaling factor here. We ignore that and just consider
	 * both for simplicity.
	 */
	pixel_rate = crtc_state->pixel_rate;

	src_w = drm_rect_width(&plane_state->uapi.src) >> 16;
	dst_w = drm_rect_width(&plane_state->uapi.dst);

	if (src_w != dst_w)
		ivb_plane_ratio_scaling(crtc_state, plane_state, &num, &den);
	else
		ivb_plane_ratio(crtc_state, plane_state, &num, &den);

	/* Horizontal downscaling limits the maximum pixel rate */
	dst_w = min(src_w, dst_w);

	return DIV_ROUND_UP_ULL(mul_u32_u32(pixel_rate, num * src_w),
				den * dst_w);
}

static void hsw_plane_ratio(const struct intel_crtc_state *crtc_state,
			    const struct intel_plane_state *plane_state,
			    unsigned int *num, unsigned int *den)
{
	u8 active_planes = crtc_state->active_planes & ~BIT(PLANE_CURSOR);
	const struct drm_framebuffer *fb = plane_state->hw.fb;
	unsigned int cpp = fb->format->cpp[0];

	if (hweight8(active_planes) == 2) {
		switch (cpp) {
		case 8:
			*num = 10;
			*den = 8;
			break;
		default:
			*num = 1;
			*den = 1;
			break;
		}
	} else {
		switch (cpp) {
		case 8:
			*num = 9;
			*den = 8;
			break;
		default:
			*num = 1;
			*den = 1;
			break;
		}
	}
}

int hsw_plane_min_cdclk(const struct intel_crtc_state *crtc_state,
			const struct intel_plane_state *plane_state)
{
	unsigned int pixel_rate = crtc_state->pixel_rate;
	unsigned int num, den;

	hsw_plane_ratio(crtc_state, plane_state, &num, &den);

	return DIV_ROUND_UP(pixel_rate * num, den);
}

static u32 ivb_sprite_ctl_crtc(const struct intel_crtc_state *crtc_state)
{
	u32 sprctl = 0;

	if (crtc_state->gamma_enable)
		sprctl |= SPRITE_PIPE_GAMMA_ENABLE;

	if (crtc_state->csc_enable)
		sprctl |= SPRITE_PIPE_CSC_ENABLE;

	return sprctl;
}

static bool ivb_need_sprite_gamma(const struct intel_plane_state *plane_state)
{
	return false;
}

static u32 ivb_sprite_ctl(const struct intel_crtc_state *crtc_state,
			  const struct intel_plane_state *plane_state)
{
	const struct drm_framebuffer *fb = plane_state->hw.fb;
	unsigned int rotation = plane_state->hw.rotation;
	const struct drm_intel_sprite_colorkey *key = &plane_state->ckey;
	u32 sprctl;

	sprctl = SPRITE_ENABLE;

	switch (fb->format->format) {
	case DRM_FORMAT_XBGR8888:
		sprctl |= SPRITE_FORMAT_RGBX888 | SPRITE_RGB_ORDER_RGBX;
		break;
	case DRM_FORMAT_XRGB8888:
		sprctl |= SPRITE_FORMAT_RGBX888;
		break;
	case DRM_FORMAT_XBGR2101010:
		sprctl |= SPRITE_FORMAT_RGBX101010 | SPRITE_RGB_ORDER_RGBX;
		break;
	case DRM_FORMAT_XRGB2101010:
		sprctl |= SPRITE_FORMAT_RGBX101010;
		break;
	case DRM_FORMAT_XBGR16161616F:
		sprctl |= SPRITE_FORMAT_RGBX161616 | SPRITE_RGB_ORDER_RGBX;
		break;
	case DRM_FORMAT_XRGB16161616F:
		sprctl |= SPRITE_FORMAT_RGBX161616;
		break;
	case DRM_FORMAT_YUYV:
		sprctl |= SPRITE_FORMAT_YUV422 | SPRITE_YUV_ORDER_YUYV;
		break;
	case DRM_FORMAT_YVYU:
		sprctl |= SPRITE_FORMAT_YUV422 | SPRITE_YUV_ORDER_YVYU;
		break;
	case DRM_FORMAT_UYVY:
		sprctl |= SPRITE_FORMAT_YUV422 | SPRITE_YUV_ORDER_UYVY;
		break;
	case DRM_FORMAT_VYUY:
		sprctl |= SPRITE_FORMAT_YUV422 | SPRITE_YUV_ORDER_VYUY;
		break;
	default:
		MISSING_CASE(fb->format->format);
		return 0;
	}

	if (!ivb_need_sprite_gamma(plane_state))
		sprctl |= SPRITE_PLANE_GAMMA_DISABLE;

	if (plane_state->hw.color_encoding == DRM_COLOR_YCBCR_BT709)
		sprctl |= SPRITE_YUV_TO_RGB_CSC_FORMAT_BT709;

	if (plane_state->hw.color_range == DRM_COLOR_YCBCR_FULL_RANGE)
		sprctl |= SPRITE_YUV_RANGE_CORRECTION_DISABLE;

	if (fb->modifier == I915_FORMAT_MOD_X_TILED)
		sprctl |= SPRITE_TILED;

	if (rotation & DRM_MODE_ROTATE_180)
		sprctl |= SPRITE_ROTATE_180;

	if (key->flags & I915_SET_COLORKEY_DESTINATION)
		sprctl |= SPRITE_DEST_KEY;
	else if (key->flags & I915_SET_COLORKEY_SOURCE)
		sprctl |= SPRITE_SOURCE_KEY;

	return sprctl;
}

static void ivb_sprite_linear_gamma(const struct intel_plane_state *plane_state,
				    u16 gamma[18])
{
	int scale, i;

	/*
	 * WaFP16GammaEnabling:ivb,hsw
	 * "Workaround : When using the 64-bit format, the sprite output
	 *  on each color channel has one quarter amplitude. It can be
	 *  brought up to full amplitude by using sprite internal gamma
	 *  correction, pipe gamma correction, or pipe color space
	 *  conversion to multiply the sprite output by four."
	 */
	scale = 4;

	for (i = 0; i < 16; i++)
		gamma[i] = min((scale * i << 10) / 16, (1 << 10) - 1);

	gamma[i] = min((scale * i << 10) / 16, 1 << 10);
	i++;

	gamma[i] = 3 << 10;
	i++;
}

static void ivb_sprite_update_gamma(const struct intel_plane_state *plane_state)
{
	struct intel_plane *plane = to_intel_plane(plane_state->uapi.plane);
	struct drm_i915_private *dev_priv = to_i915(plane->base.dev);
	enum pipe pipe = plane->pipe;
	u16 gamma[18];
	int i;

	if (!ivb_need_sprite_gamma(plane_state))
		return;

	ivb_sprite_linear_gamma(plane_state, gamma);

	/* FIXME these register are single buffered :( */
	for (i = 0; i < 16; i++)
		intel_de_write_fw(dev_priv, SPRGAMC(pipe, i),
				  gamma[i] << 20 | gamma[i] << 10 | gamma[i]);

	intel_de_write_fw(dev_priv, SPRGAMC16(pipe, 0), gamma[i]);
	intel_de_write_fw(dev_priv, SPRGAMC16(pipe, 1), gamma[i]);
	intel_de_write_fw(dev_priv, SPRGAMC16(pipe, 2), gamma[i]);
	i++;

	intel_de_write_fw(dev_priv, SPRGAMC17(pipe, 0), gamma[i]);
	intel_de_write_fw(dev_priv, SPRGAMC17(pipe, 1), gamma[i]);
	intel_de_write_fw(dev_priv, SPRGAMC17(pipe, 2), gamma[i]);
	i++;
}

static void
ivb_sprite_update_noarm(struct intel_plane *plane,
			const struct intel_crtc_state *crtc_state,
			const struct intel_plane_state *plane_state)
{
	struct drm_i915_private *dev_priv = to_i915(plane->base.dev);
	enum pipe pipe = plane->pipe;
	int crtc_x = plane_state->uapi.dst.x1;
	int crtc_y = plane_state->uapi.dst.y1;
	u32 crtc_w = drm_rect_width(&plane_state->uapi.dst);
	u32 crtc_h = drm_rect_height(&plane_state->uapi.dst);
	u32 src_w = drm_rect_width(&plane_state->uapi.src) >> 16;
	u32 src_h = drm_rect_height(&plane_state->uapi.src) >> 16;
	u32 sprscale = 0;

	if (crtc_w != src_w || crtc_h != src_h)
		sprscale = SPRITE_SCALE_ENABLE |
			SPRITE_SRC_WIDTH(src_w - 1) |
			SPRITE_SRC_HEIGHT(src_h - 1);

	intel_de_write_fw(dev_priv, SPRSTRIDE(pipe),
			  plane_state->view.color_plane[0].mapping_stride);
	intel_de_write_fw(dev_priv, SPRPOS(pipe),
			  SPRITE_POS_Y(crtc_y) | SPRITE_POS_X(crtc_x));
	intel_de_write_fw(dev_priv, SPRSIZE(pipe),
			  SPRITE_HEIGHT(crtc_h - 1) | SPRITE_WIDTH(crtc_w - 1));
}

static void
ivb_sprite_update_arm(struct intel_plane *plane,
		      const struct intel_crtc_state *crtc_state,
		      const struct intel_plane_state *plane_state)
{
	struct drm_i915_private *dev_priv = to_i915(plane->base.dev);
	enum pipe pipe = plane->pipe;
	const struct drm_intel_sprite_colorkey *key = &plane_state->ckey;
	u32 sprsurf_offset = plane_state->view.color_plane[0].offset;
	u32 x = plane_state->view.color_plane[0].x;
	u32 y = plane_state->view.color_plane[0].y;
	u32 sprctl, linear_offset;

	sprctl = plane_state->ctl | ivb_sprite_ctl_crtc(crtc_state);

	linear_offset = intel_fb_xy_to_linear(x, y, plane_state, 0);

	if (key->flags) {
		intel_de_write_fw(dev_priv, SPRKEYVAL(pipe), key->min_value);
		intel_de_write_fw(dev_priv, SPRKEYMSK(pipe),
				  key->channel_mask);
		intel_de_write_fw(dev_priv, SPRKEYMAX(pipe), key->max_value);
	}

	intel_de_write_fw(dev_priv, SPRLINOFF(pipe), linear_offset);
	intel_de_write_fw(dev_priv, SPRTILEOFF(pipe),
			  SPRITE_OFFSET_Y(y) | SPRITE_OFFSET_X(x));

	/*
	 * The control register self-arms if the plane was previously
	 * disabled. Try to make the plane enable atomic by writing
	 * the control register just before the surface register.
	 */
	intel_de_write_fw(dev_priv, SPRCTL(pipe), sprctl);
	intel_de_write_fw(dev_priv, SPRSURF(pipe),
			  intel_plane_ggtt_offset(plane_state) + sprsurf_offset);

	ivb_sprite_update_gamma(plane_state);
}

static void
ivb_sprite_disable_arm(struct intel_plane *plane,
		       const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(plane->base.dev);
	enum pipe pipe = plane->pipe;

	intel_de_write_fw(dev_priv, SPRCTL(pipe), 0);
	intel_de_write_fw(dev_priv, SPRSURF(pipe), 0);
}

static bool
ivb_sprite_get_hw_state(struct intel_plane *plane,
			enum pipe *pipe)
{
	struct drm_i915_private *dev_priv = to_i915(plane->base.dev);
	enum intel_display_power_domain power_domain;
	intel_wakeref_t wakeref;
	bool ret;

	power_domain = POWER_DOMAIN_PIPE(plane->pipe);
	wakeref = intel_display_power_get_if_enabled(dev_priv, power_domain);
	if (!wakeref)
		return false;

	ret =  intel_de_read(dev_priv, SPRCTL(plane->pipe)) & SPRITE_ENABLE;

	*pipe = plane->pipe;

	intel_display_power_put(dev_priv, power_domain, wakeref);

	return ret;
}

static unsigned int
g4x_sprite_max_stride(struct intel_plane *plane,
		      u32 pixel_format, u64 modifier,
		      unsigned int rotation)
{
	const struct drm_format_info *info = drm_format_info(pixel_format);
	int cpp = info->cpp[0];

	/* Limit to 4k pixels to guarantee TILEOFF.x doesn't get too big. */
	if (modifier == I915_FORMAT_MOD_X_TILED)
		return min(4096 * cpp, 16 * 1024);
	else
		return 16 * 1024;
}

static int
g4x_sprite_check_scaling(struct intel_crtc_state *crtc_state,
			 struct intel_plane_state *plane_state)
{
	struct drm_i915_private *i915 = to_i915(plane_state->uapi.plane->dev);
	const struct drm_framebuffer *fb = plane_state->hw.fb;
	const struct drm_rect *src = &plane_state->uapi.src;
	const struct drm_rect *dst = &plane_state->uapi.dst;
	int src_x, src_w, src_h, crtc_w, crtc_h;
	const struct drm_display_mode *adjusted_mode =
		&crtc_state->hw.adjusted_mode;
	unsigned int stride = plane_state->view.color_plane[0].mapping_stride;
	unsigned int cpp = fb->format->cpp[0];
	unsigned int width_bytes;
	int min_width, min_height;

	crtc_w = drm_rect_width(dst);
	crtc_h = drm_rect_height(dst);

	src_x = src->x1 >> 16;
	src_w = drm_rect_width(src) >> 16;
	src_h = drm_rect_height(src) >> 16;

	if (src_w == crtc_w && src_h == crtc_h)
		return 0;

	min_width = 3;

	if (adjusted_mode->flags & DRM_MODE_FLAG_INTERLACE) {
		if (src_h & 1) {
			drm_dbg_kms(&i915->drm, "Source height must be even with interlaced modes\n");
			return -EINVAL;
		}
		min_height = 6;
	} else {
		min_height = 3;
	}

	width_bytes = ((src_x * cpp) & 63) + src_w * cpp;

	if (src_w < min_width || src_h < min_height ||
	    src_w > 2048 || src_h > 2048) {
		drm_dbg_kms(&i915->drm, "Source dimensions (%dx%d) exceed hardware limits (%dx%d - %dx%d)\n",
			    src_w, src_h, min_width, min_height, 2048, 2048);
		return -EINVAL;
	}

	if (width_bytes > 4096) {
		drm_dbg_kms(&i915->drm, "Fetch width (%d) exceeds hardware max with scaling (%u)\n",
			    width_bytes, 4096);
		return -EINVAL;
	}

	if (stride > 4096) {
		drm_dbg_kms(&i915->drm, "Stride (%u) exceeds hardware max with scaling (%u)\n",
			    stride, 4096);
		return -EINVAL;
	}

	return 0;
}

static int i9xx_check_plane_surface(struct intel_plane_state *plane_state)
{
	struct drm_i915_private *dev_priv =
		to_i915(plane_state->uapi.plane->dev);
	const struct drm_framebuffer *fb = plane_state->hw.fb;
	int src_x, src_y, src_w;
	u32 offset;
	int ret;

	ret = intel_plane_compute_gtt(plane_state);
	if (ret)
		return ret;

	if (!plane_state->uapi.visible)
		return 0;

	src_w = drm_rect_width(&plane_state->uapi.src) >> 16;
	src_x = plane_state->uapi.src.x1 >> 16;
	src_y = plane_state->uapi.src.y1 >> 16;

	offset = intel_plane_compute_aligned_offset(&src_x, &src_y,
						    plane_state, 0);
	/*
	 * When using an X-tiled surface the plane starts to
	 * misbehave if the x offset + width exceeds the stride.
	 * hsw/bdw: underrun galore
	 * ilk/snb/ivb: wrap to the next tile row mid scanout
	 * i965/g4x: so far appear immune to this
	 * vlv/chv: TODO check
	 *
	 * Linear surfaces seem to work just fine, even on hsw/bdw
	 * despite them not using the linear offset anymore.
	 */
	if (fb->modifier == I915_FORMAT_MOD_X_TILED) {
		u32 alignment = intel_surf_alignment(fb, 0);
		int cpp = fb->format->cpp[0];

		while ((src_x + src_w) * cpp > plane_state->view.color_plane[0].mapping_stride) {
			if (offset == 0) {
				drm_dbg_kms(&dev_priv->drm,
					    "Unable to find suitable display surface offset due to X-tiling\n");
				return -EINVAL;
			}

			offset = intel_plane_adjust_aligned_offset(&src_x, &src_y, plane_state, 0,
								   offset, offset - alignment);
		}
	}

	/*
	 * Put the final coordinates back so that the src
	 * coordinate checks will see the right values.
	 */
	drm_rect_translate_to(&plane_state->uapi.src,
			      src_x << 16, src_y << 16);

	/* HSW/BDW do this automagically in hardware */
	if (1) {
		unsigned int rotation = plane_state->hw.rotation;
		int src_w = drm_rect_width(&plane_state->uapi.src) >> 16;
		int src_h = drm_rect_height(&plane_state->uapi.src) >> 16;

		if (rotation & DRM_MODE_ROTATE_180) {
			src_x += src_w - 1;
			src_y += src_h - 1;
		} else if (rotation & DRM_MODE_REFLECT_X) {
			src_x += src_w - 1;
		}
	}

	plane_state->view.color_plane[0].offset = offset;
	plane_state->view.color_plane[0].x = src_x;
	plane_state->view.color_plane[0].y = src_y;

	return 0;
}

static int
g4x_sprite_check(struct intel_crtc_state *crtc_state,
		 struct intel_plane_state *plane_state)
{
	int min_scale = DRM_PLANE_HELPER_NO_SCALING;
	int max_scale = DRM_PLANE_HELPER_NO_SCALING;
	int ret;

	ret = intel_atomic_plane_check_clipping(plane_state, crtc_state,
						min_scale, max_scale, true);
	if (ret)
		return ret;

	ret = i9xx_check_plane_surface(plane_state);
	if (ret)
		return ret;

	if (!plane_state->uapi.visible)
		return 0;

	ret = intel_plane_check_src_coordinates(plane_state);
	if (ret)
		return ret;

	ret = g4x_sprite_check_scaling(crtc_state, plane_state);
	if (ret)
		return ret;

	plane_state->ctl = ivb_sprite_ctl(crtc_state, plane_state);

	return 0;
}

int chv_plane_check_rotation(const struct intel_plane_state *plane_state)
{
	return 0;
}

static bool has_dst_key_in_primary_plane(struct drm_i915_private *dev_priv)
{
	return true;
}

static void intel_plane_set_ckey(struct intel_plane_state *plane_state,
				 const struct drm_intel_sprite_colorkey *set)
{
	struct intel_plane *plane = to_intel_plane(plane_state->uapi.plane);
	struct drm_intel_sprite_colorkey *key = &plane_state->ckey;

	*key = *set;

	/*
	 * We want src key enabled on the
	 * sprite and not on the primary.
	 */
	if (plane->id == PLANE_PRIMARY &&
	    set->flags & I915_SET_COLORKEY_SOURCE)
		key->flags = 0;

	/*
	 * On SKL+ we want dst key enabled on
	 * the primary and not on the sprite.
	 */
	if (plane->id != PLANE_PRIMARY &&
	    set->flags & I915_SET_COLORKEY_DESTINATION)
		key->flags = 0;
}

int intel_sprite_set_colorkey_ioctl(struct drm_device *dev, void *data,
				    struct drm_file *file_priv)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct drm_intel_sprite_colorkey *set = data;
	struct drm_plane *plane;
	struct drm_plane_state *plane_state;
	struct drm_atomic_state *state;
	struct drm_modeset_acquire_ctx ctx;
	int ret = 0;

	/* ignore the pointless "none" flag */
	set->flags &= ~I915_SET_COLORKEY_NONE;

	if (set->flags & ~(I915_SET_COLORKEY_DESTINATION | I915_SET_COLORKEY_SOURCE))
		return -EINVAL;

	/* Make sure we don't try to enable both src & dest simultaneously */
	if ((set->flags & (I915_SET_COLORKEY_DESTINATION | I915_SET_COLORKEY_SOURCE)) == (I915_SET_COLORKEY_DESTINATION | I915_SET_COLORKEY_SOURCE))
		return -EINVAL;

	plane = drm_plane_find(dev, file_priv, set->plane_id);
	if (!plane || plane->type != DRM_PLANE_TYPE_OVERLAY)
		return -ENOENT;

	/*
	 * SKL+ only plane 2 can do destination keying against plane 1.
	 * Also multiple planes can't do destination keying on the same
	 * pipe simultaneously.
	 */
	if (to_intel_plane(plane)->id >= PLANE_SPRITE1 &&
	    set->flags & I915_SET_COLORKEY_DESTINATION)
		return -EINVAL;

	drm_modeset_acquire_init(&ctx, 0);

	state = drm_atomic_state_alloc(plane->dev);
	if (!state) {
		ret = -ENOMEM;
		goto out;
	}
	state->acquire_ctx = &ctx;

	while (1) {
		plane_state = drm_atomic_get_plane_state(state, plane);
		ret = PTR_ERR_OR_ZERO(plane_state);
		if (!ret)
			intel_plane_set_ckey(to_intel_plane_state(plane_state), set);

		/*
		 * On some platforms we have to configure
		 * the dst colorkey on the primary plane.
		 */
		if (!ret && has_dst_key_in_primary_plane(dev_priv)) {
			struct intel_crtc *crtc =
				intel_crtc_for_pipe(dev_priv,
						    to_intel_plane(plane)->pipe);

			plane_state = drm_atomic_get_plane_state(state,
								 crtc->base.primary);
			ret = PTR_ERR_OR_ZERO(plane_state);
			if (!ret)
				intel_plane_set_ckey(to_intel_plane_state(plane_state), set);
		}

		if (!ret)
			ret = drm_atomic_commit(state);

		if (ret != -EDEADLK)
			break;

		drm_atomic_state_clear(state);
		drm_modeset_backoff(&ctx);
	}

	drm_atomic_state_put(state);
out:
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	return ret;
}

static const u32 snb_sprite_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_XRGB2101010,
	DRM_FORMAT_XBGR2101010,
	DRM_FORMAT_XRGB16161616F,
	DRM_FORMAT_XBGR16161616F,
	DRM_FORMAT_YUYV,
	DRM_FORMAT_YVYU,
	DRM_FORMAT_UYVY,
	DRM_FORMAT_VYUY,
};

static bool snb_sprite_format_mod_supported(struct drm_plane *_plane,
					    u32 format, u64 modifier)
{
	if (!intel_fb_plane_supports_modifier(to_intel_plane(_plane), modifier))
		return false;

	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_XRGB16161616F:
	case DRM_FORMAT_XBGR16161616F:
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_YVYU:
	case DRM_FORMAT_UYVY:
	case DRM_FORMAT_VYUY:
		if (modifier == DRM_FORMAT_MOD_LINEAR ||
		    modifier == I915_FORMAT_MOD_X_TILED)
			return true;
		fallthrough;
	default:
		return false;
	}
}

static const struct drm_plane_funcs snb_sprite_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = intel_plane_destroy,
	.atomic_duplicate_state = intel_plane_duplicate_state,
	.atomic_destroy_state = intel_plane_destroy_state,
	.format_mod_supported = snb_sprite_format_mod_supported,
};

struct intel_plane *
intel_sprite_plane_create(struct drm_i915_private *dev_priv,
			  enum pipe pipe, int sprite)
{
	struct intel_plane *plane;
	const struct drm_plane_funcs *plane_funcs;
	unsigned int supported_rotations;
	const u64 *modifiers;
	const u32 *formats;
	int num_formats;
	int ret, zpos;

	plane = intel_plane_alloc();
	if (IS_ERR(plane))
		return plane;

	plane->update_noarm = ivb_sprite_update_noarm;
	plane->update_arm = ivb_sprite_update_arm;
	plane->disable_arm = ivb_sprite_disable_arm;
	plane->get_hw_state = ivb_sprite_get_hw_state;
	plane->check_plane = g4x_sprite_check;

	plane->max_stride = g4x_sprite_max_stride;
	plane->min_cdclk = ivb_sprite_min_cdclk;

	formats = snb_sprite_formats;
	num_formats = ARRAY_SIZE(snb_sprite_formats);

	plane_funcs = &snb_sprite_funcs;
	supported_rotations = DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_180;

	plane->pipe = pipe;
	plane->id = PLANE_SPRITE0 + sprite;
	plane->frontbuffer_bit = INTEL_FRONTBUFFER(pipe, plane->id);

	modifiers = intel_fb_plane_get_modifiers(dev_priv, INTEL_PLANE_CAP_TILING_X);

	ret = drm_universal_plane_init(&dev_priv->drm, &plane->base,
				       0, plane_funcs,
				       formats, num_formats, modifiers,
				       DRM_PLANE_TYPE_OVERLAY,
				       "sprite %c", sprite_name(pipe, sprite));
	kfree(modifiers);

	if (ret)
		goto fail;

	drm_plane_create_rotation_property(&plane->base,
					   DRM_MODE_ROTATE_0,
					   supported_rotations);

	drm_plane_create_color_properties(&plane->base,
					  BIT(DRM_COLOR_YCBCR_BT601) |
					  BIT(DRM_COLOR_YCBCR_BT709),
					  BIT(DRM_COLOR_YCBCR_LIMITED_RANGE) |
					  BIT(DRM_COLOR_YCBCR_FULL_RANGE),
					  DRM_COLOR_YCBCR_BT709,
					  DRM_COLOR_YCBCR_LIMITED_RANGE);

	zpos = sprite + 1;
	drm_plane_create_zpos_immutable_property(&plane->base, zpos);

	intel_plane_helper_add(plane);

	return plane;

fail:
	intel_plane_free(plane);

	return ERR_PTR(ret);
}
