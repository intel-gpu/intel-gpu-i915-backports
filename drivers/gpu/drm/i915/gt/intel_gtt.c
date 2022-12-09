// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

#include <linux/slab.h> /* fault-inject.h is not standalone! */

#include <linux/fault-inject.h>
#include <linux/pseudo_fs.h>

#include <drm/drm_cache.h>

#include "gem/i915_gem_internal.h"
#include "gem/i915_gem_lmem.h"
#include "gem/i915_gem_region.h"
#include "gem/i915_gem_vm_bind.h"

#include "i915_svm.h"
#include "i915_trace.h"
#include "i915_utils.h"
#include "intel_gt.h"
#include "intel_gt_regs.h"
#include "intel_gtt.h"


static bool intel_ggtt_update_needs_vtd_wa(struct drm_i915_private *i915)
{
	return IS_BROXTON(i915) && i915_vtd_active(i915);
}

bool intel_vm_no_concurrent_access_wa(struct drm_i915_private *i915)
{
	return IS_CHERRYVIEW(i915) || intel_ggtt_update_needs_vtd_wa(i915);
}

struct drm_i915_gem_object *alloc_pt_lmem(struct i915_address_space *vm, int sz)
{
	struct drm_i915_gem_object *obj;

	obj = intel_gt_object_create_lmem(vm->gt, sz,
					  I915_BO_ALLOC_IGNORE_MIN_PAGE_SIZE);
	/*
	 * Ensure all paging structures for this vm share the same dma-resv
	 * object underneath, with the idea that one object_lock() will lock
	 * them all at once.
	 */
	if (!IS_ERR(obj)) {
		obj->base.resv = i915_vm_resv_get(vm);
		obj->shares_resv_from = vm;
	}

	return obj;
}

struct drm_i915_gem_object *alloc_pt_dma(struct i915_address_space *vm, int sz)
{
	struct drm_i915_gem_object *obj;

	if (I915_SELFTEST_ONLY(should_fail(&vm->fault_attr, 1)))
		i915_gem_shrink_all(vm->i915);

	obj = i915_gem_object_create_internal(vm->i915, sz);
	/*
	 * Ensure all paging structures for this vm share the same dma-resv
	 * object underneath, with the idea that one object_lock() will lock
	 * them all at once.
	 */
	if (!IS_ERR(obj)) {
		obj->base.resv = i915_vm_resv_get(vm);
		obj->shares_resv_from = vm;
	}

	return obj;
}

int map_pt_dma(struct i915_address_space *vm, struct drm_i915_gem_object *obj)
{
	enum i915_map_type type;
	void *vaddr;

	type = i915_coherent_map_type(vm->i915, obj, true);
	vaddr = i915_gem_object_pin_map_unlocked(obj, type);
	if (IS_ERR(vaddr))
		return PTR_ERR(vaddr);

	i915_gem_object_make_unshrinkable(obj);
	return 0;
}

int map_pt_dma_locked(struct i915_address_space *vm, struct drm_i915_gem_object *obj)
{
	enum i915_map_type type;
	void *vaddr;

	type = i915_coherent_map_type(vm->i915, obj, true);
	vaddr = i915_gem_object_pin_map(obj, type);
	if (IS_ERR(vaddr))
		return PTR_ERR(vaddr);

	i915_gem_object_make_unshrinkable(obj);
	return 0;
}

static void __i915_vm_close(struct i915_address_space *vm)
{
	struct drm_i915_gem_object *obj, *on;
	struct i915_vma *vma, *vn;
	struct list_head list;

	INIT_LIST_HEAD(&list);

	spin_lock(&vm->i915->vm_priv_obj_lock);
	list_for_each_entry_safe(obj, on, &vm->priv_obj_list, priv_obj_link) {
		list_del_init(&obj->priv_obj_link);
		obj->vm = (void *)I915_BO_INVALID_PRIV_VM;
	}
	spin_unlock(&vm->i915->vm_priv_obj_lock);

	i915_gem_vm_unbind_all(vm);
	mutex_lock(&vm->mutex);
	list_for_each_entry_safe(vma, vn, &vm->bound_list, vm_link) {
		struct drm_i915_gem_object *obj = vma->obj;

		/* Keep the obj (and hence the vma) alive as _we_ destroy it */
		if (!kref_get_unless_zero(&obj->base.refcount))
			continue;

		atomic_and(~I915_VMA_PIN_MASK, &vma->flags);
		WARN_ON(__i915_vma_unbind(vma));
		list_add_tail(&vma->vm_link, &list);
	}
	mutex_unlock(&vm->mutex);

	/* Release vma outside vm->mutex */
	list_for_each_entry_safe(vma, vn, &list, vm_link) {
		struct drm_i915_gem_object *obj = vma->obj;

		__i915_vma_put(vma);
		i915_gem_object_put(obj);
	}

	if (vm->mfence.obj) {
		i915_gem_object_put(vm->mfence.obj);
		vm->mfence.vma = NULL;
		vm->mfence.obj = NULL;
	}
}

