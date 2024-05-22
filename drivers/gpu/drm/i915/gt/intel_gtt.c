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
#include "gem/i915_gem_vm_bind.h" /* XXX */

#include "i915_trace.h"
#include "i915_utils.h"
#include "intel_gt.h"
#include "intel_gt_mcr.h"
#include "intel_gt_print.h"
#include "intel_gt_regs.h"
#include "intel_gtt.h"

struct drm_i915_gem_object *alloc_pt_lmem(struct i915_address_space *vm, int sz)
{
	return intel_gt_object_create_lmem(vm->gt, sz,
					   I915_BO_ALLOC_IGNORE_MIN_PAGE_SIZE);
}

struct drm_i915_gem_object *alloc_pt_dma(struct i915_address_space *vm, int sz)
{
	struct drm_i915_gem_object *obj;

	if (I915_SELFTEST_ONLY(should_fail(&vm->fault_attr, 1)))
		i915_gem_shrink_all(vm->i915);

	obj = i915_gem_object_create_internal(vm->i915, sz);
	if (!IS_ERR(obj))
		obj->flags |= I915_BO_ALLOC_CONTIGUOUS;

	return obj;
}

int map_pt_dma(struct i915_address_space *vm, struct drm_i915_gem_object *obj)
{
	enum i915_map_type type = i915_coherent_map_type(vm->i915, obj, true);
	void *vaddr;

	if (unlikely(!i915_gem_object_trylock(obj)))
		return -EBUSY;

	vaddr = i915_gem_object_pin_map(obj, type);
	i915_gem_object_unlock(obj);
	if (IS_ERR(vaddr))
		return PTR_ERR(vaddr);

	if (obj->mm.region.mem) {
		struct intel_memory_region *mem = obj->mm.region.mem;

		spin_lock(&mem->objects.lock);
		list_move(&obj->mm.region.link, &mem->objects.pt);
		spin_unlock(&mem->objects.lock);
	} else {
		i915_gem_object_make_unshrinkable(obj);
	}

	return 0;
}

static void __i915_vm_close(struct i915_address_space *vm)
{
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma, *vn;

	spin_lock(&vm->priv_obj_lock);
	list_for_each_entry(obj, &vm->priv_obj_list, priv_obj_link)
		obj->vm = ERR_PTR(-EACCES);
	spin_unlock(&vm->priv_obj_lock);

	i915_gem_vm_unbind_all(vm);

	mutex_lock(&vm->mutex);
	list_for_each_entry_safe(vma, vn, &vm->bound_list, vm_link)
		i915_vma_unpublish(vma);
	mutex_unlock(&vm->mutex);
}

int i915_vm_lock_objects(const struct i915_address_space *vm,
			 struct i915_gem_ww_ctx *ww)
{
	return i915_gem_object_lock(vm->root_obj, ww);
}

void i915_address_space_fini(struct i915_address_space *vm)
{
	if (vm->client)
		i915_drm_client_put(vm->client);

	i915_active_fini(&vm->active);
	i915_active_fence_fini(&vm->user_fence);

	drm_mm_takedown(&vm->mm);

	if (!i915_is_ggtt(vm) && HAS_UM_QUEUES(vm->i915))
		GEM_WARN_ON(!xa_erase(&vm->i915->asid_resv.xa, vm->asid));

	mutex_destroy(&vm->mutex);
	i915_gem_object_put(vm->root_obj);
	GEM_BUG_ON(!RB_EMPTY_ROOT(&vm->va.rb_root));
	mutex_destroy(&vm->vm_bind_lock);

	iput(vm->inode);
}

static void __i915_vm_release(struct work_struct *work)
{
	struct i915_address_space *vm =
		container_of(work, struct i915_address_space, rcu.work);

	vm->cleanup(vm);
	i915_address_space_fini(vm);

	kfree(vm);
}

void i915_vm_release(struct kref *kref)
{
	struct i915_address_space *vm =
		container_of(kref, struct i915_address_space, ref);

	GEM_BUG_ON(i915_is_ggtt(vm));
	trace_i915_ppgtt_release(vm);

	queue_rcu_work(vm->i915->wq, &vm->rcu);
}

