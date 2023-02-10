// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */
#include "gem/i915_gem_lmem.h"
#include "gem/i915_gem_mman.h"
#include "gem/i915_gem_userptr.h"
#include "gem/i915_gem_vm_bind.h"

#include "i915_drv.h"
#include "i915_trace.h"

#include "gen8_ppgtt.h"
#include "intel_context.h"
#include "intel_gt.h"
#include "intel_gt_regs.h"
#include "intel_tlb.h"
#include "intel_pagefault.h"
#include "uc/intel_guc.h"
#include "uc/intel_guc_fwif.h"

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

enum access_type {
	ACCESS_TYPE_READ = 0,
	ACCESS_TYPE_WRITE = 1,
	ACCESS_TYPE_ATOMIC = 2,
	ACCESS_TYPE_RESERVED = 3,
};

enum fault_type {
	NOT_PRESENT = 0,
	WRITE_ACCESS_VIOLATION = 1,
	ATOMIC_ACCESS_VIOLATION = 2,
};

void intel_gt_pagefault_process_cat_error_msg(struct intel_gt *gt, u32 guc_ctx_id)
{
	struct drm_device *drm = &gt->i915->drm;
	struct intel_guc *guc = &gt->uc.guc;
	struct intel_context *ce;
	char buf[11];

	ce = xa_load(&guc->context_lookup, guc_ctx_id);
	if (ce) {
		snprintf(buf, sizeof(buf), "%#04x", guc_ctx_id);
		intel_context_set_error(ce);
	} else {
		snprintf(buf, sizeof(buf), "n/a");
	}

	trace_intel_gt_cat_error(gt, buf);

	drm_err(drm, "GPU catastrophic memory error. GT: %d, GuC context: %s\n", gt->info.id, buf);
}

static u64 fault_va(u32 fault_data1, u32 fault_data0)
{
	return ((u64)(fault_data1 & FAULT_VA_HIGH_BITS) << GEN12_FAULT_VA_HIGH_SHIFT) |
	       ((u64)fault_data0 << GEN12_FAULT_VA_LOW_SHIFT);
}

int intel_gt_pagefault_process_page_fault_msg(struct intel_gt *gt, const u32 *msg, u32 len)
{
	struct drm_i915_private *i915 = gt->i915;
	u64 address;
	u32 fault_reg, fault_data0, fault_data1;

	if (GRAPHICS_VER(i915) < 12)
		return -EPROTO;

	if (len != GUC2HOST_NOTIFY_PAGE_FAULT_MSG_LEN)
		return -EPROTO;

	if (FIELD_GET(GUC2HOST_NOTIFY_PAGE_FAULT_MSG_0_MBZ, msg[0]) != 0)
		return -EPROTO;

	fault_reg = FIELD_GET(GUC2HOST_NOTIFY_PAGE_FAULT_MSG_1_ALL_ENGINE_FAULT_REG, msg[1]);
	fault_data0 = FIELD_GET(GUC2HOST_NOTIFY_PAGE_FAULT_MSG_2_FAULT_TLB_RD_DATA0, msg[2]);
	fault_data1 = FIELD_GET(GUC2HOST_NOTIFY_PAGE_FAULT_MSG_3_FAULT_TLB_RD_DATA1, msg[3]);

	address = fault_va(fault_data1, fault_data0);

	trace_intel_gt_pagefault(gt, address, fault_reg, fault_data1 & FAULT_GTT_SEL);

	drm_err(&i915->drm, "Unexpected fault\n"
			    "\tGT: %d\n"
			    "\tAddr: 0x%llx\n"
			    "\tAddress space%s\n"
			    "\tEngine ID: %u\n"
			    "\tSource ID: %u\n"
			    "\tType: %u\n"
			    "\tFault Level: %u\n"
			    "\tAccess type: %s\n",
			    gt->info.id,
			    address,
			    fault_data1 & FAULT_GTT_SEL ? "GGTT" : "PPGTT",
			    GEN8_RING_FAULT_ENGINE_ID(fault_reg),
			    RING_FAULT_SRCID(fault_reg),
			    GEN12_RING_FAULT_FAULT_TYPE(fault_reg),
			    RING_FAULT_LEVEL(fault_reg),
			    !!(fault_reg & GEN12_RING_FAULT_ACCESS_TYPE) ? "Write" : "Read");

	return 0;
}

static void print_recoverable_fault(struct recoverable_page_fault_info *info,
				    const char *reason, int ret)
{
	DRM_DEBUG_DRIVER("\n\t%s: error %d\n"
			 "\tASID: %d\n"
			 "\tVFID: %d\n"
			 "\tPDATA: 0x%04x\n"
			 "\tFaulted Address: 0x%08x_%08x\n"
			 "\tFaultType: %d\n"
			 "\tAccessType: %d\n"
			 "\tFaultLevel: %d\n"
			 "\tEngineClass: %d\n"
			 "\tEngineInstance: %d\n",
			 reason, ret,
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
		if (ret != -EDEADLK)
			DRM_ERROR("Cannot unmap obj(%d)\n", ret);
		return ret;
	}

	return i915_gem_object_migrate(obj, ww, ce, lmem_id, true);
}

