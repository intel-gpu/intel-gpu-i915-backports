// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <linux/pm_runtime.h>
#include <linux/string_helpers.h>

#include "i915_drv.h"
#include "i915_reg.h"
#include "intel_engine_regs.h"
#include "intel_gpu_commands.h"
#include "intel_gt.h"
#include "intel_gt_mcr.h"
#include "intel_gt_pm.h"
#include "intel_gt_regs.h"
#include "intel_pcode.h"
#include "intel_rc6.h"
#include "gem/i915_gem_region.h"

/**
 * DOC: RC6
 *
 * RC6 is a special power stage which allows the GPU to enter an very
 * low-voltage mode when idle, using down to 0V while at this stage.  This
 * stage is entered automatically when the GPU is idle when RC6 support is
 * enabled, and as soon as new workload arises GPU wakes up automatically as
 * well.
 *
 * There are different RC6 modes available in Intel GPU, which differentiate
 * among each other with the latency required to enter and leave RC6 and
 * voltage consumed by the GPU in different states.
 *
 * The combination of the following flags define which states GPU is allowed
 * to enter, while RC6 is the normal RC6 state, RC6p is the deep RC6, and
 * RC6pp is deepest RC6. Their support by hardware varies according to the
 * GPU, BIOS, chipset and platform. RC6 is usually the safest one and the one
 * which brings the most power savings; deeper states save more power, but
 * require higher latency to switch to and wake up.
 */

static struct intel_gt *rc6_to_gt(struct intel_rc6 *rc6)
{
	return container_of(rc6, struct intel_gt, rc6);
}

static struct intel_uncore *rc6_to_uncore(struct intel_rc6 *rc)
{
	return rc6_to_gt(rc)->uncore;
}

static struct drm_i915_private *rc6_to_i915(struct intel_rc6 *rc)
{
	return rc6_to_gt(rc)->i915;
}

static void set(struct intel_uncore *uncore, i915_reg_t reg, u32 val)
{
	intel_uncore_write_fw(uncore, reg, val);
}

static bool enable_softpg(struct intel_gt *gt)
{
	int p = gt->i915->params.enable_softpg;

	if (p > 0)
		return true;
	else if (p < 0)
		return IS_PONTEVECCHIO(gt->i915);
	else
		return false;
}

