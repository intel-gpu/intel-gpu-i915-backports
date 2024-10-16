// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

#include <linux/pgtable.h>
#include <asm/set_memory.h>
#include <asm/smp.h>
#include <linux/types.h>
#include <linux/stop_machine.h>

#include <drm/i915_drm.h>
#include <drm/intel-gtt.h>

#include "gem/i915_gem_lmem.h"

#include "iov/abi/iov_actions_prelim_abi.h"
#include "iov/intel_iov.h"
#include "iov/intel_iov_relay.h"
#include "iov/intel_iov_utils.h"

#include "intel_gpu_commands.h"
#include "intel_gt.h"
#include "intel_gt_pm.h"
#include "intel_gt_print.h"
#include "intel_gt_regs.h"
#include "intel_gtt.h"
#include "intel_ring.h"
#include "intel_pci_config.h"
#include "i915_drv.h"
#include "i915_pci.h"
#include "i915_scatterlist.h"
#include "i915_utils.h"
#include "gen8_ppgtt.h"

static int
i915_get_ggtt_vma_pages(struct i915_vma *vma);

/**
 * i915_ggtt_suspend_vm - Suspend the memory mappings for a GGTT or DPT VM
 * @vm: The VM to suspend the mappings for
 *
 * Suspend the memory mappings for all objects mapped to HW via the GGTT or a
 * DPT page table.
 */
void i915_ggtt_suspend_vm(struct i915_address_space *vm)
{
	struct i915_vma *vma, *vn;
	int open;

	drm_WARN_ON(&vm->i915->drm, !vm->is_ggtt && !vm->is_dpt);

	mutex_lock(&vm->mutex);

	/* Skip rewriting PTE on VMA unbind. */
	open = atomic_xchg(&vm->open, 0);

	list_for_each_entry_safe(vma, vn, &vm->bound_list, vm_link) {
		GEM_BUG_ON(!drm_mm_node_allocated(&vma->node));
		i915_vma_wait_for_bind(vma);

		if (i915_vma_is_pinned(vma))
			continue;

		if (!i915_vma_is_bound(vma, I915_VMA_GLOBAL_BIND)) {
			__i915_vma_evict(vma);
			drm_mm_remove_node(&vma->node);
		}
	}

	vm->clear_range(vm, 0, vm->total);

	atomic_set(&vm->open, open);

	mutex_unlock(&vm->mutex);
}

void i915_ggtt_suspend(struct i915_ggtt *ggtt)
{
	struct intel_gt *gt;

	i915_ggtt_suspend_vm(&ggtt->vm);
	ggtt->invalidate(ggtt);

	list_for_each_entry(gt, &ggtt->gt_list, ggtt_link)
		intel_gt_check_and_clear_faults(gt);
}

static void gen8_ggtt_invalidate(struct i915_ggtt *ggtt)
{
	wmb();
}

static void guc_ggtt_ct_invalidate(struct i915_ggtt *ggtt)
{
	struct intel_gt *gt = ggtt->vm.gt;
	struct intel_uncore *uncore = gt->uncore;
	struct intel_guc *guc = &gt->uc.guc;
	int err = -ENODEV;

	if (guc->ct.enabled)
		err = intel_guc_invalidate_tlb_guc(guc, INTEL_GUC_TLB_INVAL_MODE_HEAVY);

	if (err) {
		intel_uncore_write_fw(uncore, PVC_GUC_TLB_INV_DESC1,
				PVC_GUC_TLB_INV_DESC1_INVALIDATE);
		intel_uncore_write_fw(uncore, PVC_GUC_TLB_INV_DESC0,
				PVC_GUC_TLB_INV_DESC0_VALID);
	}
}

static void guc_ggtt_invalidate(struct i915_ggtt *ggtt)
{
	struct drm_i915_private *i915 = ggtt->vm.i915;

	gen8_ggtt_invalidate(ggtt);

	if (HAS_ASID_TLB_INVALIDATION(i915)) {
		guc_ggtt_ct_invalidate(ggtt);
	} else if (GRAPHICS_VER(i915) >= 12) {
		struct intel_gt *gt;

		list_for_each_entry(gt, &ggtt->gt_list, ggtt_link)
			intel_uncore_write_fw(gt->uncore,
					      GEN12_GUC_TLB_INV_CR,
					      GEN12_GUC_TLB_INV_CR_INVALIDATE);
	} else {
		intel_uncore_write_fw(ggtt->vm.gt->uncore,
				      GEN8_GTCR, GEN8_GTCR_INVALIDATE);
	}
}

static void gen12vf_ggtt_invalidate(struct i915_ggtt *ggtt)
{
	struct intel_gt *gt;

	list_for_each_entry(gt, &ggtt->gt_list, ggtt_link) {
		struct intel_guc *guc = &gt->uc.guc;
		intel_wakeref_t wakeref;

		if (!guc->ct.enabled)
			continue;
		with_intel_runtime_pm(gt->uncore->rpm, wakeref)
			intel_guc_invalidate_tlb_guc(guc, INTEL_GUC_TLB_INVAL_MODE_HEAVY);
	}
}

static u64 mtl_ggtt_pte_encode(dma_addr_t addr,
			       unsigned int pat_index,
			       u32 flags)
{
	gen8_pte_t pte = addr | GEN8_PAGE_PRESENT;

	GEM_BUG_ON(addr & ~GEN12_GGTT_PTE_ADDR_MASK);

	if (flags & PTE_LM)
		pte |= GEN12_GGTT_PTE_LM;

	if (pat_index & 1)
		pte |= MTL_GGTT_PTE_PAT0;

	if ((pat_index >> 1) & 1)
		pte |= MTL_GGTT_PTE_PAT1;

	return pte;
}

static u64 gen8_ggtt_pte_encode(dma_addr_t addr,
				unsigned int pat_index,
				u32 flags)
{
	gen8_pte_t pte = addr | GEN8_PAGE_PRESENT;

	GEM_BUG_ON(addr & ~GEN12_GGTT_PTE_ADDR_MASK);

	if (flags & PTE_LM)
		pte |= GEN12_GGTT_PTE_LM;

	return pte;
}

void gen8_set_pte(void __iomem *addr, gen8_pte_t pte)
{
	writeq(pte, addr);
}

gen8_pte_t gen8_get_pte(void __iomem *addr)
{
	return readq(addr);
}

u64 ggtt_addr_to_pte_offset(u64 ggtt_addr)
{
	GEM_BUG_ON(!IS_ALIGNED(ggtt_addr, I915_GTT_PAGE_SIZE_4K));

	return (ggtt_addr / I915_GTT_PAGE_SIZE_4K) * sizeof(gen8_pte_t);
}

