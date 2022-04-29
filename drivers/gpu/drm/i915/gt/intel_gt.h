/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_GT__
#define __INTEL_GT__

#include <linux/compiler_attributes.h>
#include "intel_engine_types.h"
#include "intel_gt_types.h"
#include "intel_reset.h"
#include "i915_drv.h"

struct drm_i915_private;
struct drm_printer;

#define GT_TRACE(gt, fmt, ...) do {					\
	const struct intel_gt *gt__ __maybe_unused = (gt);		\
	GEM_TRACE("%s " fmt, dev_name(gt__->i915->drm.dev),		\
		  ##__VA_ARGS__);					\
} while (0)

static inline struct intel_gt *uc_to_gt(struct intel_uc *uc)
{
	return container_of(uc, struct intel_gt, uc);
}

static inline struct intel_gt *guc_to_gt(struct intel_guc *guc)
{
	return container_of(guc, struct intel_gt, uc.guc);
}

static inline struct intel_gt *huc_to_gt(struct intel_huc *huc)
{
	return container_of(huc, struct intel_gt, uc.huc);
}

static inline struct intel_gt *gsc_to_gt(struct intel_gsc *gsc)
{
	return container_of(gsc, struct intel_gt, gsc);
}

void intel_gt_init_early(struct intel_gt *gt, struct drm_i915_private *i915);
int intel_gt_init_mmio(struct intel_gt *gt);
int __must_check intel_gt_init_hw(struct intel_gt *gt);
void intel_gt_init_ggtt(struct intel_gt *gt, struct i915_ggtt *ggtt);
int intel_gt_init(struct intel_gt *gt);
void intel_gt_driver_register(struct intel_gt *gt);

void intel_gt_driver_unregister(struct intel_gt *gt);
void intel_gt_driver_remove(struct intel_gt *gt);
void intel_gt_driver_release(struct intel_gt *gt);

void intel_gt_driver_late_release(struct intel_gt *gt);

void intel_gt_shutdown(struct intel_gt *gt);

int intel_gt_wait_for_idle(struct intel_gt *gt, long timeout);

void intel_gt_check_and_clear_faults(struct intel_gt *gt);
void intel_gt_clear_error_registers(struct intel_gt *gt,
				    intel_engine_mask_t engine_mask);

void intel_gt_flush_ggtt_writes(struct intel_gt *gt);
void intel_gt_chipset_flush(struct intel_gt *gt);

static inline u32 intel_gt_scratch_offset(const struct intel_gt *gt,
					  enum intel_gt_scratch_field field)
{
	return i915_ggtt_offset(gt->scratch) + field;
}

static inline bool intel_gt_has_unrecoverable_error(const struct intel_gt *gt)
{
	return test_bit(I915_WEDGED_ON_INIT, &gt->reset.flags) ||
	       test_bit(I915_WEDGED_ON_FINI, &gt->reset.flags);
}

static inline bool intel_gt_is_wedged(const struct intel_gt *gt)
{
	GEM_BUG_ON(intel_gt_has_unrecoverable_error(gt) &&
		   !test_bit(I915_WEDGED, &gt->reset.flags));

	return unlikely(test_bit(I915_WEDGED, &gt->reset.flags));
}

static inline bool intel_gt_needs_read_steering(struct intel_gt *gt,
						enum intel_steering_type type)
{
	return gt->steering_table[type];
}

void intel_gt_get_valid_steering_for_reg(struct intel_gt *gt, i915_reg_t reg,
					 u8 *sliceid, u8 *subsliceid);

u32 intel_gt_read_register_fw(struct intel_gt *gt, i915_reg_t reg);
u32 intel_gt_read_register(struct intel_gt *gt, i915_reg_t reg);

void intel_gt_report_steering(struct drm_printer *p, struct intel_gt *gt,
			      bool dump_table);

static inline bool
i915_is_level4_wa_active(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;
	bool guc_ready = (!intel_guc_submission_is_wanted(&gt->uc.guc) ||
			  intel_guc_is_ready(&gt->uc.guc));

	return i915_is_mem_wa_enabled(i915, I915_WA_USE_FLAT_PPGTT_UPDATE) &&
		i915->bind_ctxt_ready && guc_ready &&
		!atomic_read(&i915->level4_wa_disabled);
}

int intel_gt_tiles_setup(struct drm_i915_private *i915);
int intel_gt_tiles_init(struct drm_i915_private *i915);
void intel_gt_tiles_cleanup(struct drm_i915_private *i915);

#define for_each_gt(i915__, id__, gt__) \
	for ((id__) = 0; \
	     (id__) < I915_MAX_TILES; \
	     (id__)++) \
		for_each_if (((gt__) = (i915__)->gts[(id__)]))

/*
 *  Wa_16015476723 & Wa_16015666671 Hold forcewake on GT0 & GT1
 *  to disallow rc6
 */
static inline void _pvc_wa_disallow_rc6(struct drm_i915_private *i915, bool enable, bool rpm_awake)
{
	unsigned int id;
	struct intel_gt *gt;
	intel_wakeref_t wakeref;
	void (*intel_uncore_forcewake)(struct intel_uncore *uncore,
				       enum forcewake_domains fw_domains);

	if (!i915->params.enable_rc6)
		return;

	if (!i915->params.rc6_ignore_steppings)
		return;

	/*
	 * GUC RC disallow override is sufficient to disallow rc6, But
	 * forcewake needs to be held till last active client disallows
	 * rc6, else rc6 will be allowed at intermediate level
	 */
	if (IS_PVC_BD_REVID(i915, PVC_BD_REVID_B0, STEP_FOREVER) && i915->remote_tiles > 0) {
		intel_uncore_forcewake = enable ? intel_uncore_forcewake_get :
						intel_uncore_forcewake_put;

		for_each_gt(i915, id, gt) {
			/* FIXME Remove static check and add dynamic check to avoid rpm helper */
			if (!rpm_awake) {
				/*
				 * Notify GuC to drop frequency to RPe when idle
				 * through GUC RC Disallow override event
				 */
				with_intel_runtime_pm(gt->uncore->rpm, wakeref) {
					intel_guc_slpc_gucrc_disallow(gt, enable);
					intel_uncore_forcewake(gt->uncore, FORCEWAKE_ALL);
				}
			} else {
				intel_guc_slpc_gucrc_disallow(gt, enable);
				intel_uncore_forcewake(gt->uncore, FORCEWAKE_ALL);
			}
		}
	}
}

static inline void pvc_wa_disallow_rc6(struct drm_i915_private *i915)
{
	_pvc_wa_disallow_rc6(i915, true, false);
}

static inline void pvc_wa_allow_rc6(struct drm_i915_private *i915)
{
	_pvc_wa_disallow_rc6(i915, false, false);
}

static inline void pvc_wa_disallow_rc6_if_awake(struct drm_i915_private *i915)
{
	_pvc_wa_disallow_rc6(i915, true, true);
}

static inline void pvc_wa_allow_rc6_if_awake(struct drm_i915_private *i915)
{
	_pvc_wa_disallow_rc6(i915, false, true);
}

void intel_gt_info_print(const struct intel_gt_info *info,
			 struct drm_printer *p);
int intel_gt_get_l3bank_count(struct intel_gt *gt);
bool intel_gt_has_eus(const struct intel_gt *gt);

void intel_gt_watchdog_work(struct work_struct *work);

void intel_boost_fake_int_timer(struct intel_gt *gt, bool on_off);

#endif /* __INTEL_GT_H__ */
