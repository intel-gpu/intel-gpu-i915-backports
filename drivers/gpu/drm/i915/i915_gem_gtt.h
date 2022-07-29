/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2020 Intel Corporation
 */

#ifndef __I915_GEM_GTT_H__
#define __I915_GEM_GTT_H__

#include <linux/io-mapping.h>
#include <linux/types.h>

#include <drm/drm_mm.h>

#include "gt/intel_gtt.h"
#include "i915_scatterlist.h"

struct drm_i915_gem_object;
struct i915_address_space;

int __must_check i915_gem_gtt_prepare_pages(struct drm_i915_gem_object *obj,
					    struct sg_table *pages);
void i915_gem_gtt_finish_pages(struct drm_i915_gem_object *obj,
			       struct sg_table *pages);

int i915_gem_gtt_reserve(struct i915_address_space *vm,
			 struct drm_mm_node *node,
			 u64 size, u64 offset, unsigned long color,
			 unsigned int flags);

int i915_gem_gtt_insert(struct i915_address_space *vm,
			struct drm_mm_node *node,
			u64 size, u64 alignment, unsigned long color,
			u64 start, u64 end, unsigned int flags);

struct drm_mm_node *i915_gem_gtt_lookup(struct i915_address_space *vm,
					u64 addr);

/* Flags used by pin/bind&friends. */
#define PIN_NOEVICT		BIT_ULL(0)
#define PIN_NOSEARCH		BIT_ULL(1)
#define PIN_RESIDENT		BIT_ULL(1) /* Cannot exist together with PIN_NOSEARCH */
#define PIN_NONBLOCK		BIT_ULL(2)
#define PIN_MAPPABLE		BIT_ULL(3)
#define PIN_ZONE_32		BIT_ULL(4)
#define PIN_ZONE_48		BIT_ULL(5)
#define PIN_HIGH		BIT_ULL(6)
#define PIN_OFFSET_BIAS		BIT_ULL(7)
#define PIN_OFFSET_FIXED	BIT_ULL(8)
#define PIN_OFFSET_GUARD	BIT_ULL(9)

#define PIN_GLOBAL		BIT_ULL(10) /* I915_VMA_GLOBAL_BIND */
#define PIN_USER		BIT_ULL(11) /* I915_VMA_LOCAL_BIND */

#define PIN_OFFSET_MASK		I915_GTT_PAGE_MASK

static inline int i915_vm_move_to_active(struct i915_address_space *vm,
					 struct intel_context *ce,
					 struct i915_request *rq)
{
	if (i915_vm_page_fault_enabled(vm))
		return 0;

	return i915_active_add_suspend_fence(&vm->active, ce, rq);
}

static inline int i915_vm_sync(struct i915_address_space *vm)
{
	/* Wait for all requests under this vm to finish */
	return i915_active_wait(&vm->active);
}

static inline bool i915_vm_is_active(const struct i915_address_space *vm)
{
	return !i915_active_is_idle(&vm->active);
}

#endif