/* lock the vm into the current ww, if we lock one, we lock all */
int i915_vm_lock_objects(struct i915_address_space *vm,
			 struct i915_gem_ww_ctx *ww)
{
	if (vm->scratch[0]->base.resv == &vm->_resv) {
		return i915_gem_object_lock(vm->scratch[0], ww);
	} else {
		struct i915_ppgtt *ppgtt = i915_vm_to_ppgtt(vm);

		/* We borrowed the scratch page from ggtt, take the top level object */
		return i915_gem_object_lock(ppgtt->pd->pt.base, ww);
	}
}

void i915_address_space_fini(struct i915_address_space *vm)
{
	if (vm->client)
		i915_drm_client_put(vm->client);

	i915_active_fini(&vm->active);

	drm_mm_takedown(&vm->mm);

	if (!i915_is_ggtt(vm) && HAS_UM_QUEUES(vm->i915))
		GEM_WARN_ON(!xa_erase(&vm->i915->asid_resv.xa, vm->asid));

	mutex_destroy(&vm->mutex);
	mutex_destroy(&vm->svm_mutex);
	i915_gem_object_put(vm->root_obj);
	GEM_BUG_ON(!RB_EMPTY_ROOT(&vm->va.rb_root));
	mutex_destroy(&vm->vm_bind_lock);
	GEM_BUG_ON(!llist_empty(&vm->vm_bind_free_list));

	iput(vm->inode);
}

/**
 * i915_vm_resv_release - Final struct i915_address_space destructor
 * @kref: Pointer to the &i915_address_space.resv_ref member.
 *
 * This function is called when the last lock sharer no longer shares the
 * &i915_address_space._resv lock.
 */
void i915_vm_resv_release(struct kref *kref)
{
	struct i915_address_space *vm =
		container_of(kref, typeof(*vm), resv_ref);

	dma_resv_fini(&vm->_resv);
	kfree(vm);
}

static void __i915_vm_release(struct work_struct *work)
{
	struct i915_address_space *vm =
		container_of(work, struct i915_address_space, rcu.work);

	i915_svm_unbind_mm(vm);
	vm->cleanup(vm);
	i915_address_space_fini(vm);

	i915_vm_resv_put(vm);
}

void i915_vm_release(struct kref *kref)
{
	struct i915_address_space *vm =
		container_of(kref, struct i915_address_space, ref);

	GEM_BUG_ON(i915_is_ggtt(vm));
	trace_i915_ppgtt_release(vm);

	queue_rcu_work(system_unbound_wq, &vm->rcu);
}

static void i915_vm_close_work(struct work_struct *wrk)
{
	struct i915_address_space *vm = container_of(wrk, typeof(*vm),
						     close_work);

	__i915_vm_close(vm);
	i915_vm_put(vm);
}

void i915_vm_close(struct i915_address_space *vm)
{
	GEM_BUG_ON(atomic_read(&vm->open) <= 0);
	if (atomic_dec_and_test(&vm->open))
		queue_work(system_unbound_wq, &vm->close_work);
	else
		i915_vm_put(vm);
}

static inline struct i915_address_space *active_to_vm(struct i915_active *ref)
{
	return container_of(ref, typeof(struct i915_address_space), active);
}

static int __i915_vm_active(struct i915_active *ref)
{
	return i915_vm_tryopen(active_to_vm(ref)) ? 0 : -ENOENT;
}

static void __i915_vm_retire(struct i915_active *ref)
{
	i915_vm_close(active_to_vm(ref));
}

