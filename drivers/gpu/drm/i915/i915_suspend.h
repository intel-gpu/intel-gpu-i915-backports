/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __I915_SUSPEND_H__
#define __I915_SUSPEND_H__

struct drm_i915_private;

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
void i915_save_display(struct drm_i915_private *i915);
void i915_restore_display(struct drm_i915_private *i915);
#else
static inline void i915_save_display(struct drm_i915_private *i915) { return; }
static inline void i915_restore_display(struct drm_i915_private *i915) { return; }
#endif

#endif /* __I915_SUSPEND_H__ */
