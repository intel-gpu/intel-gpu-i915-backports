// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <linux/string_helpers.h>
#include <linux/suspend.h>

#include "gem/i915_gem_shmem.h"

#include "i915_drv.h"
#include "i915_params.h"
#include "intel_context.h"
#include "intel_engine_pm.h"
#include "intel_gt.h"
#include "intel_gt_ccs_mode.h"
#include "intel_gt_clock_utils.h"
#include "intel_gt_pm.h"
#include "intel_gt_print.h"
#include "intel_gt_requests.h"
#include "intel_llc.h"
#include "intel_pm.h"
#include "intel_tlb.h"
#include "intel_rc6.h"
#include "intel_rps.h"
#include "intel_wakeref.h"
#include "intel_pcode.h"

#include "pxp/intel_pxp_pm.h"

static void dbg_poison_ce(struct intel_context *ce)
{
	if (!IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GEM))
		return;

	if (ce->state) {
		struct drm_i915_gem_object *obj = ce->state->obj;

		memset(page_mask_bits(obj->mm.mapping),
		       CONTEXT_REDZONE, obj->base.size);
		i915_gem_object_flush_map(obj);
	}
}

static void reset_pinned_contexts(struct intel_gt *gt)
{
	struct intel_context *ce;

	list_for_each_entry(ce, &gt->pinned_contexts, pinned_contexts_link) {
		dbg_poison_ce(ce);
		ce->ops->reset(ce);
	}
}

/*
 * Wa_14017210380: mtl
 */

static bool mtl_needs_media_mc6_wa(struct intel_gt *gt)
{
	return (IS_MTL_GRAPHICS_STEP(gt->i915, P, STEP_A0, STEP_B0) &&
		gt->type == GT_MEDIA);
}

static void mtl_mc6_wa_media_busy(struct intel_gt *gt)
{
	if (mtl_needs_media_mc6_wa(gt))
		snb_pcode_write_p(gt->uncore, PCODE_MBOX_GT_STATE,
				  PCODE_MBOX_GT_STATE_MEDIA_BUSY,
				  PCODE_MBOX_GT_STATE_DOMAIN_MEDIA, 0);
}

static void mtl_mc6_wa_media_not_busy(struct intel_gt *gt)
{
	if (mtl_needs_media_mc6_wa(gt))
		snb_pcode_write_p(gt->uncore, PCODE_MBOX_GT_STATE,
				  PCODE_MBOX_GT_STATE_MEDIA_NOT_BUSY,
				  PCODE_MBOX_GT_STATE_DOMAIN_MEDIA, 0);
}

static void user_forcewake(struct intel_gt *gt, bool suspend)
{
	int count = atomic_read(&gt->user_wakeref);
	intel_wakeref_t wakeref;

	/* Inside suspend/resume so single threaded, no races to worry about. */
	if (likely(!count))
		return;

	wakeref = intel_gt_pm_get(gt);
	if (suspend) {
		GEM_BUG_ON(count > atomic_read(&gt->wakeref.count));
		atomic_sub(count, &gt->wakeref.count);
	} else {
		atomic_add(count, &gt->wakeref.count);
	}
	intel_gt_pm_put(gt, wakeref);
}

static void runtime_begin(struct intel_gt *gt)
{
	smp_wmb(); /* pairs with intel_gt_get_busy_time() */
	WRITE_ONCE(gt->stats.start, ktime_get());
}

static void runtime_end(struct intel_gt *gt)
{
	ktime_t total;

	total = ktime_sub(ktime_get(), gt->stats.start);
	total = ktime_add(gt->stats.total, total);

	WRITE_ONCE(gt->stats.start, 0);
	smp_wmb(); /* pairs with intel_gt_get_busy_time() */
	gt->stats.total = total;
}

static int __gt_unpark(struct intel_wakeref *wf)
{
	struct intel_gt *gt = container_of(wf, typeof(*gt), wakeref);

	GT_TRACE(gt, "unparking\n");

	/* Wa_14017210380: mtl */
	mtl_mc6_wa_media_busy(gt);

	intel_rc6_unpark(&gt->rc6);
	intel_rps_unpark(&gt->rps);
	i915_pmu_gt_unparked(gt);
	intel_guc_busyness_unpark(gt);

	intel_gt_unpark_requests(gt);
	runtime_begin(gt);

	GT_TRACE(gt, "unparked\n");
	return 0;
}

