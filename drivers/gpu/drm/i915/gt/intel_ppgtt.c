// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

#include <linux/interval_tree_generic.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>

#include "gem/i915_gem_lmem.h"

#include "i915_trace.h"
#include "intel_gt.h"
#include "intel_gt_requests.h"
#include "intel_gtt.h"
#include "intel_tlb.h"
#include "gen8_ppgtt.h"

static struct kmem_cache *slab_pt;
static struct kmem_cache *slab_pd;

struct drm_i915_gem_object *i915_vm_alloc_px(struct i915_address_space *vm)
{
	struct llist_node *first = NULL;

	if (IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_PX_CACHE) &&
	    likely(vm->gt->px_cache)) {
		struct llist_head *c;

		preempt_disable();
		c = this_cpu_ptr(vm->gt->px_cache);
		first = c->first;
		if (first)
			c->first = first->next;
		preempt_enable();
	}

	return first ? container_of(first, struct drm_i915_gem_object, freed) : vm->alloc_pt_dma(vm, SZ_4K);
}

static void i915_vm_free_px(struct i915_address_space *vm,
			    struct drm_i915_gem_object *px)
{
	if (IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_PX_CACHE) &&
	    likely(vm->gt->px_cache && px_vaddr(px))) {
		preempt_disable();
		__llist_add(&px->freed, this_cpu_ptr(vm->gt->px_cache));
		preempt_enable();
	} else {
		i915_gem_object_put(px);
	}
}

static void __i915_px_cache_release(struct llist_head *c)
{
	struct drm_i915_gem_object *pt, *pn;

	llist_for_each_entry_safe(pt, pn, __llist_del_all(c), freed)
		i915_gem_object_put(pt);
}

struct px_cache_cpu {
	struct intel_gt *gt;
	bool result;
};

static void i915_px_cache_release_cpu(void *arg)
{
	struct px_cache_cpu *data = arg;
	struct llist_head *c;

	preempt_disable();
	c = this_cpu_ptr(data->gt->px_cache);
	if (!llist_empty(c)) {
		__i915_px_cache_release(c);
		data->result = true;
	}
	preempt_enable();
}

int i915_px_cache_init(struct intel_gt *gt)
{
	int cpu;

	if (!IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_PX_CACHE))
		return 0;

	gt->px_cache = alloc_percpu(*gt->px_cache);
	if (unlikely(!gt->px_cache))
		return -ENOMEM;

	for_each_possible_cpu(cpu)
		init_llist_head(per_cpu_ptr(gt->px_cache, cpu));

	return 0;
}

static bool has_px_cache(int cpu, void *arg)
{
	struct px_cache_cpu *data = arg;

	return !llist_empty(per_cpu_ptr(data->gt->px_cache, cpu));
}

bool i915_px_cache_release(struct intel_gt *gt)
{
	struct px_cache_cpu data = { .gt = gt };

	if (!IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_PX_CACHE))
		return false;

	if (unlikely(!gt->px_cache))
		return false;

	on_each_cpu_cond(has_px_cache, i915_px_cache_release_cpu, &data, true);

	return data.result;
}

void i915_px_cache_fini(struct intel_gt *gt)
{
	struct llist_head __percpu *px;
	int cpu;

	if (!IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_PX_CACHE))
		return;

	px = fetch_and_zero(&gt->px_cache);
	if (unlikely(!px))
		return;

	rcu_barrier();
	for_each_possible_cpu(cpu)
		__i915_px_cache_release(per_cpu_ptr(px, cpu));
	i915_gem_flush_free_objects(gt->i915);
	rcu_barrier();

	free_percpu(px);
}

struct i915_page_table *alloc_pt(struct i915_address_space *vm, int sz)
{
	struct i915_page_table *pt;

	pt = kmem_cache_alloc(slab_pt, I915_GFP_ALLOW_FAIL);
	if (unlikely(!pt))
		return ERR_PTR(-ENOMEM);

	pt->base = i915_vm_alloc_px(vm);
	if (IS_ERR(pt->base)) {
		kmem_cache_free(slab_pt, pt);
		return ERR_PTR(-ENOMEM);
	}

	pt->is_compact = false;
	atomic_set(&pt->used, 0);
	return pt;
}

struct i915_page_directory *__alloc_pd(int count)
{
	struct i915_page_directory *pd;

	pd = kmem_cache_alloc(slab_pd, I915_GFP_ALLOW_FAIL);
	if (unlikely(!pd))
		return NULL;