int i915_address_space_init(struct i915_address_space *vm, int subclass)
{
	u64 min_alignment;

	GEM_BUG_ON(!vm->total);

	kref_init(&vm->ref);

	/*
	 * Special case for GGTT that has already done an early
	 * kref_init here.
	 */
	if (!kref_read(&vm->resv_ref))
		kref_init(&vm->resv_ref);

	INIT_RCU_WORK(&vm->rcu, __i915_vm_release);
	atomic_set(&vm->open, 1);
	INIT_WORK(&vm->close_work, i915_vm_close_work);

	/*
	 * The vm->mutex must be reclaim safe (for use in the shrinker).
	 * Do a dummy acquire now under fs_reclaim so that any allocation
	 * attempt holding the lock is immediately reported by lockdep.
	 */
	mutex_init(&vm->mutex);
	mutex_init(&vm->svm_mutex);
	lockdep_set_subclass(&vm->mutex, subclass);

	if (!intel_vm_no_concurrent_access_wa(vm->i915)) {
		fs_reclaim_taints_mutex(&vm->mutex);
	} else {
		/*
		 * CHV + BXT VTD workaround use stop_machine(),
		 * which is allowed to allocate memory. This means &vm->mutex
		 * is the outer lock, and in theory we can allocate memory inside
		 * it through stop_machine().
		 *
		 * Add the annotation for this, we use trylock in shrinker.
		 */
		mutex_acquire(&vm->mutex.dep_map, 0, 0, _THIS_IP_);
		might_alloc(GFP_KERNEL);
		mutex_release(&vm->mutex.dep_map, _THIS_IP_);
	}
	dma_resv_init(&vm->_resv);

	vm->inode = alloc_anon_inode(vm->i915->drm.anon_inode->i_sb);
	if (IS_ERR(vm->inode))
		return PTR_ERR(vm->inode);

	min_alignment = I915_GTT_MIN_ALIGNMENT;
	if (subclass == VM_CLASS_GGTT &&
	    intel_ggtt_needs_same_mem_type_within_cl_wa(vm->i915)) {
		min_alignment = I915_GTT_PAGE_SIZE_64K;
	}

	memset64(vm->min_alignment, min_alignment,
		 ARRAY_SIZE(vm->min_alignment));

	if (HAS_64K_PAGES(vm->i915)) {
		vm->min_alignment[INTEL_MEMORY_LOCAL] = I915_GTT_PAGE_SIZE_64K;
		vm->min_alignment[INTEL_MEMORY_STOLEN_LOCAL] = I915_GTT_PAGE_SIZE_64K;
	}

	/* Wa_1409502670:xehpsdv */
	if (IS_XEHPSDV(vm->i915) && subclass == VM_CLASS_PPGTT)
		drm_mm_init(&vm->mm, I915_GTT_PAGE_SIZE_64K, vm->total - I915_GTT_PAGE_SIZE_64K);
	else
		drm_mm_init(&vm->mm, 0, vm->total);

	vm->mm.head_node.color = I915_COLOR_UNEVICTABLE;

	INIT_LIST_HEAD(&vm->debugger_fence_list);
	spin_lock_init(&vm->debugger_lock);

	INIT_LIST_HEAD(&vm->bound_list);

	vm->va = RB_ROOT_CACHED;
	INIT_LIST_HEAD(&vm->vm_bind_list);
	INIT_LIST_HEAD(&vm->vm_bound_list);
	mutex_init(&vm->vm_bind_lock);
	INIT_LIST_HEAD(&vm->non_priv_vm_bind_list);
	vm->root_obj = i915_gem_object_create_internal(vm->i915, PAGE_SIZE);
	GEM_BUG_ON(IS_ERR(vm->root_obj));
	INIT_LIST_HEAD(&vm->priv_obj_list);
	INIT_LIST_HEAD(&vm->vm_capture_list);
	spin_lock_init(&vm->vm_capture_lock);
	INIT_LIST_HEAD(&vm->vm_rebind_list);
	spin_lock_init(&vm->vm_rebind_lock);
	vm->has_scratch = true;

	i915_active_init(&vm->active, __i915_vm_active, __i915_vm_retire, 0);

	init_llist_head(&vm->vm_bind_free_list);
	atomic_set(&vm->invalidations, 0);
	i915_gem_vm_bind_init(vm);

	if (HAS_UM_QUEUES(vm->i915) && subclass == VM_CLASS_PPGTT) {
		u32 asid;
		int err;
		/* ASID field is 20-bit wide */
		err = xa_alloc_cyclic(&vm->i915->asid_resv.xa,
				      &asid, vm,
				      XA_LIMIT(0, I915_MAX_ASID - 1),
				      &vm->i915->asid_resv.next_id,
				      GFP_KERNEL);
		if (unlikely(err < 0)) {
			iput(vm->inode);
			return err;
		}

		vm->asid = asid;
	}

	RCU_INIT_POINTER(vm->svm, NULL);
	return 0;
}

