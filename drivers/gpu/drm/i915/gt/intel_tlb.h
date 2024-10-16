/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef INTEL_TLB_H
#define INTEL_TLB_H

#include <linux/seqlock.h>
#include <linux/types.h>

#include "intel_gt_types.h"

struct i915_address_space;

static inline u32 intel_tlb_next_seqno(struct intel_gt *gt)
{
	u32 old = READ_ONCE(gt->tlb.next_seqno);
	u32 seqno;

	do {
		seqno = old + 1;
		if (!seqno)
			seqno = 1;
	} while (!try_cmpxchg(&gt->tlb.next_seqno, &old, seqno));

	return seqno;
}

u32 intel_gt_invalidate_tlb_range(struct intel_gt *gt,
				  struct i915_address_space *vm,
				  u64 start, u64 length);
void intel_gt_invalidate_tlb_sync(struct intel_gt *gt, u32 seqno, bool atomic);

void intel_tlb_invalidation_done(struct intel_gt *gt, u32 seqno);
void intel_tlb_invalidation_revoke(struct intel_gt *gt);

void intel_gt_init_tlb(struct intel_gt *gt);
void intel_gt_fini_tlb(struct intel_gt *gt);

#endif /* INTEL_TLB_H */