static gen8_pte_t __iomem *gsm_base(struct i915_ggtt *ggtt)
{
	/*
	 * We need both the device to be awake and for PVC to be out of rc6;
	 * GT pm ensures both. Alternatively we could use runtime pm plus
	 * forcewake. However, as all users are generally talking to the GT
	 * when updating the GGTT on that tile, we are, or soon will be,
	 * holding the full GT pm.
	 */
	assert_gt_pm_held(ggtt->vm.gt);
	return (gen8_pte_t __iomem *)ggtt->gsm;
}

static void gen8_ggtt_insert_page(struct i915_address_space *vm,
				  dma_addr_t addr,
				  u64 offset,
				  unsigned int pat_index,
				  u32 flags)
{
	struct i915_ggtt *ggtt = i915_vm_to_ggtt(vm);
	gen8_pte_t __iomem *pte = gsm_base(ggtt) + offset / I915_GTT_PAGE_SIZE;

	gen8_set_pte(pte, ggtt->vm.pte_encode(addr, pat_index, flags));

	ggtt->invalidate(ggtt);
}

static int gen8_ggtt_insert_entries(struct i915_address_space *vm,
				    struct i915_vma *vma,
				    struct i915_gem_ww_ctx *ww,
				    unsigned int pat_index,
				    u32 flags)
{
	struct i915_ggtt *ggtt = i915_vm_to_ggtt(vm);
	gen8_pte_t __iomem *gte;
	gen8_pte_t __iomem *end;
	gen8_pte_t pte_encode;
	struct sgt_iter iter;
	dma_addr_t addr;

	pte_encode = ggtt->vm.pte_encode(0, pat_index, flags);

	/*
	 * Note that we ignore PTE_READ_ONLY here. The caller must be careful
	 * not to allow the user to override access to a read only page.
	 */
	gte = gsm_base(ggtt) + vma->node.start / I915_GTT_PAGE_SIZE;

	end = gte + vma->guard / I915_GTT_PAGE_SIZE;
	while (gte < end)
		gen8_set_pte(gte++, i915_vm_ggtt_scratch0_encode(vm));

	end += (vma->node.size - vma->guard) / I915_GTT_PAGE_SIZE;
	for_each_sgt_daddr(addr, iter, vma->pages)
		gen8_set_pte(gte++, pte_encode | addr);
	GEM_BUG_ON(gte > end);

	/* Fill the allocated but "unused" space beyond the end of the buffer */
	while (gte < end)
		gen8_set_pte(gte++, i915_vm_ggtt_scratch0_encode(vm));

	/*
	 * We want to flush the TLBs only after we're certain all the PTE
	 * updates have finished.
	 */
	ggtt->invalidate(ggtt);
	return 0;
}

static void gen8_ggtt_clear_range(struct i915_address_space *vm,
				  u64 start, u64 length)
{
	struct i915_ggtt *ggtt = i915_vm_to_ggtt(vm);
	unsigned long first_entry = start / I915_GTT_PAGE_SIZE;
	unsigned long num_entries = length / I915_GTT_PAGE_SIZE;
	gen8_pte_t __iomem *pte = gsm_base(ggtt) + first_entry;
	const gen8_pte_t scratch = i915_vm_ggtt_scratch0_encode(vm);

	while (num_entries--)
		iowrite32(scratch, pte++);
}

static void nop_clear_range(struct i915_address_space *vm,
			    u64 start, u64 length)
{
}

int intel_ggtt_bind_vma(struct i915_address_space *vm,
			struct i915_vma *vma,
			struct i915_gem_ww_ctx *ww,
			unsigned int pat_index,
			u32 flags)
{
	struct drm_i915_gem_object *obj = vma->obj;
	u32 pte_flags;

	if (i915_vma_is_bound(vma, ~flags & I915_VMA_BIND_MASK))
		return 0;

	/* Applicable to VLV (gen8+ do not support RO in the GGTT) */
	pte_flags = 0;
	if (i915_gem_object_is_readonly(obj))
		pte_flags |= PTE_READ_ONLY;
	if (i915_gem_object_is_lmem(obj) || i915_gem_object_has_fabric(obj))
		pte_flags |= PTE_LM;

	vm->insert_entries(vm, vma, ww, pat_index, pte_flags);
	return 0;
}

void intel_ggtt_unbind_vma(struct i915_address_space *vm, struct i915_vma *vma)
{
	vm->clear_range(vm, vma->node.start, vma->size);
}

static int ggtt_reserve_guc_top(struct i915_ggtt *ggtt)
{
	if (!intel_uc_uses_guc(&ggtt->vm.gt->uc))
		return 0;

	GEM_BUG_ON(ggtt->vm.total <= GUC_GGTT_TOP);
	return i915_ggtt_balloon(ggtt, GUC_GGTT_TOP, ggtt->vm.total,
				 &ggtt->uc_fw);
}

/**
 * i915_ggtt_address_lock_init - initialize the SRCU for GGTT address computation lock
 * @i915: i915 device instance struct
 */
void i915_ggtt_address_lock_init(struct i915_ggtt *ggtt)
{
	init_waitqueue_head(&ggtt->queue);
	init_srcu_struct(&ggtt->blocked_srcu);
}

/**
 * i915_ggtt_address_lock_fini - finalize the SRCU for GGTT address computation lock
 * @i915: i915 device instance struct
 */
void i915_ggtt_address_lock_fini(struct i915_ggtt *ggtt)
{
	cleanup_srcu_struct(&ggtt->blocked_srcu);
}

static void ggtt_release_guc_top(struct i915_ggtt *ggtt)
{
	i915_ggtt_deballoon(ggtt, &ggtt->uc_fw);
}

static void cleanup_init_ggtt(struct i915_ggtt *ggtt)
{
	ggtt_release_guc_top(ggtt);
	i915_ggtt_address_lock_fini(ggtt);
}

static void ggtt_address_write_lock(struct i915_ggtt *ggtt)
{
	/*
	 * We are just setting the bit, without the usual checks whether it is
	 * already set. Such checks are unneccessary if the blocked code is
	 * running in a worker and the caller function just schedules it.
	 * But the worker must be aware of re-schedules and know when to skip
	 * finishing the locking.
	 */
	set_bit(GGTT_ADDRESS_COMPUTE_BLOCKED, &ggtt->flags);
	wake_up_all(&ggtt->queue);
	/*
	 * After switching our GGTT_ADDRESS_COMPUTE_BLOCKED bit, we should wait for
	 * all related critical sections to finish. First make sure any read-side
	 * locking currently in progress either got the lock or noticed the BLOCKED
	 * flag and is waiting for it to clear. Then wait for all read-side unlocks.
	 */
	synchronize_rcu_expedited();
	synchronize_srcu(&ggtt->blocked_srcu);
}

