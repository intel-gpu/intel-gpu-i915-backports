/* SPDX-License-Identifier: MIT */
/*
 * Function prototypes for misc. drm utility functions.
 * Specifically this file is for function prototypes for functions which
 * may also be used outside of drm code (e.g. in fbdev drivers).
 *
 * Copyright (C) 2017 Hans de Goede <hdegoede@redhat.com>
 */

#ifndef __DRM_UTILS_H__
#define __DRM_UTILS_H__

#include <linux/types.h>
#include <linux/osv_version.h>

#if SLES_RELEASE_CODE > SLES_RELEASE_VERSION(59,19)
#define drm_get_panel_orientation_quirk LINUX_I915_BACKPORT(drm_get_panel_orientation_quirk)
#endif

int drm_get_panel_orientation_quirk(int width, int height);

signed long drm_timeout_abs_to_jiffies(int64_t timeout_nsec);

#endif 