	pd->entry = kcalloc(count, sizeof(*pd->entry), I915_GFP_ALLOW_FAIL);
	if (unlikely(!pd->entry)) {
		kmem_cache_free(slab_pd, pd);
		return NULL;
	}

	pd->pt.is_compact = false;
	atomic_set(&pd->pt.used, 0);
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
		kmem_cache_free(slab_pd, pd);
		return ERR_PTR(-ENOMEM);
	}

	return pd;
}

void free_px(struct i915_address_space *vm, struct i915_page_table *pt, int lvl)
{
	BUILD_BUG_ON(offsetof(struct i915_page_directory, pt));

	if (pt->base)
		i915_vm_free_px(vm, pt->base);

	if (lvl) {
		struct i915_page_directory *pd =
			container_of(pt, typeof(*pd), pt);
		kfree(pd->entry);
	}

	kmem_cache_free(lvl ? slab_pd : slab_pt, pt);
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
	return min(vma->size, __i915_vma_size(vma));
}

struct tlb_range {
	struct rb_node rb;
	union {
		struct rcu_head rcu;
		struct tlb_range *erase;
	};
	u64 __subtree_last;
	u64 start, end;
	u32 seqno;
};

#define START(r) ((r)->start)
#define END(r) ((r)->end)

INTERVAL_TREE_DEFINE(struct tlb_range, rb,
		     u64, __subtree_last,
		     START, END, static inline, tlb_range)

#define to_tlb_range(n) rb_entry(READ_ONCE(n), struct tlb_range, rb)
#define next_tlb_range(r) rb_entry(rb_next(&(r)->rb), struct tlb_range, rb)

static inline struct tlb_range *
tlb_range_first(struct rb_root_cached *root, u64 start, u64 last)
{
	struct tlb_range *node;

	node = to_tlb_range(root->rb_leftmost);
	if (!node || node->start > last)
		return NULL;

	node = to_tlb_range(root->rb_root.rb_node);
	if (!node || node->__subtree_last < start)
		return NULL;

	do {
		struct tlb_range *left = to_tlb_range(node->rb.rb_left);

		if (left && start <= left->__subtree_last) {
			node = left;
			continue;
		}

		if (node->start <= last) {
			if (start <= node->end)
				return node;

			node = to_tlb_range(node->rb.rb_right);
			if (node && start <= node->__subtree_last)
				continue;
		}

		return NULL;
	} while (1);
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
		u32 seqno = 0;

		if (atomic_read(&vm->active_contexts[id])) {
			seqno = intel_gt_invalidate_tlb_range(gt, vm,
							      __i915_vma_offset(vma),
							      pte_size(vma));
			if (seqno) {
				struct i915_vm_tlb *tlb = &vm->tlb[id];
				struct tlb_range *r;

				intel_tlb_advance(&tlb->last, seqno);

				r = NULL;
				if (likely(!tlb->has_error))
					r = kmalloc(sizeof(*r), GFP_NOWAIT | __GFP_NOWARN);
				if (r) {
					r->start = __i915_vma_offset(vma);
					r->end = r->start + pte_size(vma) - 1;
					r->seqno = seqno;

					spin_lock(&tlb->lock);
					tlb_range_insert(r, &tlb->range);
					spin_unlock(&tlb->lock);
				} else {
					tlb->has_error = true;
				}
			}
		}

		WRITE_ONCE(obj->mm.tlb[id], seqno);
	}
}

static bool tlb_range_prune(struct rb_root_cached *root, u64 start, u64 end)
{
	struct tlb_range *erase = NULL;
	struct tlb_range *r;

	end += start - 1;

	for (r = tlb_range_first(root, start, end); r && r->start < end; r = next_tlb_range(r)) {
		if (r->end <= end)
			r->end = start;
		if (r->start >= start)
			r->start = end;
		if (r->start >= r->end) {
			r->erase = erase;
			erase = r;
		}
	}

	while (erase) {
		r = erase;
		erase = r->erase;

		tlb_range_remove(r, root);
		kfree_rcu(r, rcu);
	}

	return RB_EMPTY_ROOT(&root->rb_root);
}

