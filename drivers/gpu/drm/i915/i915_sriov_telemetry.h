/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __I915_SRIOV_TELEMETRY_H__
#define __I915_SRIOV_TELEMETRY_H__

#include <linux/types.h>

struct drm_i915_private;

bool i915_sriov_telemetry_is_enabled(struct drm_i915_private *i915);

void i915_sriov_telemetry_pf_init(struct drm_i915_private *i915);
void i915_sriov_telemetry_pf_release(struct drm_i915_private *i915);
int i915_sriov_telemetry_pf_process_data(struct drm_i915_private *i915, u32 vfid, u16 count,
					 const u32 *data, u32 len);
void i915_sriov_telemetry_pf_reset(struct drm_i915_private *i915, u32 vfid);

u64 i915_sriov_telemetry_pf_get_lmem_alloc_size(struct drm_i915_private *i915, u32 vfid);

void i915_sriov_telemetry_vf_init(struct drm_i915_private *i915);
void i915_sriov_telemetry_vf_fini(struct drm_i915_private *i915);
void i915_sriov_telemetry_vf_start(struct drm_i915_private *i915);
void i915_sriov_telemetry_vf_stop(struct drm_i915_private *i915);

#endif /* __I915_SRIOV_TELEMETRY_H__ */
