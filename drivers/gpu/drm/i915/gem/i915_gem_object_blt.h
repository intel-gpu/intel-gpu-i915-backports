/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __I915_GEM_OBJECT_BLT_H__
#define __I915_GEM_OBJECT_BLT_H__

#include <linux/types.h>

#include "gt/intel_context.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_gpu_commands.h"
#include "i915_vma.h"

struct drm_i915_gem_object;
struct i915_gem_ww_ctx;

struct i915_vma *intel_emit_vma_fill_blt(struct intel_context *ce,
					 struct i915_vma *vma,
					 struct i915_gem_ww_ctx *ww,
					 u32 value);

struct i915_vma *intel_emit_vma_copy_blt(struct intel_context *ce,
					 struct i915_gem_ww_ctx *ww,
					 struct i915_vma *src,
					 struct i915_vma *dst);

int intel_emit_vma_mark_active(struct i915_vma *vma, struct i915_request *rq);
void intel_emit_vma_release(struct intel_context *ce, struct i915_vma *vma);

int i915_gem_object_fill_blt(struct drm_i915_gem_object *obj,
			     struct intel_context *ce,
			     u32 value);

int i915_gem_object_copy_blt(struct drm_i915_gem_object *src,
			     struct drm_i915_gem_object *dst,
			     struct intel_context *ce,
			     bool nowait);

int i915_gem_object_ww_fill_blt(struct drm_i915_gem_object *obj,
				struct i915_gem_ww_ctx *ww,
				struct intel_context *ce,
				u32 value);

int i915_gem_object_ww_copy_blt(struct drm_i915_gem_object *src,
				struct drm_i915_gem_object *dst,
				struct i915_gem_ww_ctx *ww,
				struct intel_context *ce,
				bool nowait);

int i915_gem_object_ww_compressed_copy_blt(struct drm_i915_gem_object *src,
				struct drm_i915_gem_object *dst,
				struct i915_gem_ww_ctx *ww,
				struct intel_context *ce,
				bool nowait);

static inline u32 *i915_flush_dw(u32 *cmd, struct i915_vma *dst, u32 flags)
{
	/* Mask the 3 LSB to use the PPGTT address space */
	GEM_BUG_ON(!IS_ALIGNED(i915_vma_offset(dst), 8));

	*cmd++ = MI_FLUSH_DW | flags;
	*cmd++ = lower_32_bits(i915_vma_offset(dst));
	*cmd++ = upper_32_bits(i915_vma_offset(dst));

	return cmd;
}

#endif