int ppgtt_bind_vma(struct i915_address_space *vm,
		   struct i915_vma *vma,
		   struct i915_gem_ww_ctx *ww,
		   unsigned int pat_index,
		   u32 flags)
{
	struct intel_gt *gt;
	u32 pte_flags;
	int err;
	int id;

	/* Paper over race with vm_unbind */
	if (!drm_mm_node_allocated(&vma->node))
		return 0;

	for_each_gt(gt, vm->i915, id) {
		struct i915_vm_tlb *tlb = &vm->tlb[id];
		struct rb_root root = RB_ROOT;
		struct tlb_range *r, *n;

		if (RB_EMPTY_ROOT(&tlb->range.rb_root))
			continue;

		spin_lock(&tlb->lock);
		if (i915_seqno_passed(READ_ONCE(gt->tlb.seqno), tlb->last)) {
			root = tlb->range.rb_root;
			tlb->range = RB_ROOT_CACHED;
			tlb->has_error = false;
		} else {
			if (tlb_range_prune(&tlb->range, vma->node.start, vma->node.size))
				tlb->has_error = false;
		}
		spin_unlock(&tlb->lock);

		rbtree_postorder_for_each_entry_safe(r, n, &root, rb)
			kfree_rcu(r, rcu);
	}

	/*
	 * Force the next access to this vma to trigger a pagefault.
	 * This only installs a NULL PTE, and will *not* populate TLB.
	 */
	if (!(flags & PIN_RESIDENT))
		return 0;

	/* Applicable to VLV, and gen8+ */
	pte_flags = 0;
	if (flags & PIN_READ_ONLY)
		pte_flags |= PTE_READ_ONLY;
	if (test_bit(I915_MM_NODE_READONLY_BIT, &vma->node.flags))
		pte_flags |= PTE_READ_ONLY;
	if (i915_gem_object_is_readonly(vma->obj))
		pte_flags |= PTE_READ_ONLY;
	if (i915_gem_object_is_lmem(vma->obj) ||
	    i915_gem_object_has_fabric(vma->obj))
		pte_flags |= vm->top == 4 ? PTE_LM | PTE_AE : PTE_LM;

	err = vm->insert_entries(vm, vma, ww, pat_index, pte_flags);
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

u32 ppgtt_tlb_range(struct i915_address_space *vm, struct intel_gt *gt, u64 start, u64 end)
{
	struct i915_vm_tlb *tlb = &vm->tlb[gt->info.id];
	const u32 last = READ_ONCE(tlb->last);
	u32 seqno = 0;

	if (RB_EMPTY_ROOT(&tlb->range.rb_root))
		return 0;

	if (!tlb->has_error) {
		struct tlb_range *r;

		end += start - 1;

		rcu_read_lock();
		r = tlb_range_first(&tlb->range, start, end);
		if (!r) {
			rcu_read_unlock();
			return 0;
		}

		seqno = last - r->seqno;
		while ((r = next_tlb_range(r)) && r->start < end)
			seqno = min(seqno, last - r->seqno);
		rcu_read_unlock();
	}

	if (unlikely(!seqno))
		intel_tlb_advance(&tlb->last, intel_guc_invalidate_tlb_flush(&gt->uc.guc, vm->asid));

	return last - seqno;
}

void ppgtt_tlb_cleanup(struct i915_address_space *vm)
{
	struct tlb_range *r, *n;
	int i;

	for (i = 0; i < ARRAY_SIZE(vm->tlb); i++) {
		rbtree_postorder_for_each_entry_safe(r, n, &vm->tlb[i].range.rb_root, rb)
			kfree(r);
	}
}

void ppgtt_unbind_vma(struct i915_address_space *vm, struct i915_vma *vma)
{
	if (!test_and_clear_bit(I915_VMA_ALLOC_BIT, __i915_vma_flags(vma)))
		return;

	vm->clear_range(vm, __i915_vma_offset(vma), pte_size(vma));
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
	return 0;
}

void ppgtt_clear_pages(struct i915_vma *vma)
{
	GEM_BUG_ON(!vma->pages);
	vma->pages = NULL;
}

int ppgtt_init(struct i915_ppgtt *ppgtt, struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;
	unsigned int ppgtt_size = INTEL_INFO(i915)->ppgtt_size;
	int err;

	ppgtt->vm.gt = gt;
	ppgtt->vm.i915 = i915;
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

void intel_ppgtt_module_exit(void)
{
	kmem_cache_destroy(slab_pt);
	kmem_cache_destroy(slab_pd);
}

int __init intel_ppgtt_module_init(void)
{
	slab_pt = KMEM_CACHE(i915_page_table, SLAB_TYPESAFE_BY_RCU);
	if (!slab_pt)
		goto err;

	slab_pd = KMEM_CACHE(i915_page_directory, SLAB_TYPESAFE_BY_RCU);
	if (!slab_pd)
		goto err_pt;

	return 0;
err_pt:
	kmem_cache_destroy(slab_pt);
err:
	return -ENOMEM;
}
