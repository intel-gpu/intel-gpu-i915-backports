// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

#include "gem/i915_gem_userptr.h"

#include "gt/gen8_ppgtt.h"
#include "gt/intel_context.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_regs.h"
#include "gt/intel_tlb.h"

#include "gt/uc/intel_guc.h"
#include "gt/uc/intel_guc_fwif.h"

#include "i915_drv.h"
#include "gem/i915_gem_lmem.h"
#include "gem/i915_gem_mman.h"
#include "intel_pagefault.h"
#include "intel_uncore.h"
#include "gem/i915_gem_vm_bind.h"

/**
 * DOC: Recoverable page fault implications
 *
 * Modern GPU hardware support recoverable page fault. This has extensive
 * implications to driver implementation.
 *
 * DMA fence is used extensively to track object activity for cross-device
 * and cross-application synchronization. But if recoverable page fault is
 * enabled, using of DMA fence can potentially induce deadlock: A pending
 * page fault holds up the GPU work which holds up the dma fence signaling,
 * and memory allocation is usually required to resolve a page fault, but
 * memory allocation is not allowed to gate dma fence signaling.
 *
 * Non-long-run context usually uses DMA fence for GPU job/object completion
 * tracking, thus faultable vm is not allowed for non-long-run context.
 *
 * Suspend fence is used to suspend long run context before we unbind
 * BOs, in case of userptr invalidation, memory shrinking or eviction.
 * For faultable vm, there is no need to use suspend fence: we directly
 * unbind BOs w/o suspend context and BOs will be rebound during a recoverable
 * page fault handling thereafter.
 *
 * DMA fences attached to vm's active are used to track vm's activity.
 * i.e., driver wait on those dma fences for vm to be idle. This method
 * is useful for non-faultable vm. For faultable vm, we don't support
 * any DMA fence because of the deadlock described above. Thus, we can't attach
 * any DMA fences, including suspend fence or request fence, to a faultable vm.
 */

struct page_fault_info {
	u8 access_type;
	u8 fault_type;
	u8 engine_id;
	u8 source_id;
	u8 fault_lvl;
	u64 address;
};

int intel_pagefault_process_cat_error_msg(struct intel_guc *guc,
					  const u32 *payload, u32 len)
{
	struct intel_gt *gt = guc_to_gt(guc);
	struct drm_i915_private *i915 = gt->i915;
	u32 ctx_id;

	if (len < 1)
		return -EPROTO;

	ctx_id = payload[0];

	drm_err(&i915->drm, "GPU catastrophic memory error: GT %d, GuC context 0x%x\n",
		gt->info.id, ctx_id);

	return 0;
}

static u64 __get_address(u32 fault_data0, u32 fault_data1)
{
	return ((u64)(fault_data1 & FAULT_VA_HIGH_BITS) << 44) |
	       ((u64)fault_data0 << 12);
}

static u8 __get_engine_id(u32 fault_reg_data)
{
	return GEN8_RING_FAULT_ENGINE_ID(fault_reg_data);
}

static u8 __get_source_id(u32 fault_reg_data)
{
	return RING_FAULT_SRCID(fault_reg_data);
}

static u8 __get_access_type(u32 fault_reg_data)
{
	return !!(fault_reg_data & GEN12_RING_FAULT_ACCESS_TYPE);
}

static u8 __get_fault_lvl(u32 fault_reg_data)
{
	return RING_FAULT_LEVEL(fault_reg_data);
}

static u8 __get_fault_type(u32 fault_reg_data)
{
	return GEN12_RING_FAULT_FAULT_TYPE(fault_reg_data);
}

static void print_page_fault(struct drm_printer *p,
			     struct page_fault_info *info)
{
	drm_printf(p, "Unexpected fault\n"
		      "\tAddr: 0x%08x_%08x\n"
		      "\tEngine ID: %d\n"
		      "\tSource ID: %d\n"
		      "\tType: %d\n"
		      "\tFault Level: %d\n"
		      "\tAccess type: %s\n",
		      upper_32_bits(info->address),
		      lower_32_bits(info->address),
		      info->engine_id,
		      info->source_id,
		      info->fault_type,
		      info->fault_lvl,
		      info->access_type ?
		      "Write" : "Read");
}