static int __gt_park(struct intel_wakeref *wf)
{
	struct intel_gt *gt = container_of(wf, typeof(*gt), wakeref);
	struct drm_i915_private *i915 = gt->i915;

	GT_TRACE(gt, "clearing memory\n");
	atomic_set(&gt->user_engines, 0); /* clear any meta bits */

	if (gt->lmem && i915_gem_lmem_park(gt->lmem))
		return -EBUSY;

	if (i915->mm.regions[0]->gt == gt && i915_gem_shmem_park(i915->mm.regions[0]))
		return -EBUSY;

	GT_TRACE(gt, "parking\n");
	runtime_end(gt);
	intel_gt_park_requests(gt);

	/* TLB are always invalidated on restarting any execution */
	intel_tlb_invalidation_revoke(gt);

	intel_guc_busyness_park(gt);
	i915_pmu_gt_parked(gt);
	intel_rps_park(&gt->rps);
	intel_rc6_park(&gt->rc6);

	intel_gt_park_ccs_mode(gt, NULL);

	clear_bit(INTEL_MEMORY_CLEAR_FREE, &i915->mm.regions[0]->flags);

	/* Everything switched off, flush any residual interrupt just in case */
	intel_synchronize_irq(i915);

	/* Wa_14017210380: mtl */
	mtl_mc6_wa_media_not_busy(gt);

	GT_TRACE(gt, "parked\n");
	return 0;
}

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
static intel_wakeref_t display_pm_get(void *rpm)
{
	/*
	 * It seems that the DMC likes to transition between the DC states a lot
	 * when there are no connected displays (no active power domains) during
	 * command submission.
	 *
	 * This activity has negative impact on the performance of the chip with
	 * huge latencies observed in the interrupt handler and elsewhere.
	 *
	 * Work around it by grabbing a GT IRQ power domain whilst there is any
	 * GT activity, preventing any DC state transitions.
	 */
	return intel_display_power_get(rpm, POWER_DOMAIN_GT_IRQ);
}

static void display_pm_put(void *rpm, intel_wakeref_t wf)
{
	/* Defer dropping the display power well for 100ms, it's slow! */
	intel_display_power_put_async(rpm, POWER_DOMAIN_GT_IRQ, wf);
}
#endif

static const struct intel_wakeref_ops root_ops = {
#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
	.pm_get = display_pm_get,
	.pm_put = display_pm_put,
#endif

	.get = __gt_unpark,
	.put = __gt_park,
};

static const struct intel_wakeref_ops wf_ops = {
	.pm_get = (typeof(wf_ops.pm_get))intel_gt_pm_get,
	.pm_put = (typeof(wf_ops.pm_put))intel_gt_pm_put,

	.get = __gt_unpark,
	.put = __gt_park,
};

void intel_gt_pm_init_early(struct intel_gt *gt)
{
	/*
	 * We access the runtime_pm structure via gt->i915 here rather than
	 * gt->uncore as we do elsewhere in the file because gt->uncore is not
	 * yet initialized for all tiles at this point in the driver startup.
	 * runtime_pm is per-device rather than per-tile, so this is still the
	 * correct structure.
	 */
	if (gt == to_root_gt(gt->i915))
		intel_wakeref_init(&gt->wakeref,
#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
				   gt->i915,
#else
				   &gt->i915->runtime_pm,
#endif
				   &root_ops, "GT");
	else
		intel_wakeref_init(&gt->wakeref, to_root_gt(gt->i915), &wf_ops, "GT+");
}

void intel_gt_pm_init(struct intel_gt *gt)
{
	/*
	 * Enabling power-management should be "self-healing". If we cannot
	 * enable a feature, simply leave it disabled with a notice to the
	 * user.
	 */
	intel_rc6_init(&gt->rc6);
	intel_rps_init(&gt->rps);
}

static bool reset_engines(struct intel_gt *gt)
{
	return __intel_gt_reset(gt, ALL_ENGINES) == 0;
}

