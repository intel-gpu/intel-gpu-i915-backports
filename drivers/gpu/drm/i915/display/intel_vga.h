/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_VGA_H__
#define __INTEL_VGA_H__

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
struct drm_i915_private;

void intel_vga_reset_io_mem(struct drm_i915_private *i915);
void intel_vga_disable(struct drm_i915_private *i915);
void intel_vga_redisable(struct drm_i915_private *i915);
void intel_vga_redisable_power_on(struct drm_i915_private *i915);
int intel_vga_register(struct drm_i915_private *i915);
void intel_vga_unregister(struct drm_i915_private *i915);
#endif /* CPTCFG_DRM_I915_DISPLAY */
#endif /* __INTEL_VGA_H__ */
