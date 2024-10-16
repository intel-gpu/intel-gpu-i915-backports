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
#include "uc/intel_guc_ct.h"

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

static unsigned long local_clock_ns(unsigned int *cpu)
{
	unsigned long t;

	/*
	 * The local clock is only comparable on the local cpu. However,
	 * we don't want to disable preemption for the entirety of the busy
	 * spin but instead we use the preemption event as an indication
	 * that we have overstayed our welcome and should relinquish the CPU,
	 * to stop busywaiting and go to sleep.
	 */
	*cpu = get_cpu();
	t = local_clock();
	put_cpu();

	return t;
}

static bool busy_wait_stop(unsigned long timeout_ns, unsigned int cpu)
{
	unsigned int this_cpu;

	if (time_after(local_clock_ns(&this_cpu), timeout_ns))
		return true;

	/*
	 * Check if we were preempted off the cpu, or if something else is
	 * ready to run.  We don't immediately yield in that case, i.e. using
	 * need_resched() instead of cond_resched(), as we want to set up our
	 * interrupt prior to calling schedule()
	 */
	return this_cpu != cpu || need_resched();
}

static bool busy_wait(struct intel_gt *gt, u32 seqno, unsigned long timeout_ns)
{
	unsigned int cpu;

	/*
	 * Is this invalidation next in the queue?
	 *
	 * Don't waste cycles if we are not being served, we are better off
	 * sleeping while we wait for service.
	 */
	if (!tlb_seqno_passed(gt, seqno - 1))
		return false;

	timeout_ns += local_clock_ns(&cpu);
	do {
		intel_guc_ct_receive(&gt->uc.guc.ct);
		if (tlb_seqno_passed(gt, seqno))
			return true;
	} while (!busy_wait_stop(timeout_ns, cpu));

	return false;
}

void intel_gt_invalidate_tlb_sync(struct intel_gt *gt, u32 seqno, bool atomic)
{
	if (unlikely(!i915_seqno_passed(READ_ONCE(gt->tlb.next_seqno), seqno)))
		return;

	if (tlb_seqno_passed(gt, seqno))
		return;

	while (atomic) {
		intel_guc_ct_receive(&gt->uc.guc.ct);
		if (tlb_seqno_passed(gt, seqno))
			return;
	}

	/*
	 * Drain the recieve queue before sleeping in case the TLB invalidation
	 * was already completed and so we can avoid the context switch and
	 * wakeups. Normally the invalidations are very quick so we expect the
	 * reply before we perfomed the deferred sync.
	 */
	if (busy_wait(gt, seqno, 20 * NSEC_PER_USEC))
		return;

	wait_event_cmd(gt->tlb.wq, tlb_seqno_passed(gt, seqno),
		       intel_guc_ct_receive(&gt->uc.guc.ct), );
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