static void ggtt_address_write_unlock(struct i915_ggtt *ggtt)
{
	clear_bit_unlock(GGTT_ADDRESS_COMPUTE_BLOCKED, &ggtt->flags);
	smp_mb__after_atomic();
	wake_up_all(&ggtt->queue);
}

/**
 * i915_ggtt_address_write_lock - enter the ggtt address computation fixups section
 * @i915: i915 device instance struct
 */
void i915_ggtt_address_write_lock(struct drm_i915_private *i915)
{
	ggtt_address_write_lock(to_gt(i915)->ggtt);
}

static int ggtt_address_read_lock_sync(struct i915_ggtt *ggtt, int *srcu)
__acquires(&ggtt->blocked_srcu)
{
	might_sleep();

	rcu_read_lock();
	while (test_bit(GGTT_ADDRESS_COMPUTE_BLOCKED, &ggtt->flags)) {
		rcu_read_unlock();

		if (wait_event_interruptible(ggtt->queue,
					     !test_bit(GGTT_ADDRESS_COMPUTE_BLOCKED,
						       &ggtt->flags)))
			return -EINTR;

		rcu_read_lock();
	}
	*srcu = __srcu_read_lock(&ggtt->blocked_srcu);
	rcu_read_unlock();

	return 0;
}

static int ggtt_address_read_lock_interruptible(struct i915_ggtt *ggtt, int *srcu)
__acquires(&ggtt->blocked_srcu)
{
	rcu_read_lock();
	while (test_bit(GGTT_ADDRESS_COMPUTE_BLOCKED, &ggtt->flags)) {
		rcu_read_unlock();

		cpu_relax();
		if (signal_pending(current))
			return -EINTR;

		rcu_read_lock();
	}
	*srcu = __srcu_read_lock(&ggtt->blocked_srcu);
	rcu_read_unlock();

	return 0;
}

static void ggtt_address_read_lock(struct i915_ggtt *ggtt, int *srcu)
__acquires(&ggtt->blocked_srcu)
{
	rcu_read_lock();
	while (test_bit(GGTT_ADDRESS_COMPUTE_BLOCKED, &ggtt->flags))
		cpu_relax();
	*srcu = __srcu_read_lock(&ggtt->blocked_srcu);
	rcu_read_unlock();
}

int gt_ggtt_address_read_lock_sync(struct intel_gt *gt, int *srcu)
{
	return ggtt_address_read_lock_sync(gt->ggtt, srcu);
}

int gt_ggtt_address_read_lock_interruptible(struct intel_gt *gt, int *srcu)
{
	return ggtt_address_read_lock_interruptible(gt->ggtt, srcu);
}

void gt_ggtt_address_read_lock(struct intel_gt *gt, int *srcu)
{
	ggtt_address_read_lock(gt->ggtt, srcu);
}

static void ggtt_address_read_unlock(struct i915_ggtt *ggtt, int tag)
__releases(&ggtt->blocked_srcu)
{
	__srcu_read_unlock(&ggtt->blocked_srcu, tag);
}

void gt_ggtt_address_read_unlock(struct intel_gt *gt, int srcu)
{
	ggtt_address_read_unlock(gt->ggtt, srcu);
}

/**
 * i915_ggtt_address_write_unlock - finish the ggtt address computation fixups section
 * @i915: i915 device instance struct
 */
void i915_ggtt_address_write_unlock(struct drm_i915_private *i915)
{
	ggtt_address_write_unlock(to_gt(i915)->ggtt);
}

static int init_ggtt(struct i915_ggtt *ggtt)
{
	/*
	 * Let GEM Manage all of the aperture.
	 *
	 * However, leave one page at the end still bound to the scratch page.
	 * There are a number of places where the hardware apparently prefetches
	 * past the end of the object, and we've seen multiple hangs with the
	 * GPU head pointer stuck in a batchbuffer bound at the last page of the
	 * aperture.  One page should be enough to keep any prefetching inside
	 * of the aperture.
	 */
	unsigned long hole_start, hole_end;
	struct drm_mm_node *entry;
	int ret;

	/*
	 * GuC requires all resources that we're sharing with it to be placed in
	 * non-WOPCM memory. If GuC is not present or not in use we still need a
	 * small bias as ring wraparound at offset 0 sometimes hangs. No idea
	 * why.
	 */
	ggtt->pin_bias = max_t(u32, I915_GTT_PAGE_SIZE,
			       intel_wopcm_guc_size(&ggtt->vm.gt->wopcm));

	i915_ggtt_address_lock_init(ggtt);

	ret = intel_iov_init_ggtt(&ggtt->vm.gt->iov);
	if (ret)
		return ret;

	/*
	 * The upper portion of the GuC address space has a sizeable hole
	 * (several MB) that is inaccessible by GuC. Reserve this range within
	 * GGTT as it can comfortably hold GuC/HuC firmware images.
	 */
	ret = ggtt_reserve_guc_top(ggtt);
	if (ret)
		goto err;

	/* Clear any non-preallocated blocks */
	drm_mm_for_each_hole(entry, &ggtt->vm.mm, hole_start, hole_end) {
		drm_dbg(&ggtt->vm.i915->drm,
			"clearing unused GTT space: [%lx, %lx]\n",
			hole_start, hole_end);
		ggtt->vm.clear_range(&ggtt->vm, hole_start,
				     hole_end - hole_start);
	}

	/* And finally clear the reserved guard page */
	ggtt->vm.clear_range(&ggtt->vm, ggtt->vm.total - PAGE_SIZE, PAGE_SIZE);

	return 0;

err:
	cleanup_init_ggtt(ggtt);
	return ret;
}

int i915_init_ggtt(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int i, j;
	int ret;

	for_each_gt(gt, i915, i) {
		/*
		 * Media GT shares primary GT's GGTT which is already
		 * initialized
		 */
		if (gt->type == GT_MEDIA) {
			drm_WARN_ON(&i915->drm, gt->ggtt != to_gt(i915)->ggtt);
			continue;
		}
		ret = init_ggtt(gt->ggtt);
		if (ret)
			goto err;
	}

	return 0;

err:
	for_each_gt(gt, i915, j) {
		if (j == i)
			break;
		cleanup_init_ggtt(gt->ggtt);
	}

	return ret;
}

