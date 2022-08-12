// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "i915_drv.h"
#include "intel_uncore.h"
#include "gt/intel_gt_debug.h"
#include "gt/intel_gt_regs.h"
#include "gt/intel_gt_pm.h"
#include "gt/intel_gt.h"

static int intel_gt_for_each_compute_slice_subslice_fw(struct intel_gt *gt,
						       bool write,
						       int (*fn)(struct intel_gt *gt,
								 void *data,
								 unsigned int slice,
								 unsigned int subslice,
								 bool subslice_present),
						       void *data)
{
	struct intel_uncore * const uncore = gt->uncore;
	struct sseu_dev_info *sseu = &gt->info.sseu;
	intel_sseu_ss_mask_t subslice_mask = sseu->subslice_mask;
	unsigned int max_slices, max_subslices;
	unsigned int slice, subslice;
	u32 mcr_ss, mcr_old;
	int ret = 0;

	/*
	 * For newer hardware access we can't use sseu directly
	 * as it gives the simplistic view for userspace. We
	 * need a direct hardware access through mcr into hardware
	 * and thus we need to figure out the exact topology.
	 * Further, the eu attention bitmask delivery also needs
	 * to know if subslice is fused or not.
	 */
	if (GRAPHICS_VER_FULL(gt->i915) >= IP_VER(12, 60)) {
		max_subslices = GEN_DSS_PER_CSLICE;
		max_slices = DIV_ROUND_UP(intel_sseu_highest_xehp_dss(subslice_mask) + 1,
					  max_subslices);
	} else if (GRAPHICS_VER_FULL(gt->i915) >= IP_VER(12, 50)) {
		max_subslices = GEN_DSS_PER_GSLICE;
		max_slices = DIV_ROUND_UP(intel_sseu_highest_xehp_dss(subslice_mask) + 1,
					  max_subslices);
	} else {
		max_slices = sseu->max_slices;
		max_subslices = sseu->max_subslices;
	}

	GEM_WARN_ON(!intel_sseu_subslice_total(sseu));
	GEM_WARN_ON(!max_slices);
	GEM_WARN_ON(!max_subslices);

	lockdep_assert_held(&uncore->lock);

	mcr_old = intel_uncore_read_fw(uncore, GEN8_MCR_SELECTOR);

	for (slice = 0; slice < max_slices; slice++) {
		for (subslice = 0; subslice < max_subslices; subslice++) {
			if (GRAPHICS_VER(uncore->i915) >= 11)
				mcr_ss = GEN11_MCR_SLICE(slice) | GEN11_MCR_SUBSLICE(subslice);
			else
				mcr_ss = GEN8_MCR_SLICE(slice) | GEN8_MCR_SUBSLICE(subslice);

			/* Wa_22013088509 */
			if (!write && GRAPHICS_VER(gt->i915) >= 12)
				mcr_ss |= GEN11_MCR_MULTICAST;

			intel_uncore_write_fw(uncore, GEN8_MCR_SELECTOR, mcr_ss);

			/*
			 * hsdes: <pending>
			 *
			 * XXX: We observe on some 12gens that less attention
			 * bits are lit than it is expected. The wa is to kick
			 * the EU thread by writing anything to EU_CTL register.
			 * 0xf value in the EU SELECT field disables a read of
			 * the debug data. It is not intrusive, so there should be
			 * no ill effects as userspace does not manipulate this
			 * register. Keep it here for now as all callsites are
			 * interested in attentions.
			 */
			if (GRAPHICS_VER_FULL(gt->i915) >= IP_VER(12, 55)) {
				const u32 val =  FIELD_PREP(EU_CTL_EU_SELECT, 0xf);
				intel_uncore_write_fw(gt->uncore, EU_CTL, val);
			}

			ret = fn(gt, data, slice, subslice,
				 intel_sseu_has_subslice(sseu, 0,
							 max_subslices * slice + subslice));
			if (ret)
				goto out;
		}
	}

out:
	intel_uncore_write_fw(uncore, GEN8_MCR_SELECTOR, mcr_old);

	return ret;
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
					     bool write,
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

		ret = intel_gt_for_each_compute_slice_subslice_fw(gt, write, fn, data);

		intel_uncore_forcewake_put__locked(uncore, fw_domains);
		spin_unlock_irq(&uncore->lock);
	}

	return ret;
}

static int read_first_attention_ss_fw(struct intel_gt *gt, void *data,
				      unsigned int slice, unsigned int subslice,
				      bool ss_present)
{
	unsigned int row;

	if (!ss_present)
		return 0;

	for (row = 0; row < 2; row++) {
		u32 val;

		val = intel_uncore_read_fw(gt->uncore, TD_ATT(row));

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
	return intel_gt_for_each_compute_slice_subslice(gt, false,
							read_first_attention_ss_fw,
							NULL);
}
