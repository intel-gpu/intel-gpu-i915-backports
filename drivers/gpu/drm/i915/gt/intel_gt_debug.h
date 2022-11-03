/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef __INTEL_GT_DEBUG_
#define __INTEL_GT_DEBUG_

#include "intel_gt_types.h"

int intel_gt_eu_threads_needing_attention(struct intel_gt *gt);

int intel_gt_for_each_compute_slice_subslice(struct intel_gt *gt,
					     int (*fn)(struct intel_gt *gt,
						       void *data,
						       unsigned int slice,
						       unsigned int subslice,
						       bool subslice_present),
					     void *data);
#endif
