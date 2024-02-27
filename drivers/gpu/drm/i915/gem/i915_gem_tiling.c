/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2008 Intel Corporation
 */

#include <linux/errno.h>

#include "i915_gem_ioctls.h"

/**
 * i915_gem_set_tiling_ioctl - IOCTL handler to set tiling mode
 * @dev: DRM device
 * @data: data pointer for the ioctl
 * @file: DRM file for the ioctl call
 *
 * Sets the tiling mode of an object, returning the required swizzling of
 * bit 6 of addresses in the object.
 *
 * Called by the user via ioctl.
 *
 * Returns:
 * Zero on success, negative errno on failure.
 */
int
i915_gem_set_tiling_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file)
{
	return -EOPNOTSUPP;
}

/**
 * i915_gem_get_tiling_ioctl - IOCTL handler to get tiling mode
 * @dev: DRM device
 * @data: data pointer for the ioctl
 * @file: DRM file for the ioctl call
 *
 * Returns the current tiling mode and required bit 6 swizzling for the object.
 *
 * Called by the user via ioctl.
 *
 * Returns:
 * Zero on success, negative errno on failure.
 */
int
i915_gem_get_tiling_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file)
{
	return -EOPNOTSUPP;
}