static inline bool access_is_atomic(struct recoverable_page_fault_info *info)
{
	return (info->access_type == ACCESS_TYPE_ATOMIC);
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

static int validate_fault(struct i915_vma *vma, struct recoverable_page_fault_info *info)
{
	/* combined access_type and fault_type */
	enum {
		FAULT_READ_NOT_PRESENT = 0x0,
		FAULT_WRITE_NOT_PRESENT = 0x1,
		FAULT_ATOMIC_NOT_PRESENT = 0x2,
		FAULT_WRITE_ACCESS_VIOLATION = 0x5,
		FAULT_ATOMIC_ACCESS_VIOLATION = 0xa,
	} err_code;
	int err = 0;

	err_code = (info->fault_type << 2) | info->access_type;

	switch (err_code & 0xF) {
	case FAULT_READ_NOT_PRESENT:
		break;
	case FAULT_WRITE_NOT_PRESENT:
		if (i915_gem_object_is_readonly(vma->obj)) {
			pr_err("Write Access Violation: read only\n");
			err = -EACCES;
		}
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

static struct dma_fence *
handle_i915_mm_fault(struct intel_guc *guc,
		     struct recoverable_page_fault_info *info)
{
	struct intel_gt *gt = guc_to_gt(guc);
	struct dma_fence *fence = NULL;
	struct i915_address_space *vm;
	enum intel_region_id lmem_id;
	struct i915_gem_ww_ctx ww;
	struct i915_vma *vma;
	int err = 0;

	vm = faulted_vm(guc, info->asid);
	/* The active context [asid] is protected while servicing a fault */
	if (GEM_WARN_ON(!vm))
		return ERR_PTR(-ENOENT);

	if (!i915_vm_page_fault_enabled(vm))
		return ERR_PTR(-ENOENT);

	vma = i915_find_vma(vm, info->page_addr);
	if (!vma) {
		if (vm->has_scratch) {
			gen12_init_fault_scratch(vm,
						 info->page_addr,
						 BIT(vm->scratch_order + PAGE_SHIFT),
						 true);
			return NULL;
		}

		return ERR_PTR(-ENOENT);
	}

	trace_i915_mm_fault(gt->i915, vm, vma, info);

	err = validate_fault(vma, info);
	if (err)
		goto put_vma;

	/*
	 * With lots of concurrency to the same unbound VMA, HW will generate a storm
	 * of page faults. Test this upfront so that the redundant fault requests
	 * return as early as possible.
	 */
	if (i915_vma_is_bound(vma, PIN_RESIDENT))
		goto put_vma;

 retry_userptr:
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

	lmem_id = get_lmem_region_id(vma->obj, gt);
	if (i915_gem_object_should_migrate_lmem(vma->obj, lmem_id,
						access_is_atomic(info))) {
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

	err = i915_vma_bind(vma, &ww);
	if (!err && userptr_needs_rebind(vma->obj)) {
		i915_gem_ww_ctx_fini(&ww);
		goto retry_userptr;
	}
 err_ww:
	if (err == -EDEADLK) {
		err = i915_gem_ww_ctx_backoff(&ww);
		if (!err)
			goto retry;
	}

	i915_gem_ww_ctx_fini(&ww);
put_vma:
	if (i915_gem_object_is_userptr(vma->obj)) {
		if (err == -EAGAIN)
			/* Need to try again in the next page fault. */
			err = 0;
	}

	fence = i915_active_fence_get_or_error(&vma->active.excl);

	i915_vma_put(vma);
	__i915_vma_put(vma);

	return fence ?: ERR_PTR(err);
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

struct fault_reply {
	struct dma_fence_work base;
	struct recoverable_page_fault_info info;
	struct i915_sw_dma_fence_cb cb;
	struct intel_guc *guc;
	struct intel_gt *gt;
	intel_wakeref_t wakeref;
};

static int fault_work(struct dma_fence_work *work)
{
	return 0;
}

static int send_fault_reply(struct fault_reply *f)
{
	struct intel_guc_pagefault_reply pkt = {
		.dw0 =  FIELD_PREP(PAGE_FAULT_REPLY_VALID, 1) |
			FIELD_PREP(PAGE_FAULT_REPLY_SUCCESS,
				   f->info.fault_unsuccessful) |
			FIELD_PREP(PAGE_FAULT_REPLY_REPLY,
				   PAGE_FAULT_REPLY_ACCESS) |
			FIELD_PREP(PAGE_FAULT_REPLY_DESC_TYPE,
				   FAULT_RESPONSE_DESC) |
			FIELD_PREP(PAGE_FAULT_REPLY_ASID,
				   f->info.asid),

		.dw1 =  FIELD_PREP(PAGE_FAULT_REPLY_VFID,
				   f->info.vfid) |
			FIELD_PREP(PAGE_FAULT_REPLY_ENG_INSTANCE,
				   f->info.engine_instance) |
			FIELD_PREP(PAGE_FAULT_REPLY_ENG_CLASS,
				   f->info.engine_class) |
			FIELD_PREP(PAGE_FAULT_REPLY_PDATA,
				   f->info.pdata),
	};

	return intel_guc_send_pagefault_reply(f->guc, &pkt);
}

static void fault_complete(struct dma_fence_work *work)
{
	struct fault_reply *f = container_of(work, typeof(*f), base);

	if (work->dma.error) {
		print_recoverable_fault(&f->info,
					"Fault response: Unsuccessful",
					work->dma.error);
		f->info.fault_unsuccessful = true;
	}

	send_fault_reply(f);
	intel_gt_pm_put(f->gt, f->wakeref);
}

static const struct dma_fence_work_ops reply_ops = {
	.name = "pagefault",
	.work = fault_work,
	.complete = fault_complete,
};

int intel_pagefault_req_process_msg(struct intel_guc *guc,
				    const u32 *payload,
				    u32 len)
{
	struct fault_reply *reply;
	struct dma_fence *fence;

	if (unlikely(len != 4))
		return -EPROTO;

	reply = kzalloc(sizeof(*reply), GFP_KERNEL);
	if (!reply)
		return -ENOMEM;

	dma_fence_work_init(&reply->base, &reply_ops);
	get_fault_info(payload, &reply->info);
	reply->guc = guc;

	reply->gt = guc_to_gt(guc);
	reply->wakeref = intel_gt_pm_get(reply->gt);

	fence = handle_i915_mm_fault(guc, &reply->info);
	if (IS_ERR(fence)) {
		i915_sw_fence_set_error_once(&reply->base.chain, PTR_ERR(fence));
	} else if (fence) {
		__i915_sw_fence_await_dma_fence(&reply->base.chain, fence, &reply->cb);
		dma_fence_put(fence);
	}

	dma_fence_work_commit_imm(&reply->base);
	return 0;
}

const char *intel_pagefault_type2str(unsigned int type)
{
	static const char * const faults[] = {
		[NOT_PRESENT] = "not present",
		[WRITE_ACCESS_VIOLATION] = "write access violation",
		[ATOMIC_ACCESS_VIOLATION] = "atomic access violation",
	};

	if (type > ATOMIC_ACCESS_VIOLATION || !faults[type])
		return "invalid fault type";

	return faults[type];
}

const char *intel_access_type2str(unsigned int type)
{
	static const char * const access[] = {
		[ACCESS_TYPE_READ] = "read",
		[ACCESS_TYPE_WRITE] = "write",
		[ACCESS_TYPE_ATOMIC] = "atomic",
		[ACCESS_TYPE_RESERVED] = "reserved",
	};

	if (type > ACCESS_TYPE_RESERVED || !access[type])
		return "invalid access type";

	return access[type];
}

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

	if (!i915_vma_is_bound(vma, PIN_RESIDENT)) {
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

static void print_access_counter(struct acc_info *info)
{
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

static int handle_i915_acc(struct intel_guc *guc,
			   struct acc_info *info)
{
	struct intel_gt *gt = guc_to_gt(guc);
	struct i915_vma *vma;

	if (info->access_type) {
		print_access_counter(info);
		return 0;
	}

	vma = get_acc_vma(guc, info);
	if (!vma) {
		print_access_counter(info);
		DRM_DEBUG_DRIVER("get_acc_vma failed\n");
		return 0;
	}

	if (i915_gem_object_is_userptr(vma->obj)) {
		int err = i915_gem_object_userptr_submit_init(vma->obj);

		if (err) {
			print_access_counter(info);
			DRM_DEBUG_DRIVER("userptr_submit_init failed %d\n", err);
			goto put_vma;
		}
	}

	acc_migrate_to_lmem(gt, vma);

	if (i915_gem_object_is_userptr(vma->obj))
		i915_gem_object_userptr_submit_done(vma->obj);

	trace_intel_access_counter(gt, info);
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
}

int intel_access_counter_req_process_msg(struct intel_guc *guc, const u32 *payload, u32 len)
{
	struct acc_info info = {};

	if (unlikely(len != 4))
		return -EPROTO;

	get_access_counter_info((struct access_counter_desc *)payload, &info);
	return handle_i915_acc(guc, &info);
}
