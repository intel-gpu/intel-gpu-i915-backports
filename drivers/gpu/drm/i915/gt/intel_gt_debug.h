/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef __INTEL_GT_DEBUG_
#define __INTEL_GT_DEBUG_

#define TD_EU_ATTENTION_MAX_ROWS 2u

#include "intel_gt_types.h"

int intel_gt_eu_threads_needing_attention(struct intel_gt *gt);

int intel_gt_for_each_compute_slice_subslice(struct intel_gt *gt,
					     int (*fn)(struct intel_gt *gt,
						       void *data,
						       unsigned int slice,
						       unsigned int subslice,
						       bool subslice_present),
					     void *data);

int intel_gt_eu_attention_bitmap_size(struct intel_gt *gt);
int intel_gt_eu_attention_bitmap(struct intel_gt *gt,
				 u8* bits,
				 unsigned int bitmap_size);

int intel_gt_invalidate_l3_mmio(struct intel_gt *gt);

#endif