static void ggtt_cleanup_hw(struct i915_ggtt *ggtt)
{
	struct i915_vma *vma, *vn;

	atomic_set(&ggtt->vm.open, 0);

	rcu_barrier(); /* flush the RCU'ed__i915_vm_release */
	if (ggtt->vm.gt->wq)
		flush_workqueue(ggtt->vm.gt->wq);

	mutex_lock(&ggtt->vm.mutex);

	list_for_each_entry_safe(vma, vn, &ggtt->vm.bound_list, vm_link)
		WARN_ON_ONCE(__i915_vma_unbind(vma));

	ggtt_release_guc_top(ggtt);
	intel_iov_fini_ggtt(&ggtt->vm.gt->iov);

	ggtt->vm.cleanup(&ggtt->vm);

	mutex_unlock(&ggtt->vm.mutex);
	i915_address_space_fini(&ggtt->vm);
}

/**
 * i915_ggtt_driver_release - Clean up GGTT hardware initialization
 * @i915: i915 device
 */
void i915_ggtt_driver_release(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int i;

	for_each_gt(gt, i915, i) {
		if (gt->type == GT_MEDIA)
			continue;

		ggtt_cleanup_hw(gt->ggtt);
	}
}

/**
 * i915_ggtt_driver_late_release - Cleanup of GGTT that needs to be done after
 * all free objects have been drained.
 * @i915: i915 device
 */
void i915_ggtt_driver_late_release(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int i;

	for_each_gt(gt, i915, i) {
		struct i915_ggtt *ggtt = gt->ggtt;

		if (gt->type == GT_MEDIA)
			continue;

		kfree(ggtt);
	}
}

static unsigned int gen8_get_total_gtt_size(u16 bdw_gmch_ctl)
{
	bdw_gmch_ctl >>= BDW_GMCH_GGMS_SHIFT;
	bdw_gmch_ctl &= BDW_GMCH_GGMS_MASK;
	if (bdw_gmch_ctl)
		bdw_gmch_ctl = 1 << bdw_gmch_ctl;

#ifdef CONFIG_X86_32
	/* Limit 32b platforms to a 2GB GGTT: 4 << 20 / pte size * I915_GTT_PAGE_SIZE */
	if (bdw_gmch_ctl > 4)
		bdw_gmch_ctl = 4;
#endif

	return bdw_gmch_ctl << 20;
}

static unsigned int gen8_gttadr_offset(void)
{
	return SZ_16M / 2;
}

static int ggtt_probe_common(struct i915_ggtt *ggtt, u64 size)
{
	struct drm_i915_private *i915 = ggtt->vm.i915;
	phys_addr_t phys_addr;
	int ret;

	ret = i915_address_space_init(&ggtt->vm, VM_CLASS_GGTT);
	if (ret)
		return ret;

	phys_addr = ggtt->vm.gt->phys_addr + gen8_gttadr_offset();

	ggtt->gsm = ioremap(phys_addr, size);
	if (!ggtt->gsm) {
		gt_err(ggtt->vm.gt, "Failed to map the ggtt page table\n");
		return -ENOMEM;
	}

	if (ggtt->vm.scratch[0] && i915_gem_object_is_lmem(ggtt->vm.scratch[0]))
		/* we rely on scratch in SMEM to clean stale LMEM for the WA */
		GEM_DEBUG_WARN_ON(intel_ggtt_needs_same_mem_type_within_cl_wa(i915));

	return 0;
}

int ggtt_set_pages(struct i915_vma *vma)
{
	GEM_BUG_ON(vma->pages);
	return i915_get_ggtt_vma_pages(vma);
}

void ggtt_clear_pages(struct i915_vma *vma)
{
	GEM_BUG_ON(!vma->pages);

	if (test_and_clear_bit(I915_VMA_PARTIAL_BIT, __i915_vma_flags(vma)))
		sg_table_inline_free(vma->pages);
	vma->pages = NULL;
}

static void gen6_gmch_remove(struct i915_address_space *vm)
{
	struct i915_ggtt *ggtt = i915_vm_to_ggtt(vm);

	iounmap(ggtt->gsm);
	i915_vm_free_scratch(vm);
}

static int gen8_gmch_probe(struct i915_ggtt *ggtt)
{
	struct drm_i915_private *i915 = ggtt->vm.i915;
	struct pci_dev *pdev = to_pci_dev(i915->drm.dev);
	unsigned int size;
	u16 snb_gmch_ctl;

	pci_read_config_word(pdev, SNB_GMCH_CTRL, &snb_gmch_ctl);
	size = gen8_get_total_gtt_size(snb_gmch_ctl);

	ggtt->vm.alloc_pt_dma = alloc_pt_dma;
	ggtt->vm.alloc_scratch_dma = alloc_pt_dma;

	ggtt->vm.total = (size / sizeof(gen8_pte_t)) * I915_GTT_PAGE_SIZE;
	ggtt->vm.cleanup = gen6_gmch_remove;
	ggtt->vm.clear_range = nop_clear_range;
	ggtt->vm.scratch_range = gen8_ggtt_clear_range;
	ggtt->vm.insert_entries = gen8_ggtt_insert_entries;
	ggtt->vm.insert_page = gen8_ggtt_insert_page;

	if (intel_uc_wants_guc(&ggtt->vm.gt->uc))
		ggtt->invalidate = guc_ggtt_invalidate;
	else
		ggtt->invalidate = gen8_ggtt_invalidate;

	ggtt->vm.vma_ops.bind_vma    = intel_ggtt_bind_vma;
	ggtt->vm.vma_ops.unbind_vma  = intel_ggtt_unbind_vma;
	ggtt->vm.vma_ops.set_pages   = ggtt_set_pages;
	ggtt->vm.vma_ops.clear_pages = ggtt_clear_pages;

	if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 70))
		ggtt->vm.pte_encode = mtl_ggtt_pte_encode;
	else
		ggtt->vm.pte_encode = gen8_ggtt_pte_encode;

	return ggtt_probe_common(ggtt, size);
}

