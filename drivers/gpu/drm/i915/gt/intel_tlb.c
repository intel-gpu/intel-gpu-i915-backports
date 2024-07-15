// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "i915_drv.h"
#include "i915_trace.h"
#include "intel_gt.h"
#include "intel_gt_pm.h"
#include "intel_tlb.h"
#include "uc/intel_guc.h"

void intel_tlb_invalidation_revoke(struct intel_gt *gt)
{
	smp_store_mb(gt->tlb.seqno, READ_ONCE(gt->tlb.next_seqno));
	wake_up_all(&gt->tlb.wq);
}

static bool tlb_advance(u32 *slot, u32 seqno)
{
	u32 old = READ_ONCE(*slot);

	do {
		if (i915_seqno_passed(old, seqno))
			return false;
	} while (!try_cmpxchg(slot, &old, seqno));

	return true;
}

void intel_tlb_invalidation_done(struct intel_gt *gt, u32 seqno)
{
	if (seqno && tlb_advance(&gt->tlb.seqno, seqno))
		wake_up_all(&gt->tlb.wq);
}

static bool tlb_seqno_passed(const struct intel_gt *gt, u32 seqno)
{
	if (intel_gt_is_wedged(gt))
		return true;

	return i915_seqno_passed(READ_ONCE(gt->tlb.seqno), seqno);
}

void intel_gt_invalidate_tlb_sync(struct intel_gt *gt, u32 seqno)
{
	if (unlikely(!i915_seqno_passed(READ_ONCE(gt->tlb.next_seqno), seqno)))
		return;

	wait_event(gt->tlb.wq, tlb_seqno_passed(gt, seqno));
}

static u64 tlb_page_selective_size(u64 *addr, u64 length)
{
	const u64 end = *addr + length;
	u64 start;

	/*
	 * Minimum invalidation size for a 2MB page that the hardware expects is
	 * 16MB
	 */
	length = max_t(u64, roundup_pow_of_two(length), SZ_4K);
	if (length >= SZ_2M)
		length = max_t(u64, SZ_16M, length);

	/*
	 * We need to invalidate a higher granularity if start address is not
	 * aligned to length. When start is not aligned with length we need to
	 * find the length large enough to create an address mask covering the
	 * required range.
	 */
	start = round_down(*addr, length);
	while (start + length < end) {
		length <<= 1;
		start = round_down(*addr, length);
	}

	*addr = start;
	return length;
}

u32 intel_gt_invalidate_tlb_range(struct intel_gt *gt,
				  struct i915_address_space *vm,
				  u64 start, u64 length)
{
	intel_wakeref_t wakeref;
	u32 seqno = 0;

	if (intel_gt_is_wedged(gt) || gt->suspend)
		return 0;

	trace_intel_tlb_invalidate(gt, start, length);

	/* Align start and length */
	length = tlb_page_selective_size(&start, length);

	with_intel_gt_pm_if_awake(gt, wakeref) {
		seqno = intel_guc_invalidate_tlb_page_selective(&gt->uc.guc,
								INTEL_GUC_TLB_INVAL_MODE_HEAVY,
								start, length,
								vm->asid);
		if (likely(seqno))
			tlb_advance(&vm->tlb[gt->info.id], seqno);
	}

	return seqno;
}

void intel_gt_init_tlb(struct intel_gt *gt)
{
	gt->tlb.seqno = 0;
	gt->tlb.next_seqno = 0;
	init_waitqueue_head(&gt->tlb.wq);
	mutex_init(&gt->tlb.mutex);
}

void intel_gt_fini_tlb(struct intel_gt *gt)
{
	mutex_destroy(&gt->tlb.mutex);
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftest_tlb.c"
#endif