/*
 * DOC: INTEL_GUC_ACTION_PAGE_FAULT_NOTIFICATION
 *
 *      +==========================================================+
 *      | G2H REPORT PAGE FAULT MESSAGE PAYLOAD                    |
 *      +==========================================================+
 *      | 0 | 31:30 |Fault response:                               |
 *      |   |       | 00 - fault successful resolved               |
 *      |   |       | 01 - fault resolution is unsuccessful        |
 *      |   |-------+----------------------------------------------|
 *      |   | 29:20 |Reserved                                      |
 *      |   |-------+----------------------------------------------|
 *      |   | 19:18 |Fault type:                                   |
 *      |   |       | 00 - page not present                        |
 *      |   |       | 01 - write access violation                  |
 *      |   |-------+----------------------------------------------|
 *      |   |   17  |Access type of the memory request that fault  |
 *      |   |       | 0 - faulted access is a read request         |
 *      |   |       | 1 = faulted access is a write request        |
 *      |   |-------+----------------------------------------------|
 *      |   | 16:12 |Engine Id of the faulted memory cycle         |
 *      |   |-------+----------------------------------------------|
 *      |   |   11  |Reserved                                      |
 *      |   |-------+----------------------------------------------|
 *      |   |  10:3 |Source ID of the faulted memory cycle         |
 *      |   |-------+----------------------------------------------|
 *      |   |   2:1 |Fault level:                                  |
 *      |   |       | 00 - PTE                                     |
 *      |   |       | 01 - PDE                                     |
 *      |   |       | 10 - PDP                                     |
 *      |   |       | 11 - PML4                                    |
 *      |   |-------+----------------------------------------------|
 *      |   |     0 |Valid bit                                     |
 *      +---+-------+----------------------------------------------+
 *      | 1 |  31:0 |Fault cycle virtual address [43:12]           |
 *      +---+-------+----------------------------------------------+
 *      | 2 |  31:4 |Reserved                                      |
 *      |   |-------+----------------------------------------------|
 *      |   |   3:0 |Fault cycle virtual address [47:44]           |
 *      +==========================================================+
 */
int intel_pagefault_process_page_fault_msg(struct intel_guc *guc,
					   const u32 *payload, u32 len)
{
	struct drm_i915_private *i915 = guc_to_gt(guc)->i915;
	struct page_fault_info info = {};
	struct drm_printer p = drm_info_printer(i915->drm.dev);

	if (len < 3)
		return -EPROTO;

	info.address = __get_address(payload[1], payload[2]);
	info.engine_id = __get_engine_id(payload[0]);
	info.source_id = __get_source_id(payload[0]);
	info.access_type = __get_access_type(payload[0]);
	info.fault_lvl = __get_fault_lvl(payload[0]);
	info.fault_type = __get_fault_type(payload[0]);

	print_page_fault(&p, &info);

	return 0;
}

static void print_recoverable_fault(struct recoverable_page_fault_info *info)
{
	/*XXX: Move to trace_printk */
	DRM_DEBUG_DRIVER("\n\tASID: %d\n"
			 "\tVFID: %d\n"
			 "\tPDATA: 0x%04x\n"
			 "\tFaulted Address: 0x%08x_%08x\n"
			 "\tFaultType: %d\n"
			 "\tAccessType: %d\n"
			 "\tFaultLevel: %d\n"
			 "\tEngineClass: %d\n"
			 "\tEngineInstance: %d\n",
			 info->asid,
			 info->vfid,
			 info->pdata,
			 upper_32_bits(info->page_addr),
			 lower_32_bits(info->page_addr),
			 info->fault_type,
			 info->access_type,
			 info->fault_level,
			 info->engine_class,
			 info->engine_instance);
}

static bool userptr_needs_rebind(struct drm_i915_gem_object *obj)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	bool ret = false;

	if (!i915_gem_object_is_userptr(obj))
		return ret;
	i915_gem_userptr_lock_mmu_notifier(i915);
	if (i915_gem_object_userptr_submit_done(obj))
		ret = true;
	i915_gem_userptr_unlock_mmu_notifier(i915);
	return ret;
}

static int migrate_to_lmem(struct drm_i915_gem_object *obj,
			   struct intel_gt *gt,
			   enum intel_region_id lmem_id,
			   struct i915_gem_ww_ctx *ww)
{
	enum intel_engine_id id = gt->rsvd_bcs;
	struct intel_context *ce;
	int ret;

	if (!gt->engine[id])
		return -ENODEV;

	ce = gt->engine[id]->blitter_context;

