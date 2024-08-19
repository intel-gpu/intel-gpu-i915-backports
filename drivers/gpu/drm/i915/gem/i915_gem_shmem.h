/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __I915_GEM_SHMEM_H__
#define __I915_GEM_SHMEM_H__

#include <linux/list.h>
#include <linux/types.h>

#include "gt/intel_gt_defines.h"

#include "i915_active.h"
#include "i915_scheduler_types.h"
#include "i915_sw_fence.h"

struct i915_dma_engine;
struct intel_memory_region;
struct page;

struct clear_page {
	struct list_head link;
	struct i915_active_fence active;
	struct i915_sw_dma_fence_cb cb;
	struct i915_dependency dep;
	struct i915_dma_engine *engine;
	struct page *page;
	dma_addr_t dma[2];
	u32 tlb[I915_MAX_GT];
};

static inline struct clear_page *to_clear_page(struct page *page)
{
	GEM_BUG_ON(!PagePrivate(page));
	return (struct clear_page *)page->private;
}

unsigned long i915_gem_reap_clear_smem(struct intel_memory_region *mem, int order, unsigned long limit);
bool i915_gem_shmem_park(struct intel_memory_region *mem);

void i915_gem_shmem_module_exit(void);
int i915_gem_shmem_module_init(void);

#endif /* __I915_GEM_SHMEM_H__ */