static void gen11_rc6_enable(struct intel_rc6 *rc6)
{
	struct intel_gt *gt = rc6_to_gt(rc6);
	struct intel_uncore *uncore = gt->uncore;
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	u32 pg_enable;
	int i;

	/*
	 * With GuCRC, these parameters are set by GuC
	 */
	if (!intel_uc_uses_guc_rc(&gt->uc)) {
		/* 2b: Program RC6 thresholds.*/
		set(uncore, GEN6_RC6_WAKE_RATE_LIMIT, 54 << 16 | 85);
		set(uncore, GEN10_MEDIA_WAKE_RATE_LIMIT, 150);

		set(uncore, GEN6_RC_EVALUATION_INTERVAL, 125000); /* 12500 * 1280ns */
		set(uncore, GEN6_RC_IDLE_HYSTERSIS, 25); /* 25 * 1280ns */
		for_each_engine(engine, rc6_to_gt(rc6), id)
			set(uncore, RING_MAX_IDLE(engine->mmio_base), 10);

		set(uncore, GUC_MAX_IDLE_COUNT, 0xA);

		set(uncore, GEN6_RC_SLEEP, 0);

		set(uncore, GEN6_RC6_THRESHOLD, 50000); /* 50/125ms per EI */
	}

	/*
	 * 2c: Program Coarse Power Gating Policies.
	 *
	 * Bspec's guidance is to use 25us (really 25 * 1280ns) here. What we
	 * use instead is a more conservative estimate for the maximum time
	 * it takes us to service a CS interrupt and submit a new ELSP - that
	 * is the time which the GPU is idle waiting for the CPU to select the
	 * next request to execute. If the idle hysteresis is less than that
	 * interrupt service latency, the hardware will automatically gate
	 * the power well and we will then incur the wake up cost on top of
	 * the service latency. A similar guide from plane_state is that we
	 * do not want the enable hysteresis to less than the wakeup latency.
	 *
	 * igt/gem_exec_nop/sequential provides a rough estimate for the
	 * service latency, and puts it under 10us for Icelake, similar to
	 * Broadwell+, To be conservative, we want to factor in a context
	 * switch on top (due to ksoftirqd).
	 */
	set(uncore, GEN9_MEDIA_PG_IDLE_HYSTERESIS, 60);
	set(uncore, GEN9_RENDER_PG_IDLE_HYSTERESIS, 60);

	/* 3a: Enable RC6
	 *
	 * With GuCRC, we do not enable bit 31 of RC_CTL,
	 * thus allowing GuC to control RC6 entry/exit fully instead.
	 * We will not set the HW ENABLE and EI bits
	 */
	if (!intel_guc_rc_enable(&gt->uc.guc))
		rc6->ctl_enable = GEN6_RC_CTL_RC6_ENABLE;
	else
		rc6->ctl_enable =
			GEN6_RC_CTL_HW_ENABLE |
			GEN6_RC_CTL_RC6_ENABLE |
			GEN6_RC_CTL_EI_MODE(1);

	/* Wa_22012237902 - disable coarse PG for PVC BD A0 */
	if (IS_PVC_BD_STEP(rc6_to_i915(rc6), STEP_A0, STEP_B0))
		return;

	/* Wa_16011777198 - Render powergating must remain disabled */
	if (IS_DG2_GRAPHICS_STEP(gt->i915, G10, STEP_A0, STEP_C0) ||
	    IS_DG2_GRAPHICS_STEP(gt->i915, G11, STEP_A0, STEP_B0))
		pg_enable =
			GEN9_MEDIA_PG_ENABLE |
			GEN11_MEDIA_SAMPLER_PG_ENABLE;
	else
		pg_enable =
			GEN9_RENDER_PG_ENABLE |
			GEN9_MEDIA_PG_ENABLE |
			GEN11_MEDIA_SAMPLER_PG_ENABLE;

	if (GRAPHICS_VER(gt->i915) >= 12 && !IS_DG1(gt->i915)) {
		for (i = 0; i < I915_MAX_VCS; i++)
			if (HAS_ENGINE(gt, _VCS(i)))
				pg_enable |= (VDN_HCP_POWERGATE_ENABLE(i) |
					      VDN_MFX_POWERGATE_ENABLE(i));
	}

	/* Manually switch powergating off/on around GPU client activity */
	if (IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_SOFT_PG) && enable_softpg(gt)) {
		rc6->pg_enable = pg_enable;
		return;
	}

	set(uncore, GEN9_PG_ENABLE, pg_enable);
}

static bool rc6_exists(struct intel_rc6 *rc6)
{
	if (is_mock_gt(rc6_to_gt(rc6)))
		return false;

	return true;
}

static bool rc6_supported(struct intel_rc6 *rc6)
{
	struct drm_i915_private *i915 = rc6_to_i915(rc6);

	/*
	 * Wa_1509372804: pvc_ct[a*]
	 */
	if (!i915->params.rc6_ignore_steppings &&
	    IS_PVC_CT_STEP(i915, STEP_A0, STEP_B0))
		return false;

	/*
	 * Wa_1508652630
	 */
	if (!i915->params.rc6_ignore_steppings &&
	    IS_PVC_BD_STEP(i915, STEP_A0, STEP_B0) &&
	    i915->remote_tiles > 0)
		return false;

	/* Disable RC6 for all steppings except B4 */
	if (!i915->params.rc6_ignore_steppings &&
	    IS_PVC_CT_STEP(i915, STEP_B0, STEP_C0))
		return false;

#ifdef BPM_RC6_DISABLED
	/*
	 * Wa for HSD: 14015706335
	 */
	if (!i915->params.rc6_ignore_steppings &&
			IS_PVC_BD_STEP(i915, STEP_B0, STEP_FOREVER))
		return false;
#endif

	return i915->params.enable_rc6;
}

void intel_rc6_rpm_get(struct intel_rc6 *rc6)
{
	GEM_BUG_ON(rc6->wakeref);
	pm_runtime_get_sync(rc6_to_i915(rc6)->drm.dev);
	rc6->wakeref = true;
}