	/*
	 * FIXME: Move this to BUG_ON later when uapi enforces object alignment
	 * to 64K for objects that can reside on both SMEM and LMEM.
	 */
	if (HAS_64K_PAGES(gt->i915) &&
	    !IS_ALIGNED(obj->base.size, I915_GTT_PAGE_SIZE_64K)) {
		DRM_DEBUG_DRIVER("Cannot migrate objects of different page sizes\n");
		return -ENOTSUPP;
	}

	i915_gem_object_release_mmap(obj);
	GEM_BUG_ON(obj->mm.mapping);
	GEM_BUG_ON(obj->base.filp && mapping_mapped(obj->base.filp->f_mapping));

	/* unmap to avoid further update to the page[s] */
	ret = i915_gem_object_unbind(obj, ww, I915_GEM_OBJECT_UNBIND_ACTIVE);
	if (ret) {
		DRM_ERROR("Cannot unmap obj(%d)\n", ret);
		return ret;
	}

	ret = i915_gem_object_migrate(obj, ww, ce, lmem_id, true);
	if (i915_gem_object_is_lmem(obj))
		DRM_DEBUG_DRIVER("Migrated object to LMEM\n");

	return ret;
}

static inline bool access_is_atomic(enum recoverable_page_fault_type err_code)
{
	if (err_code == FAULT_ATOMIC_NOT_PRESENT ||
	    err_code == FAULT_ATOMIC_ACCESS_VIOLATION)
		return true;

	return false;
}

static enum intel_region_id get_lmem_region_id(struct drm_i915_gem_object *obj, struct intel_gt *gt)
{
	int i;

	if (obj->mm.preferred_region &&
	    obj->mm.preferred_region->type == INTEL_MEMORY_LOCAL)
		return obj->mm.preferred_region->id;

	if (BIT(gt->lmem->id) & obj->memory_mask)
		return gt->lmem->id;

	for (i = 0; i < obj->mm.n_placements; i++) {
		struct intel_memory_region *mr = obj->mm.placements[i];

		if (mr->type == INTEL_MEMORY_LOCAL)
			return mr->id;
	}

	return 0;
}

static int validate_fault(struct i915_vma *vma, enum recoverable_page_fault_type err_code)
{
	int err = 0;

	switch (err_code & 0xF) {
	case FAULT_READ_NOT_PRESENT:
		break;
	case FAULT_WRITE_NOT_PRESENT:
		if (i915_gem_object_is_readonly(vma->obj))
			err = -EACCES;
		break;
	case FAULT_ATOMIC_NOT_PRESENT:
	case FAULT_ATOMIC_ACCESS_VIOLATION:
		if (!(vma->obj->memory_mask & REGION_LMEM_MASK)) {
			pr_err("Atomic Access Violation\n");
			err = -EACCES;
		}
		break;
	case FAULT_WRITE_ACCESS_VIOLATION:
		pr_err("Write Access Violation\n");
		err = -EACCES;
		break;
	default:
		pr_err("Undefined Fault Type\n");
		err = -EACCES;
		break;
	}

	return err;
}

static struct i915_address_space *faulted_vm(struct intel_guc *guc, u32 asid)
{
	if (GEM_WARN_ON(asid >= I915_MAX_ASID))
		return NULL;

	return xa_load(&guc_to_gt(guc)->i915->asid_resv.xa, asid);
}

static int handle_i915_mm_fault(struct intel_guc *guc,
				struct recoverable_page_fault_info *info)
{
	enum recoverable_page_fault_type err_code;
	struct intel_gt *gt = guc_to_gt(guc);
	struct i915_vma_work *work = NULL;
	struct intel_engine_cs *engine;
	struct i915_address_space *vm;
	struct i915_vma *vma = NULL;
	enum intel_region_id lmem_id;
	struct i915_gem_ww_ctx ww;
	int err;

	vm = faulted_vm(guc, info->asid);
	/* The active context [asid] is protected while servicing a fault */
	if (GEM_WARN_ON(!vm))
		return -ENOENT;

	vma = i915_find_vma(vm, info->page_addr);
	if (!vma) {
		if (vm->has_scratch) {
			u64 length = i915_vm_has_scratch_64K(vm) ? I915_GTT_PAGE_SIZE_64K : I915_GTT_PAGE_SIZE_4K;

			DRM_DEBUG_DRIVER("Bind invalid va: 0x%08x_%08x to scratch\n",
					 upper_32_bits(info->page_addr), lower_32_bits(info->page_addr));
			gen12_init_fault_scratch(vm, info->page_addr, length, true);
			vm->invalidate_tlb_scratch = true;
			return 0;
		}

		GEM_WARN_ON(!vma);
		return -ENOENT;
	}

