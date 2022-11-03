// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "i915_drv.h"
#include "intel_uncore.h"
#include "gt/intel_gt_debug.h"
#include "gt/intel_gt_mcr.h"
#include "gt/intel_gt_regs.h"
#include "gt/intel_gt_pm.h"
#include "gt/intel_gt.h"

static int intel_gt_for_each_compute_slice_subslice_fw(struct intel_gt *gt,
						       int (*fn)(struct intel_gt *gt,
								 void *data,
								 unsigned int slice,
								 unsigned int subslice,
								 bool subslice_present),
						       void *data)
{
	struct intel_uncore * const uncore = gt->uncore;
	struct sseu_dev_info *sseu = &gt->info.sseu;
	unsigned int dss, group, instance;
	bool present;
	int lastdss;
	int ret = 0;

	GEM_WARN_ON(!intel_sseu_subslice_total(sseu));

	lockdep_assert_held(&uncore->lock);

	if (GRAPHICS_VER_FULL(gt->i915) >= IP_VER(12, 50))
		lastdss = intel_sseu_highest_xehp_dss(sseu->subslice_mask);
	else
		lastdss = sseu->max_slices * sseu->max_subslices - 1;

	for_each_possible_ss_steering(dss, gt, group, instance, present) {
		if (dss > lastdss)
			break;

		ret = fn(gt, data, group, instance, present);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * intel_gt_for_each_compute_slice_subslice - Walk slices and sublices with MCR
 *
 * @gt: pointer to struct intel_gt
 * @write: if writes are going to be done
 * @fn: callback function for each slice/subslice with flag if present
 * @data: arbitrary data to be used by the callback
 *
 * Return: 0 if walk completed. nonzero if the callback returned nonzero
 *
 */
int intel_gt_for_each_compute_slice_subslice(struct intel_gt *gt,
					     int (*fn)(struct intel_gt *gt,
						       void *data,
						       unsigned int slice,
						       unsigned int subslice,
						       bool subslice_present),
					     void *data)
{
	const enum forcewake_domains fw_domains = FORCEWAKE_RENDER | FORCEWAKE_GT;
	struct intel_uncore * const uncore = gt->uncore;
	intel_wakeref_t wakeref;
	int ret;

	with_intel_runtime_pm(gt->uncore->rpm, wakeref) {
		spin_lock_irq(&uncore->lock);
		intel_uncore_forcewake_get__locked(uncore, fw_domains);

		ret = intel_gt_for_each_compute_slice_subslice_fw(gt, fn, data);

		intel_uncore_forcewake_put__locked(uncore, fw_domains);
		spin_unlock_irq(&uncore->lock);
	}

	return ret;
}

static int read_first_attention_ss_fw(struct intel_gt *gt, void *data,
				      unsigned int group, unsigned int instance,
				      bool ss_present)
{
	unsigned int row;

	if (!ss_present)
		return 0;

	for (row = 0; row < 2; row++) {
		u32 val;

		val = intel_gt_mcr_read_fw(gt, TD_ATT(row), group, instance);

		if (val)
			return 1;
	}

	return 0;
}

/**
 * intel_gt_eu_threads_needing_attention - Query host attention
 *
 * @gt: pointer to struct intel_gt
 *
 * Return: 1 if threads waiting host attention.
 */

int intel_gt_eu_threads_needing_attention(struct intel_gt* gt)
{
	return intel_gt_for_each_compute_slice_subslice(gt,
							read_first_attention_ss_fw,
							NULL);
}