static void gt_sanitize(struct intel_gt *gt, bool force)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	intel_wakeref_t wakeref;

	if (is_mock_gt(gt))
		return;

	if (gt->i915->quiesce_gpu)
		return;

	GT_TRACE(gt, "force:%s", str_yes_no(force));

	/* Use a raw wakeref to avoid calling intel_display_power_get early */
	wakeref = intel_runtime_pm_get(gt->uncore->rpm);
	intel_uncore_forcewake_get(gt->uncore, FORCEWAKE_ALL);

	intel_gt_check_clock_frequency(gt);

	/*
	 * As we have just resumed the machine and woken the device up from
	 * deep PCI sleep (presumably D3_cold), assume the HW has been reset
	 * back to defaults, recovering from whatever wedged state we left it
	 * in and so worth trying to use the device once more.
	 */
	if (intel_gt_is_wedged(gt))
		intel_gt_unset_wedged(gt);

	/* For GuC mode, ensure submission is disabled before stopping ring */
	intel_uc_reset_prepare(&gt->uc);

	for_each_engine(engine, gt, id) {
		if (engine->reset.prepare)
			engine->reset.prepare(engine);

		if (engine->status_page.sanitize)
			engine->status_page.sanitize(engine);
	}

	if (reset_engines(gt) || force) {
		for_each_engine(engine, gt, id)
			__intel_engine_reset(engine, false);
	}

	intel_uc_reset(&gt->uc, ALL_ENGINES);
	intel_gt_retire_requests(gt);
	reset_pinned_contexts(gt);

	for_each_engine(engine, gt, id)
		if (engine->reset.finish)
			engine->reset.finish(engine);

	intel_rps_sanitize(&gt->rps);

	intel_uncore_forcewake_put(gt->uncore, FORCEWAKE_ALL);
	intel_runtime_pm_put(gt->uncore->rpm, wakeref);
}

void intel_gt_pm_fini(struct intel_gt *gt)
{
	intel_rc6_fini(&gt->rc6);
}

void intel_gt_resume_early(struct intel_gt *gt)
{
	if (gt->type != GT_MEDIA)
		i915_ggtt_resume(gt->ggtt);

	if (GRAPHICS_VER(gt->i915) >= 8)
		setup_private_pat(gt);

	intel_uc_resume_early(&gt->uc);
}

int intel_gt_resume(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	intel_wakeref_t wakeref;
	int err;

	err = intel_gt_has_unrecoverable_error(gt);
	if (err)
		return err;

	GT_TRACE(gt, "\n");

	/*
	 * After resume, we may need to poke into the pinned kernel
	 * contexts to paper over any damage caused by the sudden suspend.
	 * Only the kernel contexts should remain pinned over suspend,
	 * allowing us to fixup the user contexts on their first pin.
	 */
	gt_sanitize(gt, true);

	wakeref = intel_gt_pm_get(gt);

	intel_uncore_forcewake_get(gt->uncore, FORCEWAKE_ALL);
	intel_rc6_sanitize(&gt->rc6);
	if (intel_gt_is_wedged(gt)) {
		err = -EIO;
		goto out_fw;
	}

	/* Only when the HW is re-initialised, can we replay the requests */
	err = intel_gt_init_hw(gt);
	if (err) {
		gt_probe_error(gt, "Failed to initialize GPU, declaring it wedged!\n");
		goto err_wedged;
	}

	intel_uc_reset_finish(&gt->uc);

	intel_rps_enable(&gt->rps);
	intel_llc_enable(&gt->llc);

	for_each_engine(engine, gt, id) {
		intel_engine_pm_get(engine);

		engine->serial++; /* kernel context lost */
		err = intel_engine_resume(engine);

		intel_engine_pm_put(engine);
		if (err) {
			intel_gt_log_driver_error(gt, INTEL_GT_DRIVER_ERROR_ENGINE_OTHER,
						  "Failed to restart '%s' (%d)\n",
						  engine->name, err);
			goto err_wedged;
		}
	}

	intel_rc6_enable(&gt->rc6);

	intel_uc_resume(&gt->uc);

	intel_pxp_resume(&gt->pxp);

	user_forcewake(gt, false);
	WRITE_ONCE(gt->suspend, false);

out_fw:
	intel_uncore_forcewake_put(gt->uncore, FORCEWAKE_ALL);
	intel_gt_pm_put(gt, wakeref);
	return err;

err_wedged:
	intel_gt_set_wedged(gt);
	goto out_fw;
}

