/* SPDX-License-Identifier: MIT
 * Copyright (C) 2018 Intel Corp.
 *
 * Authors:
 * Manasi Navare <manasi.d.navare@intel.com>
 */

#ifndef _BACKPORT_DRM_DSC_H_
#define _BACKPORT_DRM_DSC_H_

#ifdef BPM_DISPLAY_DRM_DSC_PRESENT
#include_next <drm/display/drm_dsc.h>
#include_next <drm/display/drm_dsc_helper.h>
#else
#include_next <drm/drm_dsc.h>
#endif

#ifndef DSC_1_2_MAX_LINEBUF_DEPTH_BITS
#define DSC_1_2_MAX_LINEBUF_DEPTH_BITS          16
#endif

#ifndef DSC_1_2_MAX_LINEBUF_DEPTH_VAL
#define DSC_1_2_MAX_LINEBUF_DEPTH_VAL           0
#endif

#ifndef DSC_1_1_MAX_LINEBUF_DEPTH_BITS
#define DSC_1_1_MAX_LINEBUF_DEPTH_BITS          13
#endif

#endif /* _BACKPORT_DRM_DSC_H_ */