	trace_i915_mm_fault(gt->i915, vm, vma->obj, info);

	if (!i915_vma_is_persistent(vma))
		GEM_BUG_ON(!i915_vma_is_active(vma));

	err_code = (info->fault_type << 2) | info->access_type;
	err = validate_fault(vma, err_code);
	if (err)
		goto put_vma;

	if (i915_gem_object_is_userptr(vma->obj)) {
		err = i915_gem_object_userptr_submit_init(vma->obj);
		if (err)
			goto put_vma;
	}

	i915_gem_ww_ctx_init(&ww, false);

 retry:
	err = i915_gem_object_lock(vma->obj, &ww);
	if (err)
		goto err_ww;

	GEM_BUG_ON(info->engine_class > MAX_ENGINE_CLASS ||
		   info->engine_instance > MAX_ENGINE_INSTANCE);
	engine = gt->engine_class[info->engine_class][info->engine_instance];

	lmem_id = get_lmem_region_id(vma->obj, gt);
	if (access_is_atomic(err_code) ||
	    (lmem_id && i915_gem_object_should_migrate(vma->obj, lmem_id))) {
		err = migrate_to_lmem(vma->obj, gt, lmem_id, &ww);

		/*
		 * Migration is best effort.
		 * if we see -EDEADLK handle that with proper backoff. Otherwise
		 * for scenarios like atomic operation, if migration fails,
		 * gpu will fault again and we can retry.
		 */
		if (err == -EDEADLK)
			goto err_ww;

	}

	err = vma_get_pages(vma);
	if (err)
		goto err_ww;

	work = i915_vma_work(vma);
	if (!work) {
		err = -ENOMEM;
		goto err_pages;
	}
	err = i915_vma_work_set_vm(work, vma, &ww);
	if (err)
		goto err_fence;

	err = mutex_lock_interruptible(&vm->mutex);
	if (err)
		goto err_fence;

	if (i915_vma_is_bound(vma, PIN_USER))
		goto err_unlock;

	GEM_BUG_ON(!vma->pages);

	err = i915_active_acquire(&vma->active);
	if (err)
		goto err_unlock;

	err = i915_vma_bind(vma, vma->obj->cache_level, PIN_USER, work);
	if (err)
		goto err_active;

	atomic_add(I915_VMA_PAGES_ACTIVE, &vma->pages_count);
	GEM_BUG_ON(!i915_vma_is_bound(vma, PIN_USER));

	/*
	 * For non active bind, it has already been pinned in
	 * i915_vma_fault_pin, so, only pin for active bind here.
	 */
	if (i915_vma_is_active_bind(vma))
		__i915_vma_pin(vma);
err_active:
	i915_active_release(&vma->active);
err_unlock:
	mutex_unlock(&vm->mutex);
err_fence:
	i915_vma_work_commit(work);
err_pages:
	vma_put_pages(vma);
	if (!err)
		err = i915_vma_wait_for_bind(vma);
	if (!err && userptr_needs_rebind(vma->obj)) {
		err = i915_gem_ww_ctx_backoff(&ww);
		i915_gem_ww_ctx_fini(&ww);
		if (err) {
			goto put_vma;
		} else {
			err = i915_gem_object_userptr_submit_init(vma->obj);
			if (err)
				goto put_vma;
			i915_gem_ww_ctx_init(&ww, false);
			goto retry;
		}
	}
 err_ww:
	if (err == -EDEADLK) {
		err = i915_gem_ww_ctx_backoff(&ww);
		if (!err)
			goto retry;
	}

	i915_gem_ww_ctx_fini(&ww);
put_vma:
	i915_vma_put(vma);
	__i915_vma_put(vma);
	/*
	 * Intermediate levels of page tables could have been cached in the tlbs
	 * which maps to scarcth entries. Make sure they are invalidated, so
	 * that walker see the correct mappings
	 */
	if (!err && vm->invalidate_tlb_scratch) {
		unsigned int i;

		for_each_gt(vm->i915, i, gt) {
			if (!atomic_read(&vm->active_contexts_gt[i]))
				continue;

			intel_gt_invalidate_tlb_range(gt, vm,
						      i915_vma_offset(vma),
						      i915_vma_size(vma));
		}
		vm->invalidate_tlb_scratch = false;
	}

	return err;
}

