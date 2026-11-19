/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef __INTEL_GT_DEBUG_
#define __INTEL_GT_DEBUG_

#define TD_EU_ATTENTION_MAX_ROWS 2u

#include "intel_gt_types.h"

#define INTEL_GT_ATTENTION_TIMEOUT_MS 100

int intel_gt_eu_threads_needing_attention(struct intel_gt *gt);
int intel_gt_wait_eu_thread_attention(struct intel_gt *gt,
				      void *bits,
				      unsigned int bitmap_size,
				      ktime_t *settle_ts,
				      const unsigned int timeout_ms);

int intel_gt_for_each_compute_slice_subslice(struct intel_gt *gt,
					     int (*fn)(struct intel_gt *gt,
						       void *data,
						       unsigned int slice,
						       unsigned int subslice,
						       bool subslice_present),
					     void *data);

int intel_gt_eu_attention_bitmap_size(struct intel_gt *gt);
int intel_gt_eu_attention_bitmap(struct intel_gt *gt, u8* bits, unsigned int size);
int intel_gt_eu_status_bitmap(struct intel_gt *gt, u8 *bits, unsigned int size);

bool intel_gt_eu_idle(struct intel_gt *gt);

int intel_gt_eu_threads_faulted(struct intel_gt *gt);
int intel_gt_eu_threads_dispatched(struct intel_gt *gt);

int intel_gt_invalidate_l3_mmio(struct intel_gt *gt);

#endif
