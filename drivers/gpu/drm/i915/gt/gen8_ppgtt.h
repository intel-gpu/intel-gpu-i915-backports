/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2020 Intel Corporation
 */

#ifndef __GEN8_PPGTT_H__
#define __GEN8_PPGTT_H__

#include <linux/kernel.h>

enum i915_cache_level;

struct drm_i915_gem_object;
struct drm_mm_node;
struct i915_address_space;
struct intel_gt;

struct i915_ppgtt *gen8_ppgtt_create(struct intel_gt *gt, u32 flags);

int intel_flat_lmem_ppgtt_init(struct i915_address_space *vm,
			       struct drm_mm_node *node);
int intel_flat_lmem_ppgtt_insert_window(struct i915_address_space *vm,
					struct drm_i915_gem_object *obj,
					struct drm_mm_node *node,
					bool is_compact);
void intel_flat_lmem_ppgtt_fini(struct i915_address_space *vm,
				struct drm_mm_node *node);

int pvc_ppgtt_fault(struct i915_address_space *vm,
		    u64 start, u64 length, bool valid);

#endif