static void i915_vm_close_work(struct work_struct *wrk)
{
	struct i915_address_space *vm =
		container_of(wrk, typeof(*vm), close_work);

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

	INIT_RCU_WORK(&vm->rcu, __i915_vm_release);
	atomic_set(&vm->open, 1);
	INIT_WORK(&vm->close_work, i915_vm_close_work);

	/*
	 * The vm->mutex must be reclaim safe (for use in the shrinker).
	 * Do a dummy acquire now under fs_reclaim so that any allocation
	 * attempt holding the lock is immediately reported by lockdep.
	 */
	mutex_init(&vm->mutex);
	lockdep_set_subclass(&vm->mutex, subclass);
	fs_reclaim_taints_mutex(&vm->mutex);

	vm->inode = alloc_anon_inode(vm->i915->drm.anon_inode->i_sb);
	if (IS_ERR(vm->inode))
		return PTR_ERR(vm->inode);
	i_size_write(vm->inode, vm->total);

	min_alignment = I915_GTT_MIN_ALIGNMENT;
	if (subclass == VM_CLASS_GGTT &&
	    intel_ggtt_needs_same_mem_type_within_cl_wa(vm->i915)) {
		min_alignment = I915_GTT_PAGE_SIZE_64K;
	}

	memset64(vm->min_alignment, min_alignment,
		 ARRAY_SIZE(vm->min_alignment));

	if (HAS_64K_PAGES(vm->i915)) {
		vm->min_alignment[INTEL_MEMORY_LOCAL] = I915_GTT_PAGE_SIZE_64K;
		vm->min_alignment[INTEL_MEMORY_STOLEN] = I915_GTT_PAGE_SIZE_64K;
	}

	vm->fault_start = U64_MAX;
	vm->fault_end = 0;

	drm_mm_init(&vm->mm, 0, vm->total);

	vm->mm.head_node.color = I915_COLOR_UNEVICTABLE;

	INIT_LIST_HEAD(&vm->bound_list);

	vm->va = RB_ROOT_CACHED;
	INIT_LIST_HEAD(&vm->vm_bind_list);
	INIT_LIST_HEAD(&vm->vm_bound_list);
	mutex_init(&vm->vm_bind_lock);

	vm->root_obj = i915_gem_object_create_internal(vm->i915, PAGE_SIZE);
	if (IS_ERR_OR_NULL(vm->root_obj))
		return -ENOMEM;

	spin_lock_init(&vm->priv_obj_lock);
	INIT_LIST_HEAD(&vm->priv_obj_list);
	INIT_LIST_HEAD(&vm->vm_capture_list);
	spin_lock_init(&vm->vm_capture_lock);
	INIT_ACTIVE_FENCE(&vm->user_fence);

	vm->has_scratch = true;

	i915_active_init(&vm->active, __i915_vm_active, __i915_vm_retire, 0);

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

	return 0;
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
	memset64(__px_vaddr(p), val, count);
}

u64 i915_vm_scratch_encode(struct i915_address_space *vm, int lvl)
{
	/*
	 * Irrespective of vm->has_scratch, for systems with recoverable
	 * pagefaults enabled, we should not map the entire address space to
	 * valid scratch while initializing the vm. Doing so, would  prevent from
	 * generating any faults at all. On such platforms, mapping to scratch
	 * page is handled in the page fault handler itself.
	 */
	if (!vm->has_scratch || i915_vm_page_fault_enabled(vm))
		return PTE_NULL_PAGE;

	switch (lvl) {
	default: return gen8_pde_encode(px_dma(vm->scratch[lvl]), I915_CACHE_NONE);
	case 2:  return PTE_NULL_PAGE | GEN8_PAGE_PRESENT | GEN8_PDPE_PS_1G;
	case 1:  return PTE_NULL_PAGE | GEN8_PAGE_PRESENT | GEN8_PDE_PS_2M;
	case 0:  return PTE_NULL_PAGE | GEN8_PAGE_PRESENT;
	}
}

void i915_vm_free_scratch(struct i915_address_space *vm)
{
	int i;

	for (i = 0; i <= vm->top; i++) {
		if (vm->scratch[i]) {
			i915_gem_object_put(vm->scratch[i]);
			vm->scratch[i] = NULL;
		}
	}
}

static void xelpmp_setup_private_ppat(struct intel_uncore *uncore)
{
	intel_uncore_write(uncore, XELPMP_PAT_INDEX(0), MTL_PPAT_L4_0_WB);
	intel_uncore_write(uncore, XELPMP_PAT_INDEX(1), MTL_PPAT_L4_1_WT);
	intel_uncore_write(uncore, XELPMP_PAT_INDEX(2), MTL_PPAT_L4_3_UC);
	intel_uncore_write(uncore, XELPMP_PAT_INDEX(3),
			   MTL_PPAT_L4_0_WB | MTL_2_COH_1W);
	intel_uncore_write(uncore, XELPMP_PAT_INDEX(4),
			   MTL_PPAT_L4_0_WB | MTL_3_COH_2W);

	/*
	 * Remaining PAT entries are left at the hardware-default
	 * fully-cached setting
	 */

}

