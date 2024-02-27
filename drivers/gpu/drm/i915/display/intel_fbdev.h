/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_FBDEV_H__
#define __INTEL_FBDEV_H__

#include <linux/types.h>

struct drm_device;
#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
struct drm_i915_private;
struct intel_fbdev;
struct intel_framebuffer;

#ifdef CPTCFG_DRM_FBDEV_EMULATION
int intel_fbdev_init(struct drm_device *dev);
void intel_fbdev_initial_config_async(struct drm_device *dev);
void intel_fbdev_unregister(struct drm_i915_private *dev_priv);
void intel_fbdev_fini(struct drm_i915_private *dev_priv);
void intel_fbdev_set_suspend(struct drm_device *dev, int state, bool synchronous);
void intel_fbdev_output_poll_changed(struct drm_device *dev);
void intel_fbdev_restore_mode(struct drm_device *dev);
struct intel_framebuffer *intel_fbdev_framebuffer(struct intel_fbdev *fbdev);
#else
static inline int intel_fbdev_init(struct drm_device *dev)
{
	return 0;
}

static inline void intel_fbdev_initial_config_async(struct drm_device *dev)
{
}

static inline void intel_fbdev_unregister(struct drm_i915_private *dev_priv)
{
}

static inline void intel_fbdev_fini(struct drm_i915_private *dev_priv)
{
}

static inline void intel_fbdev_set_suspend(struct drm_device *dev, int state, bool synchronous)
{
}

static inline void intel_fbdev_output_poll_changed(struct drm_device *dev)
{
}

static inline void intel_fbdev_restore_mode(struct drm_device *dev)
{
}
static inline struct intel_framebuffer *intel_fbdev_framebuffer(struct intel_fbdev *fbdev)
{
	return NULL;
}
#endif

#else
static inline void intel_fbdev_restore_mode(struct drm_device *dev) { return; }
static inline void intel_fbdev_set_suspend(struct drm_device *dev,
					int state, bool synchronous) { return; }

#endif /* CPTCFG_DRM_I915_DISPLAY */
#endif /* __INTEL_FBDEV_H__ */
