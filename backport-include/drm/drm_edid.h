/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __BACKPORT_DRM_EDID_H
#define __BACKPORT_DRM_EDID_H

#ifdef BPM_DISPLAY_DRM_HDMI_HELPER_PRESENT
#include_next <drm/display/drm_hdmi_helper.h>
#endif

#ifdef BPM_DRM_ELD_H_PRESENT
#include <drm/drm_eld.h>
#endif
#include_next <drm/drm_edid.h>

#ifdef BPM_DRM_HDMI_AVI_INFOFRAME_COLORSPACE_NOT_PRESENT
#define drm_hdmi_avi_infoframe_colorspace drm_hdmi_avi_infoframe_colorimetry
#endif

#endif /* __BACKPORT_DRM_EDID_H */