void clear_pages(struct i915_vma *vma)
{
	GEM_BUG_ON(!vma->pages);

	if (vma->pages != vma->obj->mm.pages) {
		sg_free_table(vma->pages);
		kfree(vma->pages);
	}
	vma->pages = NULL;

	memset(&vma->page_sizes, 0, sizeof(vma->page_sizes));
}

void *__px_vaddr(struct drm_i915_gem_object *p, bool *needs_flush)
{
	enum i915_map_type type;
	void *vaddr;

	GEM_BUG_ON(!i915_gem_object_has_pages(p));

	vaddr = page_unpack_bits(p->mm.mapping, &type);

	if (needs_flush) {
		/*
		 * On DG1 we sometimes see what looks like a bad TLB lookup in
		 * some tests. See if adding back the flush helps with the
		 * timing.
		 */
		if (IS_DG1(to_i915(p->base.dev)))
			*needs_flush = true;
		else
			*needs_flush = type != I915_MAP_WC;
	}

	return vaddr;
}

dma_addr_t __px_dma(struct drm_i915_gem_object *p)
{
	GEM_BUG_ON(!i915_gem_object_has_pages(p));
	return sg_dma_address(p->mm.pages->sgl);
}

struct page *__px_page(struct drm_i915_gem_object *p)
{
	GEM_BUG_ON(!i915_gem_object_has_pages(p));
	return sg_page(p->mm.pages->sgl);
}

void
fill_page_dma(struct drm_i915_gem_object *p, const u64 val, unsigned int count)
{
	bool needs_flush;
	void *vaddr;

	vaddr = __px_vaddr(p, &needs_flush);
	memset64(vaddr, val, count);
	if (needs_flush)
		drm_clflush_virt_range(vaddr, PAGE_SIZE);
}

static u32 poison_scratch_page(struct drm_i915_gem_object *scratch)
{
	void *vaddr = __px_vaddr(scratch, NULL);
	u32 val;

	val = 0;
	if (IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GEM)) {
		/*
		 * Partially randomise the scratch page.
		 *
		 * By giving every scratch page a unique poison, we can
		 * identify if a stale read is coming from a local scratch
		 * or worse, a remote vm. However, we still want to quickly
		 * identify scratch reads from regular client hangs, so
		 * we keep the top byte as a marker.
		 */
		val = get_random_u32();
		val >>= 8;
		val |= (u32)POISON_FREE << 24;
	}

	memset32(vaddr, val, scratch->base.size / sizeof(u32));
	drm_clflush_virt_range(vaddr, scratch->base.size);

	return val;
}

