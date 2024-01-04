/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef __INTEL_DPT_H__
#define __INTEL_DPT_H__

struct drm_i915_private;

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
struct i915_address_space;
struct i915_vma;
struct intel_framebuffer;

void intel_dpt_destroy(struct i915_address_space *vm);
struct i915_vma *intel_dpt_pin(struct i915_address_space *vm);
void intel_dpt_unpin(struct i915_address_space *vm);
void intel_dpt_suspend(struct drm_i915_private *i915);
void intel_dpt_resume(struct drm_i915_private *i915);
struct i915_address_space *
intel_dpt_create(struct intel_framebuffer *fb);
#else
static inline void intel_dpt_suspend(struct drm_i915_private *i915) { return; }
static inline void intel_dpt_resume(struct drm_i915_private *i915) { return; }
#endif /* CPTCFG_DRM_I915_DISPLAY */

#endif /* __INTEL_DPT_H__ */