static void xelpg_setup_private_ppat(struct intel_gt *gt)
{
	intel_gt_mcr_multicast_write(gt, XEHP_PAT_INDEX(0),
				     MTL_PPAT_L4_0_WB);
	intel_gt_mcr_multicast_write(gt, XEHP_PAT_INDEX(1),
				     MTL_PPAT_L4_1_WT);
	intel_gt_mcr_multicast_write(gt, XEHP_PAT_INDEX(2),
				     MTL_PPAT_L4_3_UC);
	intel_gt_mcr_multicast_write(gt, XEHP_PAT_INDEX(3),
				     MTL_PPAT_L4_0_WB | MTL_2_COH_1W);
	intel_gt_mcr_multicast_write(gt, XEHP_PAT_INDEX(4),
				     MTL_PPAT_L4_0_WB | MTL_3_COH_2W);

	/*
	 * Remaining PAT entries are left at the hardware-default
	 * fully-cached setting
	 */
}

static void pvc_setup_private_ppat(struct intel_gt *gt)
{
        intel_gt_mcr_multicast_write(gt, XEHP_PAT_INDEX(0), GEN8_PPAT_UC);
        intel_gt_mcr_multicast_write(gt, XEHP_PAT_INDEX(1), GEN8_PPAT_WC);
        intel_gt_mcr_multicast_write(gt, XEHP_PAT_INDEX(2), GEN8_PPAT_WT);
        intel_gt_mcr_multicast_write(gt, XEHP_PAT_INDEX(3), GEN8_PPAT_WB);
        intel_gt_mcr_multicast_write(gt, XEHP_PAT_INDEX(4),
				     GEN12_PPAT_CLOS(1) | GEN8_PPAT_WT);
        intel_gt_mcr_multicast_write(gt, XEHP_PAT_INDEX(5),
				     GEN12_PPAT_CLOS(1) | GEN8_PPAT_WB);
        intel_gt_mcr_multicast_write(gt, XEHP_PAT_INDEX(6),
				     GEN12_PPAT_CLOS(2) | GEN8_PPAT_WT);
        intel_gt_mcr_multicast_write(gt, XEHP_PAT_INDEX(7),
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

static void xehp_setup_private_ppat(struct intel_gt *gt)
{
	enum forcewake_domains fw;
	unsigned long flags;

	fw = intel_uncore_forcewake_for_reg(gt->uncore, _MMIO(XEHP_PAT_INDEX(0).reg),
					    FW_REG_WRITE);
	intel_uncore_forcewake_get(gt->uncore, fw);

	intel_gt_mcr_lock(gt, &flags);
	intel_gt_mcr_multicast_write_fw(gt, XEHP_PAT_INDEX(0), GEN8_PPAT_WB);
	intel_gt_mcr_multicast_write_fw(gt, XEHP_PAT_INDEX(1), GEN8_PPAT_WC);
	intel_gt_mcr_multicast_write_fw(gt, XEHP_PAT_INDEX(2), GEN8_PPAT_WT);
	intel_gt_mcr_multicast_write_fw(gt, XEHP_PAT_INDEX(3), GEN8_PPAT_UC);
	intel_gt_mcr_multicast_write_fw(gt, XEHP_PAT_INDEX(4), GEN8_PPAT_WB);
	intel_gt_mcr_multicast_write_fw(gt, XEHP_PAT_INDEX(5), GEN8_PPAT_WB);
	intel_gt_mcr_multicast_write_fw(gt, XEHP_PAT_INDEX(6), GEN8_PPAT_WB);
	intel_gt_mcr_multicast_write_fw(gt, XEHP_PAT_INDEX(7), GEN8_PPAT_WB);
	intel_gt_mcr_unlock(gt, flags);

	intel_uncore_forcewake_put(gt->uncore, fw);
}

void setup_private_pat(struct intel_gt *gt)
{
	struct intel_uncore *uncore = gt->uncore;
	struct drm_i915_private *i915 = gt->i915;

	GEM_BUG_ON(GRAPHICS_VER(i915) < 8);

	if (IS_SRIOV_VF(i915))
		return;

	if (gt->type == GT_MEDIA) {
		xelpmp_setup_private_ppat(gt->uncore);
	} else {
		if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 70))
			xelpg_setup_private_ppat(gt);
		else if (IS_PONTEVECCHIO(i915))
			pvc_setup_private_ppat(gt);
		else if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 50))
			xehp_setup_private_ppat(gt);
		else
			tgl_setup_private_ppat(uncore);
	}
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
#endif
