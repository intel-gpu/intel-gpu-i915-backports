// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

#include <linux/slab.h>

#include "gem/i915_gem_lmem.h"

#include "i915_trace.h"
#include "intel_gt.h"
#include "intel_gtt.h"
#include "intel_tlb.h"
#include "gen8_ppgtt.h"

void i915_px_cache_init(struct i915_px_cache *c)
{
	spin_lock_init(&c->lock);
}

struct drm_i915_gem_object *i915_vm_alloc_px(struct i915_address_space *vm)
{
	struct i915_px_cache *c = &vm->gt->px_cache;
	struct drm_i915_gem_object *px = NULL;

	if (!llist_empty(&c->px)) {
		struct llist_node *first;

		spin_lock(&c->lock);
		first = llist_del_first(&c->px);
		spin_unlock(&c->lock);
		if (first)
			px = container_of(first, struct drm_i915_gem_object, freed);
	}

	return px ?: vm->alloc_pt_dma(vm, SZ_4K);
}

static void px_release(struct drm_i915_gem_object *px)
{
	struct intel_memory_region *mem = px->mm.region.mem;

	spin_lock(&mem->objects.lock);
	list_move(&px->mm.region.link, &mem->objects.purgeable);
	spin_unlock(&mem->objects.lock);

	i915_gem_object_put(px);
}

void i915_vm_free_px(struct i915_address_space *vm,
		     struct drm_i915_gem_object *px)
{
	struct i915_px_cache *c = &vm->gt->px_cache;
	bool closed = true;

	rcu_read_lock();
	if (!c->closed) { /* serialise with i915_px_cache_fini() */
		llist_add(&px->freed, &c->px);
		closed = false;
	}
	rcu_read_unlock();

	if (closed)
		px_release(px);
}

bool i915_px_cache_release(struct i915_px_cache *c)
{
	struct drm_i915_gem_object *pt, *pn;
	struct llist_node *list;

	if (llist_empty(&c->px))
		return false;

	spin_lock(&c->lock);
	list = llist_del_all(&c->px);
	spin_unlock(&c->lock);

	llist_for_each_entry_safe(pt, pn, list, freed)
		px_release(pt);

	return true;
}

bool i915_px_cache_fini(struct i915_px_cache *c)
{
	/*
	 * Make sure that once we shutdown the cache, all concurrent and future
	 * teardown of pagetables are immediately freed and not leaked via the
	 * cache.
	 */
	c->closed = true;
	synchronize_rcu();
	return i915_px_cache_release(c);
}

struct i915_page_table *alloc_pt(struct i915_address_space *vm, int sz)
{
	struct i915_page_table *pt;

	pt = kmalloc(sizeof(*pt), I915_GFP_ALLOW_FAIL);
	if (unlikely(!pt))
		return ERR_PTR(-ENOMEM);

	pt->base = i915_vm_alloc_px(vm);
	if (IS_ERR(pt->base)) {
		kfree(pt);
		return ERR_PTR(-ENOMEM);
	}

	pt->is_compact = false;
	atomic_set(&pt->used, 0);
	return pt;
}

struct i915_page_directory *__alloc_pd(int count)
{
	struct i915_page_directory *pd;

	pd = kzalloc(sizeof(*pd), I915_GFP_ALLOW_FAIL);
	if (unlikely(!pd))
		return NULL;

	pd->entry = kcalloc(count, sizeof(*pd->entry), I915_GFP_ALLOW_FAIL);
	if (unlikely(!pd->entry)) {
		kfree(pd);
		return NULL;
	}

	spin_lock_init(&pd->lock);
	return pd;
}

struct i915_page_directory *alloc_pd(struct i915_address_space *vm)
{
	struct i915_page_directory *pd;

	pd = __alloc_pd(512);
	if (unlikely(!pd))
		return ERR_PTR(-ENOMEM);

	pd->pt.base = i915_vm_alloc_px(vm);
	if (IS_ERR(pd->pt.base)) {
		kfree(pd->entry);
		kfree(pd);
		return ERR_PTR(-ENOMEM);
	}

	return pd;
}

void free_px(struct i915_address_space *vm, struct i915_page_table *pt, int lvl)
{
	BUILD_BUG_ON(offsetof(struct i915_page_directory, pt));

	if (lvl) {
		struct i915_page_directory *pd =
			container_of(pt, typeof(*pd), pt);
		kfree(pd->entry);
	}

	if (pt->base)
		i915_vm_free_px(vm, pt->base);

	kfree(pt);
}

static struct i915_ppgtt *
__ppgtt_create(struct intel_gt *gt, u32 flags)
{
	return gen8_ppgtt_create(gt, flags);
}

struct i915_ppgtt *i915_ppgtt_create(struct intel_gt *gt, u32 flags)
{
	struct i915_ppgtt *ppgtt;

	ppgtt = __ppgtt_create(gt, flags);
	if (IS_ERR(ppgtt))
		return ppgtt;

	trace_i915_ppgtt_create(&ppgtt->vm);

	return ppgtt;
}

static u64 pte_size(const struct i915_vma *vma)
{
	return min(vma->size, i915_vma_size(vma));
}

