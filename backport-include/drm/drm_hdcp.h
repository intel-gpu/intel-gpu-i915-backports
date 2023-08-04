/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BACKPORT_DRM_HDCP_H
#define _BACKPORT_DRM_HDCP_H

#ifdef BPM_HDCP_HELPERS_NOT_IN_DISPLAY_DIRECTORY
#include_next <drm/drm_hdcp.h>
#else
#include <drm/display/drm_hdcp.h>
#endif

#endif /* _BACKPORT_DRM_HDCP_H */