void intel_rc6_rpm_put(struct intel_rc6 *rc6)
{
	GEM_BUG_ON(!rc6->wakeref);
	pm_runtime_put(rc6_to_i915(rc6)->drm.dev);
	rc6->wakeref = false;
}

static void __intel_rc6_disable(struct intel_rc6 *rc6)
{
	struct drm_i915_private *i915 = rc6_to_i915(rc6);
	struct intel_uncore *uncore = rc6_to_uncore(rc6);
	struct intel_gt *gt = rc6_to_gt(rc6);

	if (i915->quiesce_gpu)
		return;

	/* Take control of RC6 back from GuC */
	intel_guc_rc_disable(&gt->uc.guc);

	intel_uncore_forcewake_get(uncore, FORCEWAKE_ALL);
	if (GRAPHICS_VER(i915) >= 9)
		set(uncore, GEN9_PG_ENABLE, 0);
	set(uncore, GEN6_RC_CONTROL, 0);
	set(uncore, GEN6_RC_STATE, 0);
	intel_uncore_forcewake_put(uncore, FORCEWAKE_ALL);
}

void intel_rc6_init(struct intel_rc6 *rc6)
{
	int err;

	/* Disable runtime-pm until we can save the GPU state with rc6 pctx */
	intel_rc6_rpm_get(rc6);

	if (!rc6_exists(rc6))
		return;

	/* Sanitize rc6, ensure it is disabled before we are ready. */
	__intel_rc6_disable(rc6);

	err = 0;
	if (!rc6_supported(rc6))
		err = -ECANCELED;

	rc6->supported = err == 0;
}

void intel_rc6_sanitize(struct intel_rc6 *rc6)
{
	if (rc6->enabled) { /* unbalanced suspend/resume */
		intel_rc6_rpm_get(rc6);
		rc6->enabled = false;
	}

	if (rc6->supported)
		__intel_rc6_disable(rc6);
}

void intel_rc6_enable(struct intel_rc6 *rc6)
{
	struct drm_i915_private *i915 = rc6_to_i915(rc6);
	struct intel_uncore *uncore = rc6_to_uncore(rc6);

	if (!rc6->supported)
		return;

	GEM_BUG_ON(rc6->enabled);

	intel_uncore_forcewake_get(uncore, FORCEWAKE_ALL);

	gen11_rc6_enable(rc6);

	rc6->manual = rc6->ctl_enable & GEN6_RC_CTL_RC6_ENABLE;
	if (pvc_needs_rc6_wa(i915))
		rc6->ctl_enable = 0;

	intel_uncore_forcewake_put(uncore, FORCEWAKE_ALL);

	/* rc6 is ready, runtime-pm is go! */
	intel_rc6_rpm_put(rc6);
	rc6->enabled = true;
}

void intel_rc6_unpark(struct intel_rc6 *rc6)
{
	struct intel_uncore *uncore = rc6_to_uncore(rc6);

	if (!rc6->enabled)
		return;

	/* Restore HW timers for automatic RC6 entry while busy */
	set(uncore, GEN6_RC_CONTROL, rc6->ctl_enable);

	if (rc6->pg_enable)
		set(uncore, GEN9_PG_ENABLE, 0);
}

void intel_rc6_park(struct intel_rc6 *rc6)
{
	struct intel_uncore *uncore = rc6_to_uncore(rc6);
	unsigned int target;

	if (!rc6->enabled)
		return;

	if (rc6->pg_enable)
		set(uncore, GEN9_PG_ENABLE, rc6->pg_enable);

	if (!rc6->manual)
		return;

	/* Turn off the HW timers and go directly to rc6 */
	set(uncore, GEN6_RC_CONTROL, GEN6_RC_CTL_RC6_ENABLE);

	target = 0x4; /* normal rc6 */
	set(uncore, GEN6_RC_STATE, target << RC_SW_TARGET_STATE_SHIFT);
}

void intel_rc6_disable(struct intel_rc6 *rc6)
{
	if (!rc6->enabled)
		return;

	intel_rc6_rpm_get(rc6);
	rc6->enabled = false;

	__intel_rc6_disable(rc6);

	/* Reset our culmulative residency tracking over suspend */
	memset(rc6->prev_hw_residency, 0, sizeof(rc6->prev_hw_residency));
}

