/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2020 Intel Corporation
 */

#ifndef __INTEL_DRAM_H__
#define __INTEL_DRAM_H__

struct drm_i915_private;

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
void intel_dram_edram_detect(struct drm_i915_private *i915);
void intel_dram_detect(struct drm_i915_private *i915);
#else
void intel_dram_edram_detect(struct drm_i915_private *i915) { return; }
void intel_dram_detect(struct drm_i915_private *i915) { return; }
#endif
#endif /* __INTEL_DRAM_H__ */