int setup_scratch_page(struct i915_address_space *vm)
{
	unsigned long size;

	/*
	 * In order to utilize 64K pages for an object with a size < 2M, we will
	 * need to support a 64K scratch page, given that every 16th entry for a
	 * page-table operating in 64K mode must point to a properly aligned 64K
	 * region, including any PTEs which happen to point to scratch.
	 *
	 * This is only relevant for the 48b PPGTT where we support
	 * huge-gtt-pages, see also i915_vma_insert(). However, as we share the
	 * scratch (read-only) between all vm, we create one 64k scratch page
	 * for all.
	 */
	size = I915_GTT_PAGE_SIZE_4K;
	if (i915_vm_lvl(vm) >= 4 && !HAS_FULL_PS64(vm->i915) &&
	    HAS_PAGE_SIZES(vm->i915, I915_GTT_PAGE_SIZE_64K))
		size = I915_GTT_PAGE_SIZE_64K;

	do {
		struct drm_i915_gem_object *obj;

		obj = vm->alloc_scratch_dma(vm, size);
		if (IS_ERR(obj))
			goto skip;

		if (map_pt_dma(vm, obj))
			goto skip_obj;

		/* We need a single contiguous page for our scratch */
		if (obj->mm.page_sizes.sg < size)
			goto skip_obj;

		/* And it needs to be correspondingly aligned */
		if (__px_dma(obj) & (size - 1))
			goto skip_obj;

		/*
		 * Use a non-zero scratch page for debugging.
		 *
		 * We want a value that should be reasonably obvious
		 * to spot in the error state, while also causing a GPU hang
		 * if executed. We prefer using a clear page in production, so
		 * should it ever be accidentally used, the effect should be
		 * fairly benign.
		 */
		vm->poison = poison_scratch_page(obj);

		vm->scratch[0] = obj;
		vm->scratch_order = get_order(size);
		return 0;

skip_obj:
		i915_gem_object_put(obj);
skip:
		if (size == I915_GTT_PAGE_SIZE_4K)
			return -ENOMEM;

		/*
		 * If we need 64K minimum GTT pages for device local-memory,
		 * like on XEHPSDV, then we need to fail the allocation here,
		 * otherwise we can't safely support the insertion of
		 * local-memory pages for this vm, since the HW expects the
		 * correct physical alignment and size when the page-table is
		 * operating in 64K GTT mode, which includes any scratch PTEs,
		 * since userspace can still touch them.
		 */
		if (HAS_64K_PAGES(vm->i915))
			return -ENOMEM;

		size = I915_GTT_PAGE_SIZE_4K;
	} while (1);
}

void free_scratch(struct i915_address_space *vm)
{
	int i;

	if (!vm->scratch[0])
		return;

	for (i = 0; i <= vm->top; i++)
		i915_gem_object_put(vm->scratch[i]);
}

void gtt_write_workarounds(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;
	struct intel_uncore *uncore = gt->uncore;

	if (IS_SRIOV_VF(i915))
		return;

	/*
	 * This function is for gtt related workarounds. This function is
	 * called on driver load and after a GPU reset, so you can place
	 * workarounds here even if they get overwritten by GPU reset.
	 */
	/* WaIncreaseDefaultTLBEntries:chv,bdw,skl,bxt,kbl,glk,cfl,cnl,icl */
	if (IS_BROADWELL(i915))
		intel_uncore_write(uncore,
				   GEN8_L3_LRA_1_GPGPU,
				   GEN8_L3_LRA_1_GPGPU_DEFAULT_VALUE_BDW);
	else if (IS_CHERRYVIEW(i915))
		intel_uncore_write(uncore,
				   GEN8_L3_LRA_1_GPGPU,
				   GEN8_L3_LRA_1_GPGPU_DEFAULT_VALUE_CHV);
	else if (IS_GEN9_LP(i915))
		intel_uncore_write(uncore,
				   GEN8_L3_LRA_1_GPGPU,
				   GEN9_L3_LRA_1_GPGPU_DEFAULT_VALUE_BXT);
	else if (GRAPHICS_VER(i915) >= 9 && GRAPHICS_VER(i915) <= 11)
		intel_uncore_write(uncore,
				   GEN8_L3_LRA_1_GPGPU,
				   GEN9_L3_LRA_1_GPGPU_DEFAULT_VALUE_SKL);

	/*
	 * To support 64K PTEs we need to first enable the use of the
	 * Intermediate-Page-Size(IPS) bit of the PDE field via some magical
	 * mmio, otherwise the page-walker will simply ignore the IPS bit. This
	 * shouldn't be needed after GEN10.
	 *
	 * 64K pages were first introduced from BDW+, although technically they
	 * only *work* from gen9+. For pre-BDW we instead have the option for
	 * 32K pages, but we don't currently have any support for it in our
	 * driver.
	 */
	if (HAS_PAGE_SIZES(i915, I915_GTT_PAGE_SIZE_64K) &&
	    GRAPHICS_VER(i915) <= 10)
		intel_uncore_rmw(uncore,
				 GEN8_GAMW_ECO_DEV_RW_IA,
				 0,
				 GAMW_ECO_ENABLE_64K_IPS_FIELD);

	if (IS_GRAPHICS_VER(i915, 8, 11)) {
		bool can_use_gtt_cache = true;

		/*
		 * According to the BSpec if we use 2M/1G pages then we also
		 * need to disable the GTT cache. At least on BDW we can see
		 * visual corruption when using 2M pages, and not disabling the
		 * GTT cache.
		 */
		if (HAS_PAGE_SIZES(i915, I915_GTT_PAGE_SIZE_2M))
			can_use_gtt_cache = false;

		/* WaGttCachingOffByDefault */
		intel_uncore_write(uncore,
				   HSW_GTT_CACHE_EN,
				   can_use_gtt_cache ? GTT_CACHE_EN_ALL : 0);
		drm_WARN_ON_ONCE(&i915->drm, can_use_gtt_cache &&
				 intel_uncore_read(uncore,
						   HSW_GTT_CACHE_EN) == 0);
	}
}