static void get_fault_info(const u32 *payload, struct recoverable_page_fault_info *info)
{
	const struct intel_guc_pagefault_desc *desc;
	desc = (const struct intel_guc_pagefault_desc *)payload;

	info->fault_level = FIELD_GET(PAGE_FAULT_DESC_FAULT_LEVEL, desc->dw0);
	info->engine_class = FIELD_GET(PAGE_FAULT_DESC_ENG_CLASS, desc->dw0);
	info->engine_instance = FIELD_GET(PAGE_FAULT_DESC_ENG_INSTANCE, desc->dw0);
	info->pdata = FIELD_GET(PAGE_FAULT_DESC_PDATA_HI,
				desc->dw1) << PAGE_FAULT_DESC_PDATA_HI_SHIFT;
	info->pdata |= FIELD_GET(PAGE_FAULT_DESC_PDATA_LO, desc->dw0);
	info->asid =  FIELD_GET(PAGE_FAULT_DESC_ASID, desc->dw1);
	info->vfid =  FIELD_GET(PAGE_FAULT_DESC_VFID, desc->dw2);
	info->access_type = FIELD_GET(PAGE_FAULT_DESC_ACCESS_TYPE, desc->dw2);
	info->fault_type = FIELD_GET(PAGE_FAULT_DESC_FAULT_TYPE, desc->dw2);
	info->page_addr = (u64)(FIELD_GET(PAGE_FAULT_DESC_VIRTUAL_ADDR_HI,
					  desc->dw3)) << PAGE_FAULT_DESC_VIRTUAL_ADDR_HI_SHIFT;
	info->page_addr |= FIELD_GET(PAGE_FAULT_DESC_VIRTUAL_ADDR_LO,
				     desc->dw2) << PAGE_FAULT_DESC_VIRTUAL_ADDR_LO_SHIFT;
}

int intel_pagefault_req_process_msg(struct intel_guc *guc,
				    const u32 *payload,
				    u32 len)
{
	struct intel_guc_pagefault_reply reply = {};
	struct recoverable_page_fault_info info = {};
	int ret;

	if (unlikely(len != 4))
		return -EPROTO;

	get_fault_info(payload, &info);
	print_recoverable_fault(&info);

	ret = handle_i915_mm_fault(guc, &info);
	if (ret)
		info.fault_unsuccessful = 1;

	/* XXX: Move to trace_printk */
	DRM_DEBUG_DRIVER("Fault response: %s, ret = %d\n",
			 info.fault_unsuccessful ?
			 "Unsuccessful" : "Successful", ret);

	reply.dw0 = FIELD_PREP(PAGE_FAULT_REPLY_VALID, 1) |
		FIELD_PREP(PAGE_FAULT_REPLY_SUCCESS, info.fault_unsuccessful) |
		FIELD_PREP(PAGE_FAULT_REPLY_REPLY, PAGE_FAULT_REPLY_ACCESS) |
		FIELD_PREP(PAGE_FAULT_REPLY_DESC_TYPE, FAULT_RESPONSE_DESC) |
		FIELD_PREP(PAGE_FAULT_REPLY_ASID, info.asid);

	reply.dw1 =  FIELD_PREP(PAGE_FAULT_REPLY_VFID, info.vfid) |
		FIELD_PREP(PAGE_FAULT_REPLY_ENG_INSTANCE, info.engine_instance) |
		FIELD_PREP(PAGE_FAULT_REPLY_ENG_CLASS, info.engine_class) |
		FIELD_PREP(PAGE_FAULT_REPLY_PDATA, info.pdata);

	return intel_guc_send_pagefault_reply(guc, &reply);
}

const char *intel_pagefault_type2str(enum recoverable_page_fault_type type)
{
	static const char * const faults[] = {
		[FAULT_READ_NOT_PRESENT] = "read not present",
		[FAULT_WRITE_NOT_PRESENT] = "write not present",
		[FAULT_ATOMIC_NOT_PRESENT] = "atomic not present",
		[FAULT_WRITE_ACCESS_VIOLATION] = "write access violation",
		[FAULT_ATOMIC_ACCESS_VIOLATION] = "atomic access violation",
	};

	if (type > FAULT_ATOMIC_ACCESS_VIOLATION || !faults[type])
		return "invalid fault type";

	return faults[type];
};

static struct i915_vma *get_acc_vma(struct intel_guc *guc,
				    struct acc_info *info)
{
	struct i915_address_space *vm;
	u64 page_va;

	vm = faulted_vm(guc, info->asid);
	if (GEM_WARN_ON(!vm))
		return NULL;