void intel_rc6_fini(struct intel_rc6 *rc6)
{
	struct drm_i915_gem_object *pctx;
	struct intel_uncore *uncore = rc6_to_uncore(rc6);

	intel_rc6_disable(rc6);

	if (rc6->dfd_restore_obj) {
		intel_uncore_write(uncore, DFD_RESTORE_CFG_LSB, 0);
		intel_uncore_write(uncore, DFD_RESTORE_CFG_MSB, 0);

		i915_gem_object_unpin_map(rc6->dfd_restore_obj);
		i915_gem_object_put(rc6->dfd_restore_obj);
		rc6->dfd_restore_buf = NULL;
		rc6->dfd_restore_obj = NULL;
	}

	pctx = fetch_and_zero(&rc6->pctx);
	if (pctx)
		i915_gem_object_put(pctx);

	if (rc6->wakeref)
		intel_rc6_rpm_put(rc6);
}

u64 intel_rc6_residency_ns(struct intel_rc6 *rc6, const i915_reg_t reg)
{
	struct drm_i915_private *i915 = rc6_to_i915(rc6);
	struct intel_uncore *uncore = rc6_to_uncore(rc6);
	u64 time_hw, prev_hw, overflow_hw;
	unsigned int fw_domains;
	unsigned long flags;
	unsigned int i;
	u32 mul, div;
	i915_reg_t base;

	if (!rc6->supported)
		return 0;

	/*
	 * Store previous hw counter values for counter wrap-around handling.
	 *
	 * There are only four interesting registers and they live next to each
	 * other so we can use the relative address, compared to the smallest
	 * one as the index into driver storage.
	 */
	base = (rc6_to_gt(rc6)->type == GT_MEDIA) ?
	       MTL_MEDIA_MC6 : GEN6_GT_GFX_RC6_LOCKED;
	i = (i915_mmio_reg_offset(reg) -
	     i915_mmio_reg_offset(base)) / sizeof(u32);
	if (drm_WARN_ON_ONCE(&i915->drm, i >= ARRAY_SIZE(rc6->cur_residency)))
		return 0;

	fw_domains = intel_uncore_forcewake_for_reg(uncore, reg, FW_REG_READ);

	spin_lock_irqsave(&uncore->lock, flags);
	intel_uncore_forcewake_get__locked(uncore, fw_domains);

	/* 833.33ns units on Gen9LP, 1.28us elsewhere. */
	mul = 1280;
	div = 1;

	overflow_hw = BIT_ULL(32);
	time_hw = intel_uncore_read_fw(uncore, reg);

	/*
	 * Counter wrap handling.
	 *
	 * But relying on a sufficient frequency of queries otherwise counters
	 * can still wrap.
	 */
	prev_hw = rc6->prev_hw_residency[i];
	rc6->prev_hw_residency[i] = time_hw;

	/* RC6 delta from last sample. */
	if (time_hw >= prev_hw)
		time_hw -= prev_hw;
	else
		time_hw += overflow_hw - prev_hw;

	/* Add delta to RC6 extended raw driver copy. */
	time_hw += rc6->cur_residency[i];
	rc6->cur_residency[i] = time_hw;

	intel_uncore_forcewake_put__locked(uncore, fw_domains);
	spin_unlock_irqrestore(&uncore->lock, flags);

	return mul_u64_u32_div(time_hw, mul, div);
}

u64 intel_rc6_residency_us(struct intel_rc6 *rc6, i915_reg_t reg)
{
	return DIV_ROUND_UP_ULL(intel_rc6_residency_ns(rc6, reg), 1000);
}

u64 intel_rc6_rpm_unit_residency(struct intel_rc6 *rc6)
{
	struct intel_gt *gt = rc6_to_gt(rc6);
	intel_wakeref_t wakeref;
	u64 lsb, msb, counter;

	with_intel_runtime_pm(gt->uncore->rpm, wakeref) {
		lsb = intel_uncore_read(gt->uncore, GEN12_GT_GFX_RC6_LSB);
		msb = intel_uncore_read(gt->uncore, GEN12_GT_GFX_RC6_MSB);
	}

	msb = REG_FIELD_GET(GEN12_GT_GFX_RC6_MSB_MASK, (u32)msb);
	counter = msb << 32 | lsb;

	return counter;
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftest_rc6.c"
#endif
