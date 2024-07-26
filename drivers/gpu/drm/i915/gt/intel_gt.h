/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_GT__
#define __INTEL_GT__

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

/*
 * Check that the GT is a graphics GT and has an IP version within the
 * specified range (inclusive).
 */
#define IS_GFX_GT_IP_RANGE(gt, from, until) ( \
	BUILD_BUG_ON_ZERO((from) < IP_VER(2, 0)) + \
	BUILD_BUG_ON_ZERO((until) < (from)) + \
	((gt)->type != GT_MEDIA && \
	 GRAPHICS_VER_FULL((gt)->i915) >= (from) && \
	 GRAPHICS_VER_FULL((gt)->i915) <= (until)))

#define NEEDS_FASTCOLOR_BLT_WABB(engine) ( \
	IS_GFX_GT_IP_RANGE(engine->gt, IP_VER(12, 55), IP_VER(12, 71)) && \
	engine->class == COPY_ENGINE_CLASS)

static inline bool gt_is_root(struct intel_gt *gt)
{
	return !gt->info.id;
}

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

static inline struct intel_gt *gsc_uc_to_gt(struct intel_gsc_uc *gsc_uc)
{
	return container_of(gsc_uc, struct intel_gt, uc.gsc);
}

static inline struct intel_gt *gsc_to_gt(struct intel_gsc *gsc)
{
	return container_of(gsc, struct intel_gt, gsc);
}

void intel_gt_common_init_early(struct intel_gt *gt);
int intel_root_gt_init_early(struct drm_i915_private *i915);
int intel_gt_init_mmio(struct intel_gt *gt);
int __must_check intel_gt_init_hw(struct intel_gt *gt);
void intel_gt_init_ggtt(struct intel_gt *gt, struct i915_ggtt *ggtt);
int intel_gt_init(struct intel_gt *gt);
void intel_gt_driver_register(struct intel_gt *gt);

void intel_gt_driver_unregister(struct intel_gt *gt);
void intel_gt_driver_remove(struct intel_gt *gt);
void intel_gt_driver_release(struct intel_gt *gt);
void intel_gt_driver_late_release_all(struct drm_i915_private *i915);

void intel_gt_shutdown(struct intel_gt *gt);

int intel_gt_wait_for_idle(struct intel_gt *gt, long timeout);

void intel_gt_check_and_clear_faults(struct intel_gt *gt);
void intel_gt_clear_error_registers(struct intel_gt *gt,
				    intel_engine_mask_t engine_mask);

static inline void intel_gt_chipset_flush(struct intel_gt *gt)
{
	wmb(); /* flush any chipset caches */
}

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

static inline
i915_reg_t intel_gt_perf_limit_reasons_reg(struct intel_gt *gt)
{
	if (gt->type == GT_MEDIA)
		return MTL_MEDIA_PERF_LIMIT_REASONS;

	return GT0_PERF_LIMIT_REASONS;
}

int intel_count_l3_banks(struct drm_i915_private *i915,
			 struct intel_engine_cs *engine);

int intel_gt_probe_all(struct drm_i915_private *i915);
int intel_gt_tiles_init(struct drm_i915_private *i915);

#define for_each_gt(gt__, i915__, id__) \
	for ((id__) = 0; \
	     (id__) < I915_MAX_GT; \
	     (id__)++) \
		for_each_if(((gt__) = (i915__)->gt[(id__)]))

static inline bool pvc_needs_rc6_wa(struct drm_i915_private *i915)
{
	if (!i915->params.enable_rc6)
		return false;

	if (IS_SRIOV_VF(i915))
		return false;

	if (i915->quiesce_gpu)
		return false;
	/*
	 * Lets not break the dpc recovery, which will be hindered
	 * by rpm resume caused by RC6 Wa
	 */
	if (i915_is_pci_faulted(i915))
		return false;

	return (IS_PVC_BD_STEP(i915, STEP_B0, STEP_FOREVER) && i915->remote_tiles > 0);
}

/* Wa_16015496043 Hold forcewake on GT0 & GT1 to disallow rc6 */
static inline void
_pvc_wa_disallow_rc6(struct drm_i915_private *i915,
		     void (*fn)(struct intel_uncore *uncore,
				enum forcewake_domains fw_domains))
{
	intel_wakeref_t wakeref;
	struct intel_gt *gt;
	unsigned int id;

	if (!pvc_needs_rc6_wa(i915))
		return;

	with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
		for_each_gt(gt, i915, id)
			fn(gt->uncore, FORCEWAKE_GT);
	}
}

static inline void pvc_wa_disallow_rc6(struct drm_i915_private *i915)
{
	_pvc_wa_disallow_rc6(i915, intel_uncore_forcewake_get);
}

static inline void pvc_wa_uncore_forcewake_put(struct intel_uncore *uncore,
					       enum forcewake_domains fw_domains)
{
	u64 delay_ns = uncore->i915->params.pvc_fw_put_delay_ms * NSEC_PER_MSEC;

	intel_uncore_forcewake_put_delayed(uncore, fw_domains, delay_ns);
}

static inline void pvc_wa_allow_rc6(struct drm_i915_private *i915)
{
	_pvc_wa_disallow_rc6(i915, pvc_wa_uncore_forcewake_put);
}

void intel_gt_info_print(const struct intel_gt_info *info,
			 struct drm_printer *p);

void intel_boost_fake_int_timer(struct intel_gt *gt, bool on_off);

void intel_gt_silent_driver_error(struct intel_gt *gt,
				  const enum intel_gt_driver_errors error);

__printf(3, 4)
void intel_gt_log_driver_error(struct intel_gt *gt,
			       const enum intel_gt_driver_errors error,
			       const char *fmt, ...);

#endif /* __INTEL_GT_H__ */
