/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2017 Google, Inc.
 *
 * Authors:
 * Sean Paul <seanpaul@chromium.org>
 */

#ifndef _BACKPORT_DRM_HDCP_H_
#define _BACKPORT_DRM_HDCP_H_

#ifdef BPM_DISPLAY_DRM_HDCP_PRESENT
#include_next <drm/display/drm_hdcp_helper.h>
#include_next <drm/display/drm_hdcp.h>
#else
#include_next <drm/drm_hdcp.h>
#endif

#endif /* _BACKPORT_DRM_HDCP_H_ */