	page_va = info->va_range_base + (ffs(info->sub_granularity) - 1)
		  * sub_granularity_in_byte(info->granularity);

	return i915_find_vma(vm, page_va);
}

static int acc_migrate_to_lmem(struct intel_gt *gt, struct i915_vma *vma)
{
	struct i915_gem_ww_ctx ww;
	int err = 0;

	i915_gem_vm_bind_lock(vma->vm);

	if (!i915_vma_is_bound(vma, PIN_USER)) {
		i915_gem_vm_bind_unlock(vma->vm);
		return 0;
	}

	i915_gem_ww_ctx_init(&ww, false);

retry:
	err = i915_gem_object_lock(vma->obj, &ww);
	if (!err) {
		enum intel_region_id lmem_id;

		lmem_id = get_lmem_region_id(vma->obj, gt);
		if (lmem_id)
			err = migrate_to_lmem(vma->obj, gt, lmem_id, &ww);
	}

	if (err == -EDEADLK) {
		err = i915_gem_ww_ctx_backoff(&ww);
		if (!err)
			goto retry;
	}

	i915_gem_ww_ctx_fini(&ww);
	i915_gem_vm_bind_unlock(vma->vm);

	return err;
}

static int handle_i915_acc(struct intel_guc *guc,
			   struct acc_info *info)
{
	struct intel_gt *gt = guc_to_gt(guc);
	struct i915_vma *vma;

	trace_intel_access_counter(gt, info);
	if (info->access_type)
		return 0;

	vma = get_acc_vma(guc, info);
	if (!vma)
		return 0;

	if (i915_gem_object_is_userptr(vma->obj)) {
		int err = i915_gem_object_userptr_submit_init(vma->obj);

		if (err)
			goto put_vma;
	}

	acc_migrate_to_lmem(gt, vma);

	if (i915_gem_object_is_userptr(vma->obj))
		i915_gem_object_userptr_submit_done(vma->obj);
put_vma:
	i915_vma_put(vma);
	__i915_vma_put(vma);

	return 0;
}

static void get_access_counter_info(struct access_counter_desc *desc,
				    struct acc_info *info)
{
	info->granularity = FIELD_GET(ACCESS_COUNTER_GRANULARITY, desc->dw2);
	info->sub_granularity =	FIELD_GET(ACCESS_COUNTER_SUBG_HI, desc->dw1) << 31 |
				FIELD_GET(ACCESS_COUNTER_SUBG_LO, desc->dw0);
	info->engine_class = FIELD_GET(ACCESS_COUNTER_ENG_CLASS, desc->dw1);
	info->engine_instance = FIELD_GET(ACCESS_COUNTER_ENG_INSTANCE, desc->dw1);
	info->asid =  FIELD_GET(ACCESS_COUNTER_ASID, desc->dw1);
	info->vfid =  FIELD_GET(ACCESS_COUNTER_VFID, desc->dw2);
	info->access_type = FIELD_GET(ACCESS_COUNTER_TYPE, desc->dw0);
	info->va_range_base = make_u64(desc->dw3 & ACCESS_COUNTER_VIRTUAL_ADDR_RANGE_HI,
			      desc->dw2 & ACCESS_COUNTER_VIRTUAL_ADDR_RANGE_LO);

	GEM_BUG_ON(info->engine_class > MAX_ENGINE_CLASS ||
		   info->engine_instance > MAX_ENGINE_INSTANCE);
	DRM_DEBUG_DRIVER("Access counter request:\n"
			"\tType: %s\n"
			"\tASID: %d\n"
			"\tVFID: %d\n"
			"\tEngine: %s[%d]\n"
			"\tGranularity: 0x%x KB Region/ %d KB sub-granularity\n"
			"\tSub_Granularity Vector: 0x%08x\n"
			"\tVA Range base: 0x%016llx\n",
			info->access_type ? "AC_NTFY_VAL" : "AC_TRIG_VAL",
			info->asid, info->vfid,
			intel_engine_class_repr(info->engine_class),
			info->engine_instance,
			granularity_in_byte(info->granularity) / SZ_1K,
			sub_granularity_in_byte(info->granularity) / SZ_1K,
			info->sub_granularity,
			info->va_range_base
			);
}

int intel_access_counter_req_process_msg(struct intel_guc *guc, const u32 *payload, u32 len)
{
	struct acc_info info = {};

	if (unlikely(len != 4))
		return -EPROTO;

	get_access_counter_info((struct access_counter_desc *)payload, &info);
	return handle_i915_acc(guc, &info);
}