static int gen12vf_ggtt_probe(struct i915_ggtt *ggtt)
{
	struct drm_i915_private *i915 = ggtt->vm.i915;

	GEM_BUG_ON(!IS_SRIOV_VF(i915));
	GEM_BUG_ON(GRAPHICS_VER(i915) < 12);

	ggtt->vm.alloc_pt_dma = alloc_pt_dma;
	ggtt->vm.alloc_scratch_dma = alloc_pt_dma;

	/* safe guess as native expects the same minimum */
	ggtt->vm.total = 1ULL << (ilog2(GUC_GGTT_TOP - 1) + 1); /* roundup_pow_of_two(GUC_GGTT_TOP); */

	if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 70))
		ggtt->vm.pte_encode = mtl_ggtt_pte_encode;
	else
		ggtt->vm.pte_encode = gen8_ggtt_pte_encode;
	ggtt->vm.clear_range = nop_clear_range;
	ggtt->vm.insert_page = gen8_ggtt_insert_page;
	ggtt->vm.insert_entries = gen8_ggtt_insert_entries;
	ggtt->vm.cleanup = gen6_gmch_remove;

	ggtt->vm.vma_ops.bind_vma    = intel_ggtt_bind_vma;
	ggtt->vm.vma_ops.unbind_vma  = intel_ggtt_unbind_vma;
	ggtt->vm.vma_ops.set_pages   = ggtt_set_pages;
	ggtt->vm.vma_ops.clear_pages = ggtt_clear_pages;

	ggtt->invalidate = gen12vf_ggtt_invalidate;

	return ggtt_probe_common(ggtt, sizeof(gen8_pte_t) *
				 (ggtt->vm.total >> PAGE_SHIFT));
}

static int ggtt_probe_hw(struct i915_ggtt *ggtt, struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;
	int ret;

	ggtt->vm.is_ggtt = true;
	ggtt->vm.gt = gt;
	ggtt->vm.i915 = i915;
	ggtt->vm.dma = i915->drm.dev;

	if (IS_SRIOV_VF(i915))
		ret = gen12vf_ggtt_probe(ggtt);
	else
		ret = gen8_gmch_probe(ggtt);
	if (ret)
		return ret;

	if ((ggtt->vm.total - 1) >> 32) {
		gt_warn(gt,
			"We never expected a Global GTT with more than 32bits"
			" of address space! Found %lldM!\n",
			ggtt->vm.total >> 20);
		ggtt->vm.total = 1ULL << 32;
	}

	/*
	 * GMADR is the PCI mmio aperture into the global GTT. Likely only
	 * availale for non-local memory, 0-remote-tiled hw. Anyway this will
	 * be initialized at least once as tile0.
	 */
	drm_dbg(&i915->drm, "GGTT size = %lluM\n", ggtt->vm.total >> 20);
	drm_dbg(&i915->drm, "DSM size = %lluM\n",
		(u64)resource_size(&intel_graphics_stolen_res) >> 20);
	INIT_LIST_HEAD(&ggtt->gt_list);
	return 0;
}

/**
 * i915_ggtt_probe_hw - Probe GGTT hardware location
 * @i915: i915 device
 */
int i915_ggtt_probe_hw(struct drm_i915_private *i915)
{
	struct i915_ggtt *ggtt;
	struct intel_gt *gt;
	unsigned int i;
	int ret;

	for_each_gt(gt, i915, i) {
		ggtt = gt->ggtt;

		/*
		 * Media GT shares primary GT's GGTT
		 */
		if (gt->type == GT_MEDIA) {
			ggtt = to_gt(i915)->ggtt;
			intel_gt_init_ggtt(gt, ggtt);
			continue;
		}

		if (!ggtt)
			ggtt = kzalloc(sizeof(*ggtt), GFP_KERNEL);

		if (!ggtt) {
			ret = -ENOMEM;
			goto err;
		}

		ret = ggtt_probe_hw(ggtt, gt);
		if (ret) {
			if (ggtt != gt->ggtt)
				kfree(ggtt);
			goto err;
		}

		intel_gt_init_ggtt(gt, ggtt);
	}

	if (i915_vtd_active(i915))
		dev_info(i915->drm.dev, "VT-d active for gfx access\n");

	return 0;

err:
	for_each_gt(gt, i915, i) {
		if (gt->type == GT_MEDIA)
			continue;

		kfree(gt->ggtt);
	}

	return ret;
}

/**
 * i915_ggtt_resume_vm - Restore the memory mappings for a GGTT or DPT VM
 * @vm: The VM to restore the mappings for
 *
 * Restore the memory mappings for all objects mapped to HW via the GGTT or a
 * DPT page table.
 */
void i915_ggtt_resume_vm(struct i915_address_space *vm)
{
	struct i915_vma *vma;
	int open;

	GEM_BUG_ON(!vm->is_ggtt && !vm->is_dpt);

	/* First fill our portion of the GTT with scratch pages */
	vm->clear_range(vm, 0, vm->total);

	/* Skip rewriting PTE on VMA unbind. */
	open = atomic_xchg(&vm->open, 0);

	list_for_each_entry(vma, &vm->bound_list, vm_link) {
		struct drm_i915_gem_object *obj = vma->obj;
		unsigned int was_bound =
			atomic_read(&vma->flags) & I915_VMA_BIND_MASK;

		GEM_BUG_ON(!was_bound);
		vma->ops->bind_vma(vm, vma, NULL,
				   obj ? i915_gem_object_pat_index(obj) :
				   i915_gem_get_pat_index(vm->i915,
							  I915_CACHE_NONE),
				   was_bound);
	}

	atomic_set(&vm->open, open);
}

void i915_ggtt_resume(struct i915_ggtt *ggtt)
{
	struct intel_gt *gt;
	intel_wakeref_t wf;

	list_for_each_entry(gt, &ggtt->gt_list, ggtt_link)
		intel_gt_check_and_clear_faults(gt);

	with_intel_gt_pm(ggtt->vm.gt, wf) {
		i915_ggtt_resume_vm(&ggtt->vm);
		ggtt->invalidate(ggtt);
	}
}

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
static struct scatterlist *
rotate_pages(struct drm_i915_gem_object *obj, unsigned int offset,
	     unsigned int width, unsigned int height,
	     unsigned int src_stride, unsigned int dst_stride,
	     struct scatterlist *sgt,
	     struct scatterlist *sg,
	     struct scatterlist **end)
{
	unsigned int column, row;
	pgoff_t src_idx;

	for (column = 0; column < width; column++) {
		unsigned int left;

		src_idx = src_stride * (height - 1) + column + offset;
		for (row = 0; row < height; row++) {
			/*
			 * We don't need the pages, but need to initialize
			 * the entries so the sg list can be happily traversed.
			 * The only thing we need are DMA addresses.
			 */
			sg_set_page(sg, NULL, I915_GTT_PAGE_SIZE, 0);
			sg_dma_address(sg) =
				i915_gem_object_get_dma_address(obj, src_idx);
			sg_dma_len(sg) = I915_GTT_PAGE_SIZE;

			*end = sg;
			sg = sg_next(sg);
			sg_count(sgt)++;

			src_idx -= src_stride;
		}

		left = (dst_stride - height) * I915_GTT_PAGE_SIZE;
		if (!left)
			continue;

		/*
		 * The DE ignores the PTEs for the padding tiles, the sg entry
		 * here is just a conenience to indicate how many padding PTEs
		 * to insert at this spot.
		 */
		sg_set_page(sg, NULL, left, 0);
		sg_dma_address(sg) = 0;
		sg_dma_len(sg) = left;

		*end = sg;
		sg = sg_next(sg);
		sg_count(sgt)++;
	}

