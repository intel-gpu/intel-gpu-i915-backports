// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <linux/pm_qos.h>

#include "gem/i915_gem_lmem.h"
#include "gem/i915_gem_mman.h"
#include "gem/i915_gem_vm_bind.h"

#include "i915_drv.h"
#include "i915_driver.h"
#include "i915_trace.h"

#include "gen8_engine_cs.h"
#include "gen8_ppgtt.h"
#include "intel_context.h"
#include "intel_engine_heartbeat.h"
#include "intel_engine_regs.h"
#include "intel_gt.h"
#include "intel_gt_debug.h"
#include "intel_gt_mcr.h"
#include "intel_gt_print.h"
#include "intel_gt_regs.h"
#include "intel_tlb.h"
#include "intel_pagefault.h"
#include "uc/intel_guc.h"
#include "uc/intel_guc_fwif.h"
#include "i915_debugger.h"

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

enum fault_type {
	NOT_PRESENT = 0,
	WRITE_ACCESS_VIOLATION = 1,
	ATOMIC_ACCESS_VIOLATION = 2,
};

void intel_gt_pagefault_process_cat_error_msg(struct intel_gt *gt, u32 guc_ctx_id)
{
	char name[TASK_COMM_LEN + 32];
	struct i915_gem_context *ctx;
	struct intel_context *ce;
	char buf[80] = "";

	rcu_read_lock();
	ctx = NULL;
	ce = xa_load(&gt->uc.guc.context_lookup, guc_ctx_id);
	if (ce && intel_context_is_schedulable(ce))
		ctx = rcu_dereference(ce->gem_context);
	if (ctx) {
		snprintf(name, sizeof(name),
			 "%s (%s)", ctx->name, ce->engine->name);
		if (test_bit(1, (unsigned long *)&ctx->fault.addr))
			snprintf(buf, sizeof(buf), ", following user pagefault @ 0x%llx", ctx->fault.addr & ~3);

		atomic_inc(&ctx->guilty_count);
		intel_context_ban(ce, NULL);
	}
	rcu_read_unlock();
	if (!ctx) /* do not alarm users for injected CAT errors (context revocation) */
		return;

	trace_intel_gt_cat_error(gt, name);
	dev_notice(gt->i915->drm.dev,
		   "Catastrophic memory error in context %s%s\n",
		   name, buf);
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

	gt_notice_ratelimited(gt,
			      "Unexpected fault\n"
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
			      RING_FAULT_FAULT_TYPE(fault_reg),
			      RING_FAULT_LEVEL(fault_reg),
			      !!(fault_reg & RING_FAULT_ACCESS_TYPE) ? "Write" : "Read");
	return 0;
}