static void flush_clear_on_idle(struct intel_gt *gt)
{
	struct intel_memory_region *mem;

	/* Wait for the suspend flag to be visible in i915_gem_lmem_park() */
	mutex_lock(&gt->wakeref.mutex);
	mutex_unlock(&gt->wakeref.mutex);

	/* Wait for any workers started before the flag became visible */
	mem = gt->lmem;
	if (mem)
		wait_for_completion(&mem->parking);
}

static void wait_for_suspend(struct intel_gt *gt)
{
	intel_wakeref_t wf;

	/* Flush all pending page workers */
	if (gt->wq)
		flush_workqueue(gt->wq);
	rcu_barrier();

	if (gt->i915->quiesce_gpu)
		return;

	with_intel_gt_pm_if_awake(gt, wf) {
		/* Cancel outstanding work and leave the gpu quiet */
		if (intel_gt_wait_for_idle(gt, I915_GEM_IDLE_TIMEOUT) == -ETIME)
			intel_gt_set_wedged(gt);

		/* Make the GPU available again for swapout */
		intel_gt_unset_wedged(gt);
	}
}

void intel_gt_suspend_prepare(struct intel_gt *gt)
{
	user_forcewake(gt, true);

	WRITE_ONCE(gt->suspend, true);
	flush_clear_on_idle(gt);
	wait_for_suspend(gt);

	intel_gt_retire_requests(gt);
	intel_tlb_invalidation_revoke(gt);

	intel_pxp_suspend(&gt->pxp, false);
}

static suspend_state_t pm_suspend_target(void)
{
#if IS_ENABLED(CONFIG_SUSPEND) && IS_ENABLED(CONFIG_PM_SLEEP)
	return pm_suspend_target_state;
#else
	return PM_SUSPEND_TO_IDLE;
#endif
}

void intel_gt_suspend_late(struct intel_gt *gt)
{
	intel_wakeref_t wakeref;

	/* We expect to be idle already; but also want to be independent */
	wait_for_suspend(gt);
	if (intel_gt_pm_wait_for_idle(gt, I915_GEM_IDLE_TIMEOUT))
		intel_gt_set_wedged(gt);

	if (is_mock_gt(gt))
		return;

	if (gt->i915->quiesce_gpu)
		return;

	intel_uc_suspend(&gt->uc);

	/*
	 * On disabling the device, we want to turn off HW access to memory
	 * that we no longer own.
	 *
	 * However, not all suspend-states disable the device. S0 (s2idle)
	 * is effectively runtime-suspend, the device is left powered on
	 * but needs to be put into a low power state. We need to keep
	 * powermanagement enabled, but we also retain system state and so
	 * it remains safe to keep on using our allocated memory.
	 */
	if (pm_suspend_target() == PM_SUSPEND_TO_IDLE)
		return;

	with_intel_runtime_pm(gt->uncore->rpm, wakeref) {
		intel_rps_disable(&gt->rps);
		intel_rc6_disable(&gt->rc6);
		intel_llc_disable(&gt->llc);

		if (gt->type != GT_MEDIA)
			i915_ggtt_suspend(gt->ggtt);
	}

	gt_sanitize(gt, false); /* Be paranoid, remove all residual GPU state */

	GT_TRACE(gt, "\n");
}

void intel_gt_runtime_suspend(struct intel_gt *gt)
{
	intel_pxp_suspend(&gt->pxp, true);
	intel_uc_runtime_suspend(&gt->uc);

	GT_TRACE(gt, "\n");
}

int intel_gt_runtime_resume(struct intel_gt *gt)
{
	int ret;

	GT_TRACE(gt, "\n");

	ret = intel_uc_runtime_resume(&gt->uc);
	if (ret)
		return ret;

	intel_pxp_resume(&gt->pxp);

	return 0;
}

ktime_t intel_gt_get_awake_time(const struct intel_gt *gt)
{
	ktime_t total = gt->stats.total;
	ktime_t start;

	start = READ_ONCE(gt->stats.start);
	if (start) {
		smp_rmb(); /* pairs with runtime_begin/end */
		start = ktime_sub(ktime_get(), start);
	}

	return ktime_add(total, start);
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftest_gt_pm.c"
#endif