	return sg;
}

static noinline struct scatterlist *
intel_rotate_pages(struct intel_rotation_info *rot_info,
		   struct drm_i915_gem_object *obj)
{
	unsigned int size = intel_rotation_info_size(rot_info);
	struct scatterlist *sg, *end;
	struct scatterlist *sgt;
	int i;

	sgt = sg_table_inline_create(GFP_KERNEL);
	if (!sgt)
		goto err_sg_alloc;

	if (sg_table_inline_alloc(sgt, size, GFP_KERNEL)) {
		sg_table_inline_free(sgt);
		goto err_sg_alloc;
	}

	end = sg = sgt;
	for (i = 0 ; i < ARRAY_SIZE(rot_info->plane); i++)
		sg = rotate_pages(obj, rot_info->plane[i].offset,
				  rot_info->plane[i].width, rot_info->plane[i].height,
				  rot_info->plane[i].src_stride,
				  rot_info->plane[i].dst_stride,
				  sgt, sg, &end);

	sg_mark_end(end);
	return sgt;

err_sg_alloc:
	drm_dbg(obj->base.dev,
		"Failed to create rotated mapping for object size %zu! (%ux%u tiles, %u pages)\n",
		obj->base.size, rot_info->plane[0].width,
		rot_info->plane[0].height, size);
	return ERR_PTR(-ENOMEM);
}
#else
static noinline struct scatterlist *
intel_rotate_pages(struct intel_rotation_info *rot_info,
		   struct drm_i915_gem_object *obj) { return 0; }
#endif

static struct scatterlist *
add_padding_pages(unsigned int count,
		  struct scatterlist *sgt,
		  struct scatterlist *sg,
		  struct scatterlist **end)
{
	/*
	 * The DE ignores the PTEs for the padding tiles, the sg entry
	 * here is just a convenience to indicate how many padding PTEs
	 * to insert at this spot.
	 */
	sg_set_page(sg, NULL, count * I915_GTT_PAGE_SIZE, 0);
	sg_dma_address(sg) = 0;
	sg_dma_len(sg) = count * I915_GTT_PAGE_SIZE;

	*end = sg;
	sg = sg_next(sg);
	sg_count(sgt)++;

	return sg;
}

static struct scatterlist *
remap_tiled_color_plane_pages(struct drm_i915_gem_object *obj,
			      unsigned long offset, unsigned int alignment_pad,
			      unsigned int width, unsigned int height,
			      unsigned int src_stride, unsigned int dst_stride,
			      struct scatterlist *sgt,
			      struct scatterlist *sg,
			      struct scatterlist **end,
			      unsigned int *gtt_offset)
{
	unsigned int row;

	if (!width || !height)
		return sg;

	if (alignment_pad)
		sg = add_padding_pages(alignment_pad, sgt, sg, end);

	for (row = 0; row < height; row++) {
		unsigned int left = width * I915_GTT_PAGE_SIZE;

		while (left) {
			dma_addr_t addr;
			unsigned int length;

			/*
			 * We don't need the pages, but need to initialize
			 * the entries so the sg list can be happily traversed.
			 * The only thing we need are DMA addresses.
			 */

			addr = i915_gem_object_get_dma_address_len(obj, offset, &length);
			length = min(left, length);

			sg_set_page(sg, NULL, length, 0);
			sg_dma_address(sg) = addr;
			sg_dma_len(sg) = length;

			*end = sg;
			sg = sg_next(sg);
			sg_count(sgt)++;

			offset += length / I915_GTT_PAGE_SIZE;
			left -= length;
		}

		offset += src_stride - width;

		left = (dst_stride - width) * I915_GTT_PAGE_SIZE;
		if (!left)
			continue;

		sg = add_padding_pages(left >> PAGE_SHIFT, sgt, sg, end);
	}

	*gtt_offset += alignment_pad + dst_stride * height;

	return sg;
}

static struct scatterlist *
remap_contiguous_pages(struct drm_i915_gem_object *obj,
		       pgoff_t obj_offset,
		       pgoff_t page_count,
		       struct scatterlist *sgt,
		       struct scatterlist *sg)
{
	struct scatterlist *iter;
	unsigned int offset;

	iter = i915_gem_object_get_sg_dma(obj, obj_offset, &offset);
	GEM_BUG_ON(!iter);

	do {
		unsigned long len;

		len = sg_dma_len(iter) - (offset << PAGE_SHIFT);
		len = min(len, page_count << PAGE_SHIFT);
		GEM_BUG_ON(overflows_type(len, sg->length));

		sg_set_page(sg, NULL, len, 0);
		sg_dma_address(sg) =
			sg_dma_address(iter) + (offset << PAGE_SHIFT);
		sg_dma_len(sg) = len;

		sg_count(sgt)++;
		page_count -= len >> PAGE_SHIFT;
		if (page_count == 0)
			return sg;

		sg = __sg_next(sg);
		iter = __sg_next(iter);
		offset = 0;
	} while (1);
}

static struct scatterlist *
remap_linear_color_plane_pages(struct drm_i915_gem_object *obj,
			       pgoff_t obj_offset, unsigned int alignment_pad,
			       unsigned int size,
			       struct scatterlist *sgt,
			       struct scatterlist *sg,
			       struct scatterlist **end,
			       unsigned int *gtt_offset)
{
	if (!size)
		return sg;

	if (alignment_pad)
		sg = add_padding_pages(alignment_pad, sgt, sg, end);

	sg = remap_contiguous_pages(obj, obj_offset, size, sgt, sg);

	*end = sg;
	sg = sg_next(sg);

	*gtt_offset += alignment_pad + size;
	return sg;
}

