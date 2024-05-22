/*
 * Copyright © 2022 Intel Corporation
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

#ifndef _BACKPORT_DRM_DP_MST_HELPER_H_
#define _BACKPORT_DRM_DP_MST_HELPER_H_

#ifdef BPM_DRM_DP_HELPER_DIR_DISPLAY_PRESENT
#include_next <drm/display/drm_dp_mst_helper.h>
#include <drm/drm_dp_helper.h>

#ifdef BPM_DRM_DP_MST_PORT_VCPI_NOT_PRESENT
#define drm_dp_atomic_release_vcpi_slots drm_dp_atomic_release_time_slots
#endif

#elif defined(BPM_DRM_DP_HELPER_DIR_DP_PRESENT)
#include_next <drm/dp/drm_dp_mst_helper.h>
#else
#include_next <drm/drm_dp_mst_helper.h>
#endif

#ifdef BPM_DRM_DP_CALC_PBN_MODE_ARG_PRESENT
#define drm_dp_calc_pbn_mode(clock, bpp, dsc) drm_dp_calc_pbn_mode(clock,bpp)
#endif

#endif /* _BACKPORT_DRM_DP_MST_HELPER_H_ */