static void vma_invalidate_tlb(struct i915_vma *vma)
{
	struct i915_address_space *vm = vma->vm;
	struct drm_i915_gem_object *obj = vma->obj;
	struct intel_gt *gt;
	int id;

	/*
	 * Before we release the pages that were bound by this vma, we
	 * must invalidate all the TLBs that may still have a reference
	 * back to our physical address. It only needs to be done once,
	 * so after updating the PTE to point away from the pages, record
	 * the most recent TLB invalidation seqno, and if we have not yet
	 * flushed the TLBs upon release, perform a full invalidation.
	 */
	for_each_gt(gt, vm->i915, id) {
		WRITE_ONCE(obj->mm.tlb[id], 0);
		if (!atomic_read(&vm->active_contexts[id]))
			continue;

		if (!intel_gt_invalidate_tlb_range(gt, vm,
						   i915_vma_offset(vma),
						   pte_size(vma)))
			WRITE_ONCE(obj->mm.tlb[id],
				   intel_gt_next_invalidate_tlb_full(vm->gt));
	}
}

int ppgtt_bind_vma(struct i915_address_space *vm,
		   struct i915_vma *vma,
		   unsigned int pat_index,
		   u32 flags)
{
	u32 pte_flags;
	int err;

	/* Paper over race with vm_unbind */
	if (!drm_mm_node_allocated(&vma->node))
		return 0;

	/*
	 * Force the next access to this vma to trigger a pagefault.
	 * This only installs a NULL PTE, and will *not* populate TLB.
	 */
	if (!(flags & PIN_RESIDENT))
		return 0;

	/* Applicable to VLV, and gen8+ */
	pte_flags = 0;
	if (vma->vm->has_read_only && i915_gem_object_is_readonly(vma->obj))
		pte_flags |= PTE_READ_ONLY;
	if (i915_gem_object_is_lmem(vma->obj) ||
	    i915_gem_object_has_fabric(vma->obj))
		pte_flags |= (vma->vm->top == 4 ? PTE_LM | PTE_AE : PTE_LM);

	err = vm->insert_entries(vm, vma, pat_index, pte_flags);
	if (unlikely(err))
		return err;

	i915_write_barrier(vm->i915);
	set_bit(I915_VMA_ALLOC_BIT, __i915_vma_flags(vma));

	if (vm->fault_end > vm->fault_start) { /* Was there a scratch page access? */
		u64 start = vma->node.start;
		u64 end = start + vma->node.size;

		if (start < vm->fault_end && end > vm->fault_start) {
			vma_invalidate_tlb(vma);
			i915_vm_heal_scratch(vm, start, end);
		}
	}

	return 0;
}

void ppgtt_unbind_vma(struct i915_address_space *vm, struct i915_vma *vma)
{
	if (!test_and_clear_bit(I915_VMA_ALLOC_BIT, __i915_vma_flags(vma)))
		return;

	vm->clear_range(vm, i915_vma_offset(vma), pte_size(vma));

	i915_write_barrier(vm->i915);
	vma_invalidate_tlb(vma);
}

static unsigned long pd_count(u64 size, int shift)
{
	/* Beware later misalignment */
	return (size + 2 * (BIT_ULL(shift) - 1)) >> shift;
}

u64 i915_vm_estimate_pt_size(struct i915_address_space *vm, u64 size)
{
	return pd_count(size, vm->pd_shift) * I915_GTT_PAGE_SIZE_4K;
}

int ppgtt_set_pages(struct i915_vma *vma)
{
	GEM_BUG_ON(vma->pages);

	vma->pages = vma->obj->mm.pages;
	vma->page_sizes = vma->obj->mm.page_sizes;

	return 0;
}

void ppgtt_clear_pages(struct i915_vma *vma)
{
	GEM_BUG_ON(!vma->pages);

	vma->pages = NULL;
	vma->page_sizes = 0;
}

int ppgtt_init(struct i915_ppgtt *ppgtt, struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;
	unsigned int ppgtt_size = INTEL_INFO(i915)->ppgtt_size;
	int err;

	if (i915->params.ppgtt_size && IS_PONTEVECCHIO(i915))
		ppgtt_size = i915->params.ppgtt_size;

	ppgtt->vm.gt = gt;
	ppgtt->vm.i915 = i915;
	ppgtt->vm.dma = i915->drm.dev;
	ppgtt->vm.total = BIT_ULL(ppgtt_size);

	if (ppgtt_size > 48)
		ppgtt->vm.top = 4;
	else if (ppgtt_size > 32)
		ppgtt->vm.top = 3;
	else if (ppgtt_size == 32)
		ppgtt->vm.top = 2;
	else
		ppgtt->vm.top = 1;

	err = i915_address_space_init(&ppgtt->vm, VM_CLASS_PPGTT);
	if (err)
		return err;

	ppgtt->vm.vma_ops.bind_vma    = ppgtt_bind_vma;
	ppgtt->vm.vma_ops.unbind_vma  = ppgtt_unbind_vma;
	ppgtt->vm.vma_ops.set_pages   = ppgtt_set_pages;
	ppgtt->vm.vma_ops.clear_pages = ppgtt_clear_pages;

	return 0;
}