static void mtl_setup_private_ppat(struct intel_uncore *uncore)
{
	intel_uncore_write(uncore, GEN12_PAT_INDEX(0),
			   MTL_PPAT_L4_0_WB);
	intel_uncore_write(uncore, GEN12_PAT_INDEX(1),
			   MTL_PPAT_L4_1_WT | MTL_2_COH_1W);
	intel_uncore_write(uncore, GEN12_PAT_INDEX(2),
			   MTL_PPAT_L4_3_UC | MTL_2_COH_1W);
	intel_uncore_write(uncore, GEN12_PAT_INDEX(3),
			   MTL_PPAT_L4_0_WB | MTL_2_COH_1W);
	intel_uncore_write(uncore, GEN12_PAT_INDEX(4),
			   MTL_PPAT_L4_0_WB | MTL_3_COH_2W);

	/*
	 * Remaining PAT entries are left at the hardware-default
	 * fully-cached setting
	 */
}

static void pvc_setup_private_ppat(struct intel_uncore *uncore)
{
        intel_uncore_write(uncore, GEN12_PAT_INDEX(0), GEN8_PPAT_UC);
        intel_uncore_write(uncore, GEN12_PAT_INDEX(1), GEN8_PPAT_WC);
        intel_uncore_write(uncore, GEN12_PAT_INDEX(2), GEN8_PPAT_WT);
        intel_uncore_write(uncore, GEN12_PAT_INDEX(3), GEN8_PPAT_WB);
        intel_uncore_write(uncore, GEN12_PAT_INDEX(4),
			   GEN12_PPAT_CLOS(1) | GEN8_PPAT_WT);
        intel_uncore_write(uncore, GEN12_PAT_INDEX(5),
			   GEN12_PPAT_CLOS(1) | GEN8_PPAT_WB);
        intel_uncore_write(uncore, GEN12_PAT_INDEX(6),
			   GEN12_PPAT_CLOS(2) | GEN8_PPAT_WT);
        intel_uncore_write(uncore, GEN12_PAT_INDEX(7),
			   GEN12_PPAT_CLOS(2) | GEN8_PPAT_WB);
}

static void tgl_setup_private_ppat(struct intel_uncore *uncore)
{
	/* TGL doesn't support LLC or AGE settings */
	intel_uncore_write(uncore, GEN12_PAT_INDEX(0), GEN8_PPAT_WB);
	intel_uncore_write(uncore, GEN12_PAT_INDEX(1), GEN8_PPAT_WC);
	intel_uncore_write(uncore, GEN12_PAT_INDEX(2), GEN8_PPAT_WT);
	intel_uncore_write(uncore, GEN12_PAT_INDEX(3), GEN8_PPAT_UC);
	intel_uncore_write(uncore, GEN12_PAT_INDEX(4), GEN8_PPAT_WB);
	intel_uncore_write(uncore, GEN12_PAT_INDEX(5), GEN8_PPAT_WB);
	intel_uncore_write(uncore, GEN12_PAT_INDEX(6), GEN8_PPAT_WB);
	intel_uncore_write(uncore, GEN12_PAT_INDEX(7), GEN8_PPAT_WB);
}