static void print_recoverable_fault(struct intel_gt *gt,
				    struct recoverable_page_fault_info *info,
				    const char *reason, int ret)
{
	gt_dbg(gt, "%s: error %d\n"
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

static int migrate_to_lmem(struct drm_i915_gem_object *obj,
			   const struct intel_memory_region *mem,
			   struct i915_gem_ww_ctx *ww)
{
	int ret;

	if (obj->mm.region.mem == mem)
		return 0;

	ret = i915_gem_object_unbind(obj, ww, I915_GEM_OBJECT_UNBIND_ACTIVE);
	if (ret)
		return ret;

	return i915_gem_object_migrate(obj, mem->id, true);
}

static inline bool access_is_atomic(struct recoverable_page_fault_info *info)
{
	return (info->access_type == ACCESS_TYPE_ATOMIC);
}

static struct intel_memory_region *
get_lmem(struct drm_i915_gem_object *obj, struct intel_gt *gt)
{
	int i;

	if (obj->mm.preferred_region &&
	    obj->mm.preferred_region->type == INTEL_MEMORY_LOCAL)
		return obj->mm.preferred_region;

	if (BIT(gt->lmem->id) & obj->memory_mask)
		return gt->lmem;

	for (i = 0; i < obj->mm.n_placements; i++) {
		struct intel_memory_region *mr = obj->mm.placements[i];

		if (mr->type == INTEL_MEMORY_LOCAL)
			return mr;
	}

	return NULL;
}

static int validate_fault(struct drm_i915_private *i915, struct i915_vma *vma,
			  struct recoverable_page_fault_info *info)
{
	/* combined access_type and fault_type */
	enum {
		FAULT_READ_NOT_PRESENT = 0x0,
		FAULT_WRITE_NOT_PRESENT = 0x1,
		FAULT_ATOMIC_NOT_PRESENT = 0x2,
		FAULT_WRITE_ACCESS_VIOLATION = 0x5,
		FAULT_ATOMIC_ACCESS_VIOLATION = 0xa,
	} err_code = info->fault_type << 2 | info->access_type;
	const char *err = NULL;

	switch (err_code & 0xf) {
	case FAULT_READ_NOT_PRESENT:
		break;
	case FAULT_WRITE_NOT_PRESENT:
		if (test_bit(I915_MM_NODE_READONLY_BIT, &vma->node.flags) ||
		    i915_gem_object_is_readonly(vma->obj))
			err = "Write Access Violation: read only";
		break;
	case FAULT_ATOMIC_NOT_PRESENT:
		/*
		 * This case is early detection of ATOMIC ACCESS_VIOLATION.
		 *
		 * Imported (dma-buf) objects do not have a memory_mask (or
		 * placement list), so allow the NOT_PRESENT fault to proceed
		 * as we cannot test placement list.
		 * The replayed memory access will catch a true ATOMIC
		 * ACCESS_VIOLATION and fail appropriately.
		 */
		if (!vma->obj->memory_mask)
			break;
		fallthrough;
	case FAULT_ATOMIC_ACCESS_VIOLATION:
		if (!(vma->obj->memory_mask & REGION_LMEM))
			err = "Atomic Access Violation";
		break;
	case FAULT_WRITE_ACCESS_VIOLATION:
		err = "Write Access Violation";
		break;
	default:
		err = "Undefined Fault Type";
		break;
	}

	if (err) {
		dev_notice(i915->drm.dev, "%s @ 0x%llx\n", err,
			   intel_canonical_addr(INTEL_PPGTT_MSB(i915), info->page_addr));
		return -EACCES;
	}

	return 0;
}

static struct i915_address_space *__faulted_vm(struct intel_gt *gt, u32 asid)
{
	if (GEM_WARN_ON(asid >= I915_MAX_ASID))
		return NULL;

	return xa_load(&gt->i915->asid_resv.xa, asid);
}

static struct i915_address_space *faulted_vm(struct intel_gt *gt, u32 asid)
{
	struct i915_address_space *vm;

	/* The active context [asid] is protected while servicing a fault */
	rcu_read_lock();
	vm = __faulted_vm(gt, asid);
	if (vm && atomic_read(&vm->open) && atomic_read(&vm->active_contexts[gt->info.id]))
		vm = i915_vm_get(vm);
	else
		vm = NULL;
	rcu_read_unlock();

	return vm;
}

static struct intel_engine_cs *
lookup_engine(struct intel_gt *gt, u8 class, u8 instance)
{
	if (class >= ARRAY_SIZE(gt->engine_class) ||
	    instance >= ARRAY_SIZE(gt->engine_class[class]))
		return NULL;

	return gt->engine_class[class][instance];
}

static struct intel_engine_cs *
mark_engine_as_active(struct intel_gt *gt,
		      int engine_class, int engine_instance)
{
	struct intel_engine_cs *engine;

	engine = lookup_engine(gt, engine_class, engine_instance);
	if (!engine)
		return NULL;

	WRITE_ONCE(engine->stats.irq.count,
		   READ_ONCE(engine->stats.irq.count) + 1);

	return engine;
}

static struct i915_gpu_coredump *
pf_coredump(struct intel_engine_cs *engine, struct recoverable_page_fault_info *info)
{
	struct i915_gpu_coredump *error;

	error = i915_gpu_coredump_create_for_engine(engine, GFP_KERNEL);
	if (!error)
		return NULL;

	error->fault.addr =
		intel_canonical_addr(INTEL_PPGTT_MSB(engine->i915),
				     info->page_addr | BIT(0));
	error->fault.type = info->fault_type;
	error->fault.level = info->fault_level;
	error->fault.access = info->access_type;

	return error;
}

struct fault_reply {
	struct dma_fence_work base;
	struct recoverable_page_fault_info info;
	struct i915_debugger_pagefault *debugger;
	struct i915_gpu_coredump *dump;
	struct intel_engine_cs *engine;
	struct i915_address_space *vm;
	struct i915_request *request;
	struct intel_guc *guc;
	struct intel_gt *gt;
	struct pm_qos_request qos;
	intel_wakeref_t wakeref;
	unsigned int epoch;
};

static bool has_debug_sip(struct intel_gt *gt)
{
	/*
	 * When debugging is enabled, we want to enter the SIP after resolving
	 * the pagefault and read the attention bits from the SIP. In this case,
	 * we must always use a scratch page for the invalid fault so that we
	 * can enter the sip and not retrigger more faults.
	 *
	 * After capturing the attention bits, we can restore the faulting
	 * vma (if required).
	 *
	 * XXX maybe intel_context_has_debug()?
	 */
	return intel_gt_mcr_read_any(gt, TD_CTL);
}

static struct i915_debugger_pagefault *
pf_eu_debugger(struct i915_address_space *vm, struct fault_reply *reply)
{
	struct recoverable_page_fault_info *info = &reply->info;
	struct i915_debugger_pagefault *pf;
	struct intel_gt *gt = reply->gt;
	struct dma_fence *prev;

	/*
	 * If there is no debug functionality (TD_CTL_GLOBAL_DEBUG_ENABLE, etc.),
	 * don't proceed pagefault routine for eu debugger.
	 */
	if (!has_debug_sip(gt))
		return NULL;

	pf = kzalloc(sizeof(*pf), GFP_KERNEL);
	if (!pf)
		return NULL;

	/*
	 * XXX only the first fault will try to resolve attn
	 * Typically lots of eu run the same instruction,
	 * the additional page faults might be generated before i915 set TD_CTL
	 * with FEH/FE. And the HW/guc is able to queue a lot of pagefault messages.
	 * If the pagefault handler serializes all pagefaults at this point,
	 * the serialization breaks TD_CTL attn discovery since the thread is
	 * not immediately resumed on the first fault reply.
	 * So while processing pagefault WA, skip processing of followed
	 * HW pagefault event that happens before FEH/FE is set.
	 * Due to this way, hw pagefault events from GuC might not pass
	 * transparently to debugUMD. But the eu thread where the pagefault
	 * occurred is combined into the threads list of page fault events
	 * passed to debugUMD. And as FEH & FE are set, the gpu thread will jump
	 * to SIP, blocking further pagefault occurrences. When FEH/FE is unset
	 * at the end of the page fault handler, additional page faults are
	 * allowed to occur.
	 */
	mutex_lock(&gt->eu_debug.lock); /* serialise with i915_debugger */
	prev = __i915_active_fence_fetch_set(&gt->eu_debug.fault, &reply->base.rq.fence);
	mutex_unlock(&gt->eu_debug.lock);
	if (prev) {
		dma_fence_work_chain(&reply->base, prev);
		dma_fence_put(prev);
	}

	INIT_LIST_HEAD(&pf->list);

	/* Assume that the request may be retired before any delayed event processing */
	pf->context = intel_context_get(reply->request->context);
	pf->engine = reply->engine;
	pf->fault.addr =
		intel_canonical_addr(INTEL_PPGTT_MSB(vm->i915),
				     info->page_addr | BIT(0));
	pf->fault.type = info->fault_type;
	pf->fault.level = info->fault_level;
	pf->fault.access = info->access_type;

	return pf;
}

static u32 fault_size(const struct recoverable_page_fault_info *info)
{
	switch (info->fault_level) {
	case 0: return SZ_4K;
	case 1: return SZ_2M;
	default:
	case 2: return SZ_1G;
	}
}

static int scratch_fault(struct i915_address_space *vm,
			 const struct recoverable_page_fault_info *info)
{
	u64 size = fault_size(info);
	u64 addr = info->page_addr & -size;

	vm->fault_start = min(vm->fault_start, addr);
	vm->fault_end = max(vm->fault_end, addr + size);
	return pvc_ppgtt_fault(vm, addr, size, true);
}

static void repair_fault(struct i915_address_space *vm,
			 const struct recoverable_page_fault_info *info)
{
	u64 addr, size;
	u32 seqno;

	if (vm->has_scratch)
		return;

	size = fault_size(info);
	addr = info->page_addr & -size;
	vm->clear_range(vm, addr, size);

	seqno = intel_gt_invalidate_tlb_range(vm->gt, vm, addr, size);
	i915_vm_heal_scratch(vm, addr, addr + size);
	intel_gt_invalidate_tlb_sync(vm->gt, seqno, false);
}

static void
track_invalid_userfault(struct fault_reply *reply)
{
	struct intel_engine_cs *engine = reply->engine;
	struct i915_gem_context *ctx = NULL;
	struct i915_request *rq;

	local_inc(&engine->gt->stats.pagefault_invalid);

	rcu_read_lock();
	rq = reply->request;
	if (rq)
		ctx = rcu_dereference(rq->context->gem_context);
	if (ctx && !test_and_set_bit(0, (unsigned long *)&ctx->fault.addr)) {
		ctx->fault.type = reply->info.fault_type;
		ctx->fault.level = reply->info.fault_level;
		ctx->fault.access = reply->info.access_type;
		smp_wmb();

		WRITE_ONCE(ctx->fault.addr,
			   intel_canonical_addr(INTEL_PPGTT_MSB(engine->i915),
						reply->info.page_addr | BIT(1) | BIT(0)));
	}
	rcu_read_unlock();
}

static struct i915_request *
find_faulting_request(struct intel_engine_cs *engine, struct i915_address_space *vm)
{
	struct i915_sched_engine *se = engine->sched_engine;
	struct i915_request *rq, *active = NULL;
	unsigned long flags;
	u32 lrc;

	if (READ_ONCE(engine->pagefault_request)) {
		rcu_read_lock();
		rq = READ_ONCE(engine->pagefault_request);
		if (rq && (i915_request_signaled(rq) || rq->context->vm != vm))
			rq = NULL;
		rcu_read_unlock();
		if (rq)
			return rq;
	}

	lrc = 0;
	if (!IS_SRIOV_VF(engine->i915))
		lrc = ENGINE_READ(engine, RING_CURRENT_LRCA);

	spin_lock_irqsave(&se->lock, flags);
	list_for_each_entry(rq, &se->requests, sched.link) {
		if (rq->context->vm != vm)
			continue;

		if (!(rq->execution_mask & engine->mask))
			continue;

		if (lrc & CURRENT_LRCA_VALID &&
		    (rq->context->lrc.lrca ^ lrc) & GENMASK(31, 12))
			continue;

		if (__i915_request_is_complete(rq))
			continue;

		if (__i915_request_has_started(rq)) {
			if (intel_context_is_schedulable(rq->context))
				active = rq;
			break;
		}
	}
	spin_unlock_irqrestore(&se->lock, flags);

	WRITE_ONCE(engine->pagefault_request, active);
	return active;
}

static struct dma_fence *
handle_i915_mm_fault(struct intel_guc *guc, struct fault_reply *reply)
{
	struct recoverable_page_fault_info *info = &reply->info;
	struct intel_gt *gt = guc_to_gt(guc);
	struct dma_fence *fence = NULL;
	struct i915_gem_ww_ctx ww;
	struct i915_vma *vma;
	int err = 0;

	vma = NULL;
	if (i915_vm_page_fault_enabled(reply->vm)) {
		vma = i915_find_vma(reply->vm, info->page_addr);
		trace_i915_mm_fault(reply->vm, vma, info);
	}
	if (!vma) {
		struct intel_engine_cs *engine = reply->engine;

		track_invalid_userfault(reply);

		/* Each EU thread may trigger its own pf to the same address! */
		if (intel_context_set_coredump(reply->request->context)) {
			/*
			 * The crux of this code is the same for offline/online.
			 *
			 * The current differences are that for offline we record
			 * a few more registers (not a bit deal of online) and
			 * that for online we are more careful and protect
			 * concurrent TD_CTL modifications.
			 * The latter safeguard would be an improvement for offline
			 * and the extra mmio reads lost in the noise for online.
			 *
			 * Then during fault_complete we decide if there's
			 * a debugger attached to send the event, or if not
			 * we complete and save the coredump for posterity.
			 */
			intel_engine_park_heartbeat(engine); /* restart after the fault */
			if (i915_debugger_active_on_context(reply->request->context))
				reply->debugger = pf_eu_debugger(reply->vm, reply);
			if (!reply->debugger)
				reply->dump = pf_coredump(engine, info);
		}

		if (has_debug_sip(reply->gt))
			return NULL; /* jump to fault_complete() (and queue) */

		if (reply->vm->has_scratch) {
			/*
			 * Map the out-of-bound access to scratch page.
			 *
			 * Out-of-bound virtual address range is not tracked,
			 * so whenever we bind a new vma we do not know if it
			 * is replacing a scratch mapping, and so we must always
			 * flush the TLB of the vma's address range so that the
			 * next access will not load scratch.
			 *
			 * This is an exceptional path to ease userspace development.
			 * Once user space fixes all the out-of-bound access, this
			 * logic will be removed.
			 */
			return ERR_PTR(scratch_fault(reply->vm, info));
		}

		return ERR_PTR(-EFAULT);
	}

	err = validate_fault(gt->i915, vma, info);
	if (err) {
		track_invalid_userfault(reply);
		goto put_vma;
	}

	if (unlikely(test_bit(I915_VMA_ERROR_BIT, __i915_vma_flags(vma)))) {
		err = -EFAULT;
		goto put_vma;
	}

	/*
	 * With lots of concurrency to the same unbound VMA, HW will generate a storm
	 * of page faults. Test this upfront so that the redundant fault requests
	 * return as early as possible.
	 */
	if (i915_vma_is_bound(vma, PIN_RESIDENT) && i915_gem_object_is_lmem(vma->obj))
		goto put_vma;

	for_i915_gem_ww(&ww, err, false) {
		struct drm_i915_gem_object *obj = vma->obj;
		const struct intel_memory_region *mem;

		err = i915_gem_object_lock(obj, &ww);
		if (err)
			continue;

		obj->flags |= I915_BO_FAULT_CLEAR | I915_BO_SYNC_HINT;

		mem = get_lmem(obj, gt);
		if (mem && i915_gem_object_should_migrate_lmem(obj, mem, access_is_atomic(info))) {
			/*
			 * Migration is best effort.
			 * if we see -EDEADLK handle that with proper backoff. Otherwise
			 * for scenarios like atomic operation, if migration fails,
			 * gpu will fault again and we can retry.
			 */
			err = migrate_to_lmem(obj, mem, &ww);
			if (err == -EDEADLK)
				continue;
		}

		err = 0;
		if (!i915_vma_is_bound(vma, PIN_RESIDENT))
			err = i915_vma_bind(vma);
	}
	local_inc(&gt->stats.pagefault_major);

put_vma:
	fence = i915_active_fence_get_or_error(&vma->active.excl);
	i915_vma_put(vma);
	__i915_vma_put(vma);

	return fence ?: ERR_PTR(err);
}

static void
get_fault_info(struct intel_gt *gt,
	       const u32 *payload,
	       struct recoverable_page_fault_info *info)
{
	const struct intel_guc_pagefault_desc *desc =
		(const struct intel_guc_pagefault_desc *)payload;

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

	info->page_addr =
		intel_noncanonical_addr(INTEL_PPGTT_MSB(gt->i915),
					make_u64(desc->dw3, desc->dw2 & PAGE_FAULT_DESC_VIRTUAL_ADDR_LO));
}

static int fault_work(struct dma_fence_work *work)
{
	return 0;
}

static int send_fault_reply(const struct fault_reply *f, bool imm)
{
	u32 action[] = {
		INTEL_GUC_ACTION_PAGE_FAULT_RES_DESC,

		(FIELD_PREP(PAGE_FAULT_REPLY_VALID, 1) |
		 FIELD_PREP(PAGE_FAULT_REPLY_SUCCESS,
			    f->info.fault_unsuccessful) |
		 FIELD_PREP(PAGE_FAULT_REPLY_REPLY,
			    PAGE_FAULT_REPLY_ACCESS) |
		 FIELD_PREP(PAGE_FAULT_REPLY_DESC_TYPE,
			    FAULT_RESPONSE_DESC) |
		 FIELD_PREP(PAGE_FAULT_REPLY_ASID,
			    f->info.asid)),

		(FIELD_PREP(PAGE_FAULT_REPLY_VFID,
			    f->info.vfid) |
		 FIELD_PREP(PAGE_FAULT_REPLY_ENG_INSTANCE,
			    f->info.engine_instance) |
		 FIELD_PREP(PAGE_FAULT_REPLY_ENG_CLASS,
			    f->info.engine_class) |
		 FIELD_PREP(PAGE_FAULT_REPLY_PDATA,
			    f->info.pdata)),
	};
	unsigned int flags;
	int err;

	if (f->epoch != f->gt->uc.epoch)
		return 0;

	flags = 0;
	if (imm)
		flags = MAKE_SEND_FLAGS(0);

	do {
		err = intel_guc_ct_send(&f->guc->ct, action, ARRAY_SIZE(action),
					NULL, 0, flags);
		if (!err || err == -ENODEV) /* ENODEV == GT is being reset */
			return 0;
		if (flags) {
			err = -EIO;
			flags = 0;
		}
	} while (err == -EIO); /* EIO == ack from HW timeout (by GuC), try again */

	return err;
}

static void revoke_faulting_context(struct intel_engine_cs *engine, struct i915_request *rq)
{
	char msg[TASK_COMM_LEN + 32] = "Incomplete pagefault response";
	struct i915_gem_context *ctx;

	rcu_read_lock();
	ctx = NULL;
	if (rq)
		ctx = rcu_dereference(rq->context->gem_context);
	if (ctx) {
		int len = strlen(msg);

		snprintf(msg + len, sizeof(msg) - len,
			 " for %s (%s)",
			 ctx->name, engine->name);

		atomic_inc(&ctx->guilty_count);
		intel_context_ban(rq->context, rq);
	}
	rcu_read_unlock();

	if (!work_pending(&engine->reset.work)) {
		memcpy(engine->reset.msg, msg, sizeof(msg));
		schedule_work(&engine->reset.work);
	}
}

static void fault_complete(struct dma_fence_work *work)
{
	struct fault_reply *f = container_of(work, typeof(*f), base);
	struct intel_engine_capture_vma *vma = NULL;
	struct i915_page_compress *compress = NULL;
	ktime_t start;

	if (f->dump) {
		struct intel_gt_coredump *gt = f->dump->gt;

		compress = i915_vma_capture_prepare(gt);
		if (compress) {
			vma = intel_engine_coredump_add_request(gt->engine, f->request, vma, GFP_KERNEL, compress);
			vma = intel_gt_coredump_add_other_engines(gt, f->request, vma, GFP_KERNEL, compress);
		}

		if (has_debug_sip(f->gt))
			scratch_fault(f->vm, &f->info);
	}

	if (f->debugger) {
		struct i915_debugger_pagefault *pf = f->debugger;
		u32 td_ctl;

		td_ctl = intel_gt_mcr_read_any(f->gt, TD_CTL);
		if (td_ctl) {
			intel_eu_attentions_read(f->gt, &pf->attentions.before, 0);

			/* Halt on next thread dispatch */
			while (!(td_ctl & TD_CTL_FORCE_EXTERNAL_HALT)) {
				intel_gt_mcr_multicast_write(f->gt, TD_CTL, td_ctl | TD_CTL_FORCE_EXTERNAL_HALT);
				/*
				 * The sleep is needed because some interrupts are ignored
				 * by the HW, hence we allow the HW some time to acknowledge
				 * that.
				 */
				udelay(200);

				td_ctl = intel_gt_mcr_read_any(f->gt, TD_CTL);
			}

			/* Halt regardless of thread dependencies */
			while (!(td_ctl & TD_CTL_FORCE_EXCEPTION)) {
				intel_gt_mcr_multicast_write(f->gt, TD_CTL, td_ctl | TD_CTL_FORCE_EXCEPTION);
				udelay(200);

				td_ctl = intel_gt_mcr_read_any(f->gt, TD_CTL);
			}

			intel_eu_attentions_read(f->gt, &pf->attentions.after,
						 INTEL_GT_ATTENTION_TIMEOUT_MS);

			scratch_fault(f->vm, &f->info);
		}
	}

	if (work->rq.fence.error) {
		print_recoverable_fault(f->gt, &f->info,
					"Fault response: Unsuccessful",
					work->rq.fence.error);
		f->info.fault_unsuccessful = true;
	}

	if (atomic_read(&f->engine->in_pagefault) == 1)
		WRITE_ONCE(f->engine->pagefault_request, NULL);

	/*
	 * While Pagefault WA processing, i915 have to need to reply to the GuC
	 * first, then i915 can read properly the thread attentions (resolved
	 * -attentions) that SIP turns on.
	 */
	if (GEM_WARN_ON(send_fault_reply(f, test_bit(DMA_FENCE_WORK_IMM, &work->rq.fence.flags))))
		revoke_faulting_context(f->engine, f->request);

	start = READ_ONCE(f->engine->pagefault_start);
	if (atomic_dec_and_test(&f->engine->in_pagefault))
		local64_add(ktime_get() - start, &f->gt->stats.pagefault_stall);

	if (f->dump) {
		struct intel_gt_coredump *gt = f->dump->gt;
		u32 td_ctl;

		td_ctl = intel_gt_mcr_read_any(f->gt, TD_CTL);
		if (td_ctl) {
			intel_eu_attentions_read(f->gt, &gt->attentions.resolved,
						 INTEL_GT_ATTENTION_TIMEOUT_MS);

			repair_fault(f->vm, &f->info);

			/* No more exceptions, stop raising new ATTN */
			td_ctl &= ~(TD_CTL_FORCE_EXTERNAL_HALT | TD_CTL_FORCE_EXCEPTION);
			intel_gt_mcr_multicast_write(f->gt, TD_CTL, td_ctl);

			/* Reset and cleanup if there are any ATTN leftover */
			intel_engine_schedule_heartbeat(f->engine);
		}

		if (vma)
			intel_engine_coredump_add_vma(gt->engine, vma, compress);

		if (compress)
			i915_vma_capture_finish(gt, compress);

		i915_error_state_store(f->dump);
		i915_gpu_coredump_put(f->dump);
	}

	if (f->debugger) {
		struct i915_debugger_pagefault *pf = f->debugger;
		u32 td_ctl;

		intel_eu_attentions_read(f->gt,
					 &pf->attentions.resolved,
					 INTEL_GT_ATTENTION_TIMEOUT_MS);

		/*
		 * Install the fault PTE;
		 * In order to get a pagefault again at the same address in the
		 * future, it clears the PTE of the page used as pagefault WA.
		 * If very many threads on the GPU are executing the same code
		 * and this code causes a pagefault, then this can cause
		 * a pagefault flood in the worst case.
		 */

		/* clear the PTE of pagefault address */
		intel_context_clear_coredump(pf->context);
		repair_fault(f->vm, &f->info);

		/* clear Force_External and Force Exception on pagefault scenario */
		td_ctl = intel_gt_mcr_read_any(f->gt, TD_CTL);
		intel_gt_mcr_multicast_write(f->gt, TD_CTL, td_ctl &
					     ~(TD_CTL_FORCE_EXTERNAL_HALT | TD_CTL_FORCE_EXCEPTION));

		intel_gt_invalidate_l3_mmio(f->gt);

		i915_debugger_handle_page_fault(pf);

		/* Restore ATTN scanning */
		intel_engine_schedule_heartbeat(f->engine);
	}

	cpu_latency_qos_remove_request(&f->qos);
	i915_vm_put(f->vm);
	intel_gt_pm_put_async(f->gt, f->wakeref);
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
	struct intel_gt *gt = guc_to_gt(guc);
	struct fault_reply *reply;
	struct dma_fence *fence;
	int err;

	if (unlikely(len != 4))
		return -EPROTO;

	reply = kzalloc(sizeof(*reply), GFP_KERNEL);
	if (unlikely(!reply))
		return -ENOMEM;

	dma_fence_work_init(&reply->base, &reply_ops, gt->i915->sched);
	get_fault_info(gt, payload, &reply->info);
	reply->epoch = gt->uc.epoch & ~INTEL_UC_IN_RESET;
	reply->guc = guc;

	reply->gt = gt;
	reply->wakeref = intel_gt_pm_get_if_awake(gt);
	if (unlikely(!reply->wakeref)) {
		err = -ENOENT;
		goto err_reply;
	}

	reply->vm = faulted_vm(gt, reply->info.asid);
	if (unlikely(!reply->vm)) {
		err = -ENOENT;
		goto err_wf;
	}

	reply->engine =
		mark_engine_as_active(gt,
				      reply->info.engine_class,
				      reply->info.engine_instance);
	if (unlikely(!reply->engine)) {
		err = -EINVAL;
		goto err_vm;
	}
	GEM_BUG_ON(reply->engine->gt != gt);

	reply->request = find_faulting_request(reply->engine, reply->vm);
	if (unlikely(!reply->request)) {
		err = -ENOENT;
		goto err_vm;
	}
	GEM_BUG_ON(reply->request->context->vm != reply->vm);

	local_inc(&gt->stats.pagefault_minor);
	if (!atomic_fetch_inc(&reply->engine->in_pagefault))
		reply->engine->pagefault_start = ktime_get();

	memset(&reply->qos, 0, sizeof(reply->qos));
	cpu_latency_qos_add_request(&reply->qos, 0);

	/*
	 * Keep track of the background work to migrate the backing store and
	 * bind the vma for the faulting address.
	 *
	 * We often see hundreds of concurrent pagefaults raised by a single EU
	 * kernel running on many hundreds of threads on a single engine.  If
	 * we sequentially process the vma binding and then each fault response
	 * that will consume a few milliseconds (roughly 20us per CT fault
	 * response message plus the millisecond or so required to handle the
	 * fault itself). Alternatively, we can reorder the fault replies to
	 * begin all the second responses while the migration and vma binding
	 * is in progress by processing the two halves as separate halves.
	 * (For simplicity, we submit all of the fault handlers as their own
	 * work as we do not know ahead of time how many pagefaults have been
	 * generated, and just let the CPU scheduler and HW handle the
	 * parallelism.)
	 *
	 * To mitigate against stalls when trying to submit a few hundred
	 * pagefault responses via the GuC CT, we make sure we have a
	 * sufficiently larger send (H2G) buffer to accommodate a typical
	 * number of messages (assuming the buffer is not already backlogged).
	 */
	fence = handle_i915_mm_fault(guc, reply);
	if (IS_ERR(fence)) {
		i915_sw_fence_set_error_once(&reply->base.rq.submit,
					     PTR_ERR(fence));
	} else if (fence) {
		dma_fence_work_chain(&reply->base, fence);
		dma_fence_put(fence);
	}

	i915_request_set_priority(&reply->base.rq, I915_PRIORITY_BARRIER);
	dma_fence_work_commit_imm_if(&reply->base, !reply->dump && !reply->debugger);

	/* Serialise each pagefault with its reply? */
	if (!IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_ASYNC_PAGEFAULTS))
		dma_fence_wait(&reply->base.rq.fence, false);

	return 0;

err_vm:
	i915_vm_put(reply->vm);
err_wf:
	intel_gt_pm_put_async(gt, reply->wakeref);
err_reply:
	kfree(reply);
	return err;
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

	vm = __faulted_vm(guc_to_gt(guc), info->asid);
	if (GEM_WARN_ON(!vm))
		return NULL;

	page_va = info->va_range_base + (ffs(info->sub_granularity) - 1)
		  * sub_granularity_in_byte(info->granularity);

	return i915_find_vma(vm, page_va);
}

const char *intel_acc_err2str(unsigned int err)
{
	static const char * const faults[] = {
		[ACCESS_ERR_OK] = "",
		[ACCESS_ERR_NOSUP] = "not supported",
		[ACCESS_ERR_NULLVMA] = "null vma",
		[ACCESS_ERR_USERPTR] = "userptr",
	};

	if (err >= ARRAY_SIZE(faults) || !faults[err])
		return "invalid acc err!";

	return faults[err];
}

static int acc_migrate_to_lmem(struct intel_gt *gt, struct i915_vma *vma)
{
	struct i915_gem_ww_ctx ww;
	int err;

	i915_gem_vm_bind_lock(vma->vm);

	if (!i915_vma_is_bound(vma, PIN_RESIDENT)) {
		i915_gem_vm_bind_unlock(vma->vm);
		return 0;
	}

	for_i915_gem_ww(&ww, err, false) {
		struct drm_i915_gem_object *obj = vma->obj;
		const struct intel_memory_region *mem;

		err = i915_gem_object_lock(obj, &ww);
		if (err)
			continue;

		mem = get_lmem(obj, gt);
		if (!mem)
			continue;

		err = migrate_to_lmem(obj, mem, &ww);
	}

	i915_gem_vm_bind_unlock(vma->vm);

	return err;
}

static int handle_i915_acc(struct intel_guc *guc,
			   struct acc_info *info)
{
	struct intel_gt *gt = guc_to_gt(guc);
	struct i915_vma *vma;

	mark_engine_as_active(gt, info->engine_class, info->engine_instance);

	if (info->access_type) {
		trace_intel_access_counter(gt, info, ACCESS_ERR_NOSUP);
		return 0;
	}

	vma = get_acc_vma(guc, info);
	if (!vma) {
		trace_intel_access_counter(gt, info, ACCESS_ERR_NULLVMA);
		return 0;
	}

	if (i915_gem_object_is_userptr(vma->obj)) {
		trace_intel_access_counter(gt, info, ACCESS_ERR_USERPTR);
		goto put_vma;
	}

	acc_migrate_to_lmem(gt, vma);

	trace_intel_access_counter(gt, info, ACCESS_ERR_OK);
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
