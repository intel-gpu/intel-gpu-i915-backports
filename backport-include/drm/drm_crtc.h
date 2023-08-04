/*
 * Copyright © 2006 Keith Packard
 * Copyright © 2007-2008 Dave Airlie
 * Copyright © 2007-2008 Intel Corporation
 *   Jesse Barnes <jesse.barnes@intel.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef _BACKPORT_DRM_CRTC_H_
#define _BACKPORT_DRM_CRTC_H_

#include_next <drm/drm_crtc.h>
#ifdef BPM_DRM_FRAMEBUFFER_NOT_INCLUDED_IN_DRM_CRTC_H
#include <drm/drm_framebuffer.h>
#endif

#ifdef BPM_DRM_BLEND_H_NOT_INCLUDED_IN_DRM_CRTC_H
#include <drm/drm_blend.h>
#endif

#ifdef BPM_DRM_EDID_NOT_INCLUDED_IN_DRM_CRTC_H
#include <drm/drm_edid.h>
#endif

#ifdef BPM_BACKLIGHT_H_NOT_INCLUDED_IN_DRM_CRTC_H
#include <linux/backlight.h>
#endif

#endif /* _BACKPORT_DRM_CRTC_H_ */