static void icl_setup_private_ppat(struct intel_uncore *uncore)
{
	intel_uncore_write(uncore,
			   GEN10_PAT_INDEX(0),
			   GEN8_PPAT_WB | GEN8_PPAT_LLC);
	intel_uncore_write(uncore,
			   GEN10_PAT_INDEX(1),
			   GEN8_PPAT_WC | GEN8_PPAT_LLCELLC);
	intel_uncore_write(uncore,
			   GEN10_PAT_INDEX(2),
			   GEN8_PPAT_WB | GEN8_PPAT_ELLC_OVERRIDE);
	intel_uncore_write(uncore,
			   GEN10_PAT_INDEX(3),
			   GEN8_PPAT_UC);
	intel_uncore_write(uncore,
			   GEN10_PAT_INDEX(4),
			   GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(0));
	intel_uncore_write(uncore,
			   GEN10_PAT_INDEX(5),
			   GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(1));
	intel_uncore_write(uncore,
			   GEN10_PAT_INDEX(6),
			   GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(2));
	intel_uncore_write(uncore,
			   GEN10_PAT_INDEX(7),
			   GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(3));
}

/*
 * The GGTT and PPGTT need a private PPAT setup in order to handle cacheability
 * bits. When using advanced contexts each context stores its own PAT, but
 * writing this data shouldn't be harmful even in those cases.
 */
static void bdw_setup_private_ppat(struct intel_uncore *uncore)
{
	struct drm_i915_private *i915 = uncore->i915;
	u64 pat;

	pat = GEN8_PPAT(0, GEN8_PPAT_WB | GEN8_PPAT_LLC) |	/* for normal objects, no eLLC */
	      GEN8_PPAT(1, GEN8_PPAT_WC | GEN8_PPAT_LLCELLC) |	/* for something pointing to ptes? */
	      GEN8_PPAT(3, GEN8_PPAT_UC) |			/* Uncached objects, mostly for scanout */
	      GEN8_PPAT(4, GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(0)) |
	      GEN8_PPAT(5, GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(1)) |
	      GEN8_PPAT(6, GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(2)) |
	      GEN8_PPAT(7, GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(3));

	/* for scanout with eLLC */
	if (GRAPHICS_VER(i915) >= 9)
		pat |= GEN8_PPAT(2, GEN8_PPAT_WB | GEN8_PPAT_ELLC_OVERRIDE);
	else
		pat |= GEN8_PPAT(2, GEN8_PPAT_WT | GEN8_PPAT_LLCELLC);

	intel_uncore_write(uncore, GEN8_PRIVATE_PAT_LO, lower_32_bits(pat));
	intel_uncore_write(uncore, GEN8_PRIVATE_PAT_HI, upper_32_bits(pat));
}

static void chv_setup_private_ppat(struct intel_uncore *uncore)
{
	u64 pat;

	/*
	 * Map WB on BDW to snooped on CHV.
	 *
	 * Only the snoop bit has meaning for CHV, the rest is
	 * ignored.
	 *
	 * The hardware will never snoop for certain types of accesses:
	 * - CPU GTT (GMADR->GGTT->no snoop->memory)
	 * - PPGTT page tables
	 * - some other special cycles
	 *
	 * As with BDW, we also need to consider the following for GT accesses:
	 * "For GGTT, there is NO pat_sel[2:0] from the entry,
	 * so RTL will always use the value corresponding to
	 * pat_sel = 000".
	 * Which means we must set the snoop bit in PAT entry 0
	 * in order to keep the global status page working.
	 */

	pat = GEN8_PPAT(0, CHV_PPAT_SNOOP) |
	      GEN8_PPAT(1, 0) |
	      GEN8_PPAT(2, 0) |
	      GEN8_PPAT(3, 0) |
	      GEN8_PPAT(4, CHV_PPAT_SNOOP) |
	      GEN8_PPAT(5, CHV_PPAT_SNOOP) |
	      GEN8_PPAT(6, CHV_PPAT_SNOOP) |
	      GEN8_PPAT(7, CHV_PPAT_SNOOP);

	intel_uncore_write(uncore, GEN8_PRIVATE_PAT_LO, lower_32_bits(pat));
	intel_uncore_write(uncore, GEN8_PRIVATE_PAT_HI, upper_32_bits(pat));
}

