/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "mock_gtt.h"

static void mock_insert_page(struct i915_address_space *vm,
			     dma_addr_t addr,
			     u64 offset,
			     unsigned int pat_index,
			     u32 flags)
{
}

static int mock_insert_entries(struct i915_address_space *vm,
			       struct i915_vma *vma,
			       struct i915_gem_ww_ctx *ww,
			       unsigned int pat_index,
			       u32 flags)
{
	return 0;
}

static int mock_bind_ppgtt(struct i915_address_space *vm,
			   struct i915_vma *vma,
			   struct i915_gem_ww_ctx *ww,
			   unsigned int pat_index,
			   u32 flags)
{
	return 0;
}

static void mock_unbind_ppgtt(struct i915_address_space *vm,
			      struct i915_vma *vma)
{
}

static void mock_cleanup(struct i915_address_space *vm)
{
}

static void mock_clear_range(struct i915_address_space *vm,
			     u64 start, u64 length)
{
}

struct i915_ppgtt *mock_ppgtt(struct drm_i915_private *i915, const char *name)
{
	struct i915_ppgtt *ppgtt;
	int err;

	ppgtt = kzalloc(sizeof(*ppgtt), GFP_KERNEL);
	if (!ppgtt)
		return NULL;

	ppgtt->vm.gt = to_gt(i915);
	ppgtt->vm.i915 = i915;
	ppgtt->vm.total = round_down(U64_MAX, PAGE_SIZE);
	ppgtt->vm.dma = i915->drm.dev;

	err = i915_address_space_init(&ppgtt->vm, VM_CLASS_PPGTT);
	if (err) {
		kfree(ppgtt);
		return ERR_PTR(err);
	}

	ppgtt->vm.alloc_pt_dma = alloc_pt_dma;
	ppgtt->vm.alloc_scratch_dma = alloc_pt_dma;

	ppgtt->vm.clear_range = mock_clear_range;
	ppgtt->vm.insert_page = mock_insert_page;
	ppgtt->vm.insert_entries = mock_insert_entries;
	ppgtt->vm.cleanup = mock_cleanup;

	ppgtt->vm.vma_ops.bind_vma    = mock_bind_ppgtt;
	ppgtt->vm.vma_ops.unbind_vma  = mock_unbind_ppgtt;
	ppgtt->vm.vma_ops.set_pages   = ppgtt_set_pages;
	ppgtt->vm.vma_ops.clear_pages = ppgtt_clear_pages;

	return ppgtt;
}

static int mock_bind_ggtt(struct i915_address_space *vm,
			  struct i915_vma *vma,
			  struct i915_gem_ww_ctx *ww,
			  unsigned int pat_index,
			  u32 flags)
{
	return 0;
}

static void mock_unbind_ggtt(struct i915_address_space *vm,
			     struct i915_vma *vma)
{
}

void mock_init_ggtt(struct intel_gt *gt)
{
	struct i915_ggtt *ggtt = gt->ggtt;

	ggtt->vm.gt = gt;
	ggtt->vm.i915 = gt->i915;
	ggtt->vm.is_ggtt = true;

	ggtt->vm.total = 4096 * PAGE_SIZE;

	ggtt->vm.alloc_pt_dma = alloc_pt_dma;
	ggtt->vm.alloc_scratch_dma = alloc_pt_dma;

	ggtt->vm.clear_range = mock_clear_range;
	ggtt->vm.insert_page = mock_insert_page;
	ggtt->vm.insert_entries = mock_insert_entries;
	ggtt->vm.cleanup = mock_cleanup;

	ggtt->vm.vma_ops.bind_vma    = mock_bind_ggtt;
	ggtt->vm.vma_ops.unbind_vma  = mock_unbind_ggtt;
	ggtt->vm.vma_ops.set_pages   = ggtt_set_pages;
	ggtt->vm.vma_ops.clear_pages = ggtt_clear_pages;

	INIT_LIST_HEAD(&ggtt->gt_list);
	i915_address_space_init(&ggtt->vm, VM_CLASS_GGTT);
}

void mock_fini_ggtt(struct i915_ggtt *ggtt)
{
	i915_address_space_fini(&ggtt->vm);
}