static struct scatterlist *
remap_color_plane_pages(const struct intel_remapped_info *rem_info,
			struct drm_i915_gem_object *obj,
			int color_plane,
			struct scatterlist *sgt,
			struct scatterlist *sg,
			struct scatterlist **end,
			unsigned int *gtt_offset)
{
	unsigned int alignment_pad = 0;

	if (rem_info->plane_alignment)
		alignment_pad = ALIGN(*gtt_offset, rem_info->plane_alignment) - *gtt_offset;

	if (rem_info->plane[color_plane].linear)
		sg = remap_linear_color_plane_pages(obj,
						    rem_info->plane[color_plane].offset,
						    alignment_pad,
						    rem_info->plane[color_plane].size,
						    sgt, sg, end,
						    gtt_offset);
	else
		sg = remap_tiled_color_plane_pages(obj,
						   rem_info->plane[color_plane].offset,
						   alignment_pad,
						   rem_info->plane[color_plane].width,
						   rem_info->plane[color_plane].height,
						   rem_info->plane[color_plane].src_stride,
						   rem_info->plane[color_plane].dst_stride,
						   sgt, sg, end,
						   gtt_offset);

	return sg;
}

static noinline struct scatterlist *
intel_remap_pages(struct intel_remapped_info *rem_info,
		  struct drm_i915_gem_object *obj)
{
	unsigned int size = intel_remapped_info_size(rem_info);
	struct scatterlist *sg, *end;
	unsigned int gtt_offset = 0;
	struct scatterlist *sgt;
	int i;

	sgt = sg_table_inline_create(GFP_KERNEL);
	if (!sgt)
		goto err_st_alloc;

	if (sg_table_inline_alloc(sgt, size, GFP_KERNEL)) {
		sg_table_inline_free(sgt);
		goto err_st_alloc;
	}

	end = sg = sgt;
	for (i = 0 ; i < ARRAY_SIZE(rem_info->plane); i++)
		sg = remap_color_plane_pages(rem_info, obj, i, sgt, sg, &end, &gtt_offset);

	sg_mark_end(end);
	i915_sg_trim(sgt);

	return sgt;

err_st_alloc:
	drm_dbg(obj->base.dev,
		"Failed to create remapped mapping for object size %zu! (%ux%u tiles, %u pages)\n",
		obj->base.size, rem_info->plane[0].width,
		rem_info->plane[0].height, size);
	return ERR_PTR(-ENOMEM);
}

static noinline struct scatterlist *
intel_partial_pages(const struct i915_ggtt_view *view,
		    struct drm_i915_gem_object *obj)
{
	struct scatterlist *sgt;
	struct scatterlist *sg;

	sgt = sg_table_inline_create(GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	if (sg_table_inline_alloc(sgt, view->partial.size, GFP_KERNEL)) {
		sg_table_inline_free(sgt);
		return ERR_PTR(-ENOMEM);
	}

	sg = remap_contiguous_pages(obj,
				    view->partial.offset,
				    view->partial.size,
				    sgt, sgt);
	sg_mark_end(sg);
	i915_sg_trim(sgt);

	return sgt;
}

static int
i915_get_ggtt_vma_pages(struct i915_vma *vma)
{
	int ret;

	/*
	 * The vma->pages are only valid within the lifespan of the borrowed
	 * obj->mm.pages. When the obj->mm.pages sg_table is regenerated, so
	 * must be the vma->pages. A simple rule is that vma->pages must only
	 * be accessed when the obj->mm.pages are pinned.
	 */
	GEM_BUG_ON(!i915_gem_object_has_pinned_pages(vma->obj));

	if (vma->ggtt_view.type != I915_GGTT_VIEW_NORMAL) {
		ret = i915_gem_object_migrate_sync(vma->obj);
		if(ret)
			return ret;
	}

	switch (vma->ggtt_view.type) {
	default:
		GEM_BUG_ON(vma->ggtt_view.type);
		fallthrough;
	case I915_GGTT_VIEW_NORMAL:
		vma->pages = vma->obj->mm.pages;
		return 0;

	case I915_GGTT_VIEW_ROTATED:
		vma->pages =
			intel_rotate_pages(&vma->ggtt_view.rotated, vma->obj);
		break;

	case I915_GGTT_VIEW_REMAPPED:
		vma->pages =
			intel_remap_pages(&vma->ggtt_view.remapped, vma->obj);
		break;

	case I915_GGTT_VIEW_PARTIAL:
		vma->pages = intel_partial_pages(&vma->ggtt_view, vma->obj);
		break;
	}

	ret = 0;
	set_bit(I915_VMA_PARTIAL_BIT, __i915_vma_flags(vma));
	if (IS_ERR(vma->pages)) {
		ret = PTR_ERR(vma->pages);
		vma->pages = NULL;
	}
	return ret;
}

/**
 * i915_ggtt_balloon - reserve fixed space in an GGTT
 * @ggtt: the &struct i915_ggtt
 * @start: start offset inside the GGTT,
 *          must be #I915_GTT_MIN_ALIGNMENT aligned
 * @end: end offset inside the GGTT,
 *        must be #I915_GTT_PAGE_SIZE aligned
 * @node: the &struct drm_mm_node
 *
 * i915_ggtt_balloon() tries to reserve the @node from @start to @end inside
 * GGTT the address space.
 *
 * Returns: 0 on success, -ENOSPC if no suitable hole is found.
 */
int i915_ggtt_balloon(struct i915_ggtt *ggtt, u64 start, u64 end,
		      struct drm_mm_node *node)
{
	u64 size = end - start;
	int err;

	GEM_BUG_ON(start >= end);
	drm_dbg(&ggtt->vm.i915->drm, "%sGGTT [%#llx-%#llx] %lluK\n",
		"ballooning ", start, end, size / SZ_1K);

	err = i915_gem_gtt_reserve(&ggtt->vm, node, size, start,
				   I915_COLOR_UNEVICTABLE, PIN_NOEVICT);
	if (unlikely(err)) {
		intel_gt_log_driver_error(ggtt->vm.gt, INTEL_GT_DRIVER_ERROR_GGTT,
					  "%sGGTT [%#llx-%#llx] %lluK\n",
					  "Failed to balloon ", node->start,
					  node->start + node->size, node->size / SZ_1K);
		return err;
	}