void setup_private_pat(struct intel_uncore *uncore)
{
	struct drm_i915_private *i915 = uncore->i915;

	GEM_BUG_ON(GRAPHICS_VER(i915) < 8);

	if (IS_SRIOV_VF(i915))
		return;

	if (IS_METEORLAKE(i915))
		mtl_setup_private_ppat(uncore);
	else if (IS_PONTEVECCHIO(i915))
		pvc_setup_private_ppat(uncore);
	else if (GRAPHICS_VER(i915) >= 12)
		tgl_setup_private_ppat(uncore);
	else if (GRAPHICS_VER(i915) >= 11)
		icl_setup_private_ppat(uncore);
	else if (IS_CHERRYVIEW(i915) || IS_GEN9_LP(i915))
		chv_setup_private_ppat(uncore);
	else
		bdw_setup_private_ppat(uncore);
}

int svm_bind_addr_prepare(struct i915_address_space *vm,
			  struct i915_vm_pt_stash *stash,
			  struct i915_gem_ww_ctx *ww,
			  u64 start, u64 size)
{
	int ret;

	ret = i915_vm_alloc_pt_stash(vm, stash, size);
	if (ret)
		return ret;

	ret = i915_vm_lock_objects(vm, ww);
	if (ret)
		return ret;

	ret = i915_vm_map_pt_stash(vm, stash);
	if (ret)
		return ret;

	vm->allocate_va_range(vm, stash, start, size);

	return 0;
}

int svm_bind_addr_commit(struct i915_address_space *vm, u64 start, u64 size,
			 u64 flags, struct sg_table *st, u32 sg_page_sizes)
{
	struct i915_vma *vma;
	u32 pte_flags = 0;

	/* use a vma wrapper */
	vma = i915_vma_alloc();
	if (!vma)
		return -ENOMEM;

	vma->page_sizes.sg = sg_page_sizes;
	vma->node.start = start;
	vma->node.size = size;
	__set_bit(DRM_MM_NODE_ALLOCATED_BIT, &vma->node.flags);
	vma->size = size;
	vma->pages = st;
	vma->vm = vm;

	/* Applicable to VLV, and gen8+ */
	if (flags & I915_GTT_SVM_READONLY)
		pte_flags |= PTE_READ_ONLY;
	if (flags & I915_GTT_SVM_LMEM)
		pte_flags |= (vm->top == 4 ? PTE_LM | PTE_AE : PTE_LM);

	vm->insert_entries(vm, vma,
			   i915_gem_get_pat_index(vm->i915, I915_CACHE_NONE),
			   pte_flags);
	i915_vma_free(vma);
	return 0;
}

int svm_bind_addr(struct i915_address_space *vm, struct i915_gem_ww_ctx *ww,
		  u64 start, u64 size, u64 flags,
		  struct sg_table *st, u32 sg_page_sizes)
{
	struct i915_vm_pt_stash stash = { 0 };
	int ret;

	ret = svm_bind_addr_prepare(vm, &stash, ww, start, size);
	if (ret)
		goto out_stash;

	ret = svm_bind_addr_commit(vm, start, size, flags,
				   st, sg_page_sizes);
out_stash:
	i915_vm_free_pt_stash(vm, &stash);
	return ret;
}

void svm_unbind_addr(struct i915_address_space *vm,
		     u64 start, u64 size)
{
	vm->clear_range(vm, start, size);
}

struct i915_vma *
__vm_create_scratch_for_read(struct i915_address_space *vm, unsigned long size)
{
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma;

	obj = i915_gem_object_create_internal(vm->i915, PAGE_ALIGN(size));
	if (IS_ERR(obj))
		return ERR_CAST(obj);

	i915_gem_object_set_cache_coherency(obj, I915_CACHING_CACHED);

	vma = i915_vma_instance(obj, vm, NULL);
	if (IS_ERR(vma)) {
		i915_gem_object_put(obj);
		return vma;
	}

	return vma;
}

struct i915_vma *
__vm_create_scratch_for_read_pinned(struct i915_address_space *vm, unsigned long size)
{
	struct i915_vma *vma;
	int err;

	vma = __vm_create_scratch_for_read(vm, size);
	if (IS_ERR(vma))
		return vma;

	err = i915_vma_pin(vma, 0, 0,
			   i915_vma_is_ggtt(vma) ? PIN_GLOBAL : PIN_USER);
	if (err) {
		i915_vma_put(vma);
		return ERR_PTR(err);
	}

	return vma;
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftests/mock_gtt.c"
#include "selftest_gtt.c"
#include "selftest_l4wa.c"
#endif