	ggtt->vm.reserved += node->size;
	return 0;
}

bool i915_ggtt_has_xehpsdv_pte_vfid_mask(struct i915_ggtt *ggtt)
{
	return GRAPHICS_VER_FULL(ggtt->vm.i915) < IP_VER(12, 50);
}

void i915_ggtt_deballoon(struct i915_ggtt *ggtt, struct drm_mm_node *node)
{
	if (!drm_mm_node_allocated(node))
		return;

	drm_dbg(&ggtt->vm.i915->drm, "%sGGTT [%#llx-%#llx] %lluK\n",
		"deballooning ", node->start, node->start + node->size,
		node->size / SZ_1K);

	GEM_BUG_ON(ggtt->vm.reserved < node->size);
	ggtt->vm.reserved -= node->size;
	drm_mm_remove_node(node);
}

static gen8_pte_t tgl_prepare_vf_pte_vfid(u16 vfid)
{
	GEM_BUG_ON(!FIELD_FIT(TGL_GGTT_PTE_VFID_MASK, vfid));

	return FIELD_PREP(TGL_GGTT_PTE_VFID_MASK, vfid);
}

static gen8_pte_t xehpsdv_prepare_vf_pte_vfid(u16 vfid)
{
	GEM_BUG_ON(!FIELD_FIT(XEHPSDV_GGTT_PTE_VFID_MASK, vfid));

	return FIELD_PREP(XEHPSDV_GGTT_PTE_VFID_MASK, vfid);
}

static gen8_pte_t prepare_vf_pte_vfid(struct i915_ggtt *ggtt, u16 vfid)
{
	if (i915_ggtt_has_xehpsdv_pte_vfid_mask(ggtt))
		return tgl_prepare_vf_pte_vfid(vfid);
	else
		return xehpsdv_prepare_vf_pte_vfid(vfid);
}

static gen8_pte_t prepare_vf_pte(struct i915_ggtt *ggtt, u16 vfid)
{
	return prepare_vf_pte_vfid(ggtt, vfid) | GEN8_PAGE_PRESENT;
}

void i915_ggtt_set_space_owner(struct i915_ggtt *ggtt, u16 vfid,
			       const struct drm_mm_node *node)
{
	gen8_pte_t __iomem *gtt_entries = gsm_base(ggtt);
	const gen8_pte_t pte = prepare_vf_pte(ggtt, vfid);
	u64 base = node->start;
	u64 size = node->size;

	GEM_BUG_ON(!IS_SRIOV_PF(ggtt->vm.i915));
	GEM_BUG_ON(base % PAGE_SIZE);
	GEM_BUG_ON(size % PAGE_SIZE);

	drm_dbg(&ggtt->vm.i915->drm, "GGTT VF%u [%#llx-%#llx] %lluK\n",
		vfid, base, base + size, size / SZ_1K);

	gtt_entries += base >> PAGE_SHIFT;
	while (size) {
		gen8_set_pte(gtt_entries++, pte);
		size -= PAGE_SIZE;
	}

	ggtt->invalidate(ggtt);
}

static inline unsigned int __ggtt_size_to_ptes_size(u64 ggtt_size)
{
	GEM_BUG_ON(!IS_ALIGNED(ggtt_size, I915_GTT_MIN_ALIGNMENT));

	return (ggtt_size >> PAGE_SHIFT) * sizeof(gen8_pte_t);
}

static void ggtt_pte_clear_vfid(void *buf, u64 size)
{
	while (size) {
		*(gen8_pte_t *)buf &= ~XEHPSDV_GGTT_PTE_VFID_MASK;

		buf += sizeof(gen8_pte_t);
		size -= sizeof(gen8_pte_t);
	}
}

/**
 * i915_ggtt_save_ptes - copy GGTT PTEs to preallocated buffer
 * @ggtt: the &struct i915_ggtt
 * @node: the &struct drm_mm_node - the @node->start is used as the start offset for save
 * @buf: preallocated buffer in which PTEs will be saved
 * @size: size of prealocated buffer (in bytes)
 *        - must be sizeof(gen8_pte_t) aligned
 * @flags: function flags:
 *         - #I915_GGTT_SAVE_PTES_NO_VFID BIT - save PTEs without VFID
 *
 * Returns: size of the buffer used (or needed if both @buf and @size are (0)) to store all PTEs
 *          for a given node, -EINVAL if one of @buf or @size is 0.
 */
int i915_ggtt_save_ptes(struct i915_ggtt *ggtt, const struct drm_mm_node *node, void *buf,
			unsigned int size, unsigned int flags)
{
	gen8_pte_t __iomem *gtt_entries = gsm_base(ggtt);

	if (!buf && !size)
		return __ggtt_size_to_ptes_size(node->size);

	if (!buf || !size)
		return -EINVAL;

	GEM_BUG_ON(!IS_ALIGNED(size, sizeof(gen8_pte_t)));
	GEM_WARN_ON(size > __ggtt_size_to_ptes_size(SZ_4G));

	if (size < __ggtt_size_to_ptes_size(node->size))
		return -ENOSPC;
	size = __ggtt_size_to_ptes_size(node->size);

	gtt_entries += node->start >> PAGE_SHIFT;

	memcpy_fromio(buf, gtt_entries, size);

	if (flags & I915_GGTT_SAVE_PTES_NO_VFID)
		ggtt_pte_clear_vfid(buf, size);

	return size;
}

/**
 * i915_ggtt_restore_ptes() -  restore GGTT PTEs from buffer
 * @ggtt: the &struct i915_ggtt
 * @node: the &struct drm_mm_node - the @node->start is used as the start offset for restore
 * @buf: buffer from which PTEs will be restored
 * @size: size of prealocated buffer (in bytes)
 *        - must be sizeof(gen8_pte_t) aligned
 * @flags: function flags:
 *         - #I915_GGTT_RESTORE_PTES_VFID_MASK - VFID for restored PTEs
 *         - #I915_GGTT_RESTORE_PTES_NEW_VFID - restore PTEs with new VFID
 *           (from #I915_GGTT_RESTORE_PTES_VFID_MASK)
 *
 * Returns: 0 on success, -ENOSPC if @node->size is less than size.
 */
int i915_ggtt_restore_ptes(struct i915_ggtt *ggtt, const struct drm_mm_node *node, const void *buf,
			   unsigned int size, unsigned int flags)
{
	gen8_pte_t __iomem *gtt_entries = gsm_base(ggtt);
	u32 vfid = FIELD_GET(I915_GGTT_RESTORE_PTES_VFID_MASK, flags);
	gen8_pte_t pte;

	GEM_BUG_ON(!size);
	GEM_BUG_ON(!IS_ALIGNED(size, sizeof(gen8_pte_t)));

	if (size > __ggtt_size_to_ptes_size(node->size))
		return -ENOSPC;

	gtt_entries += node->start >> PAGE_SHIFT;

	while (size) {
		pte = *(gen8_pte_t *)buf;
		if (flags & I915_GGTT_RESTORE_PTES_NEW_VFID)
			pte |= prepare_vf_pte_vfid(ggtt, vfid);
		gen8_set_pte(gtt_entries++, pte);

		buf += sizeof(gen8_pte_t);
		size -= sizeof(gen8_pte_t);
	}

	ggtt->invalidate(ggtt);

	return 0;
}
