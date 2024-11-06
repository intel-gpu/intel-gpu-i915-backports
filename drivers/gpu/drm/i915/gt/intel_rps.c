// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <linux/string_helpers.h>

#include <drm/i915_drm.h>

#include "i915_drv.h"
#include "i915_irq.h"
#include "intel_breadcrumbs.h"
#include "intel_gt.h"
#include "intel_gt_clock_utils.h"
#include "intel_gt_irq.h"
#include "intel_gt_pm_irq.h"
#include "intel_gt_print.h"
#include "intel_gt_regs.h"
#include "intel_mchbar_regs.h"
#include "intel_pcode.h"
#include "intel_rps.h"

#define BUSY_MAX_EI	20u /* ms */

static struct intel_gt *rps_to_gt(struct intel_rps *rps)
{
	return container_of(rps, struct intel_gt, rps);
}

static struct drm_i915_private *rps_to_i915(struct intel_rps *rps)
{
	return rps_to_gt(rps)->i915;
}

static struct intel_uncore *rps_to_uncore(struct intel_rps *rps)
{
	return rps_to_gt(rps)->uncore;
}

static struct intel_guc_slpc *rps_to_slpc(struct intel_rps *rps)
{
	struct intel_gt *gt = rps_to_gt(rps);

	return &gt->uc.guc.slpc;
}

static bool rps_uses_slpc(struct intel_rps *rps)
{
	struct intel_gt *gt = rps_to_gt(rps);

	return intel_uc_uses_guc_slpc(&gt->uc);
}

static bool rps_supported(struct intel_rps *rps)
{
	struct drm_i915_private *i915 = rps_to_i915(rps);

	return i915->params.enable_rps;
}

static u32 rps_pm_sanitize_mask(struct intel_rps *rps, u32 mask)
{
	return mask & ~rps->pm_intrmsk_mbz;
}

static void set(struct intel_uncore *uncore, i915_reg_t reg, u32 val)
{
	intel_uncore_write_fw(uncore, reg, val);
}

static void rps_timer(struct timer_list *t)
{
	struct intel_rps *rps = from_timer(rps, t, timer);
	struct intel_engine_cs *engine;
	ktime_t dt, last, timestamp;
	enum intel_engine_id id;
	s64 max_busy[3] = {};

	timestamp = 0;
	for_each_engine(engine, rps_to_gt(rps), id) {
		s64 busy;
		int i;

		dt = intel_engine_get_busy_time(engine, &timestamp);
		last = engine->stats.rps;
		engine->stats.rps = dt;

		busy = ktime_to_ns(ktime_sub(dt, last));
		for (i = 0; i < ARRAY_SIZE(max_busy); i++) {
			if (busy > max_busy[i])
				swap(busy, max_busy[i]);
		}
	}
	last = rps->pm_timestamp;
	rps->pm_timestamp = timestamp;

	if (intel_rps_is_active(rps)) {
		s64 busy;
		int i;

		dt = ktime_sub(timestamp, last);

		/*
		 * Our goal is to evaluate each engine independently, so we run
		 * at the lowest clocks required to sustain the heaviest
		 * workload. However, a task may be split into sequential
		 * dependent operations across a set of engines, such that
		 * the independent contributions do not account for high load,
		 * but overall the task is GPU bound. For example, consider
		 * video decode on vcs followed by colour post-processing
		 * on vecs, followed by general post-processing on rcs.
		 * Since multi-engines being active does imply a single
		 * continuous workload across all engines, we hedge our
		 * bets by only contributing a factor of the distributed
		 * load into our busyness calculation.
		 */
		busy = max_busy[0];
		for (i = 1; i < ARRAY_SIZE(max_busy); i++) {
			if (!max_busy[i])
				break;

			busy += div_u64(max_busy[i], 1 << i);
		}

		if (100 * busy > rps->power.up_threshold * dt &&
		    rps->cur_freq < rps->max_freq_softlimit) {
			rps->pm_iir |= GEN6_PM_RP_UP_THRESHOLD;
			rps->pm_interval = 1;
			schedule_work(&rps->work);
		} else if (100 * busy < rps->power.down_threshold * dt &&
			   rps->cur_freq > rps->min_freq_softlimit) {
			rps->pm_iir |= GEN6_PM_RP_DOWN_THRESHOLD;
			rps->pm_interval = 1;
			schedule_work(&rps->work);
		} else {
			rps->last_adj = 0;
		}

		if (rps->pm_interval < BUSY_MAX_EI) {
			GT_TRACE(rps_to_gt(rps),
				 "busy:%lld [%d%%], max:[%lld, %lld, %lld], interval:%d\n",
				 busy, (int)div64_u64(100 * busy, dt),
				 max_busy[0], max_busy[1], max_busy[2],
				 rps->pm_interval);
		}

		mod_timer(&rps->timer,
			  jiffies + msecs_to_jiffies(rps->pm_interval));
		rps->pm_interval = min(rps->pm_interval * 2, BUSY_MAX_EI);
	}
}

static void rps_start_timer(struct intel_rps *rps)
{
	rps->pm_timestamp = ktime_sub(ktime_get(), rps->pm_timestamp);
	rps->pm_interval = 1;
	mod_timer(&rps->timer, jiffies + 1);
}

static void rps_stop_timer(struct intel_rps *rps)
{
	del_timer_sync(&rps->timer);
	rps->pm_timestamp = ktime_sub(ktime_get(), rps->pm_timestamp);
	cancel_work_sync(&rps->work);
}

static u32 rps_pm_mask(struct intel_rps *rps, u8 val)
{
	u32 mask = 0;

	/* We use UP_EI_EXPIRED interrupts for both up/down in manual mode */
	if (val > rps->min_freq_softlimit)
		mask |= (GEN6_PM_RP_UP_EI_EXPIRED |
			 GEN6_PM_RP_DOWN_THRESHOLD |
			 GEN6_PM_RP_DOWN_TIMEOUT);

	if (val < rps->max_freq_softlimit)
		mask |= GEN6_PM_RP_UP_EI_EXPIRED | GEN6_PM_RP_UP_THRESHOLD;

	mask &= rps->pm_events;

	return rps_pm_sanitize_mask(rps, ~mask);
}

static void rps_reset_ei(struct intel_rps *rps)
{
	memset(&rps->ei, 0, sizeof(rps->ei));
}

static void rps_enable_interrupts(struct intel_rps *rps)
{
	struct intel_gt *gt = rps_to_gt(rps);

	GEM_BUG_ON(rps_uses_slpc(rps));

	GT_TRACE(gt, "interrupts:on rps->pm_events: %x, rps_pm_mask:%x\n",
		 rps->pm_events, rps_pm_mask(rps, rps->last_freq));

	rps_reset_ei(rps);

	spin_lock_irq(gt->irq_lock);
	gen6_gt_pm_enable_irq(gt, rps->pm_events);
	spin_unlock_irq(gt->irq_lock);

	intel_uncore_write(gt->uncore,
			   GEN6_PMINTRMSK, rps_pm_mask(rps, rps->last_freq));
}

static void gen6_rps_reset_interrupts(struct intel_rps *rps)
{
	gen6_gt_pm_reset_iir(rps_to_gt(rps), GEN6_PM_RPS_EVENTS);
}

static void gen11_rps_reset_interrupts(struct intel_rps *rps)
{
	while (gen11_gt_reset_one_iir(rps_to_gt(rps), 0, GEN11_GTPM))
		;
}

static void rps_reset_interrupts(struct intel_rps *rps)
{
	struct intel_gt *gt = rps_to_gt(rps);

	spin_lock_irq(gt->irq_lock);
	if (GRAPHICS_VER(gt->i915) >= 11)
		gen11_rps_reset_interrupts(rps);
	else
		gen6_rps_reset_interrupts(rps);

	rps->pm_iir = 0;
	spin_unlock_irq(gt->irq_lock);
}

static void rps_disable_interrupts(struct intel_rps *rps)
{
	struct intel_gt *gt = rps_to_gt(rps);

	intel_uncore_write(gt->uncore,
			   GEN6_PMINTRMSK, rps_pm_sanitize_mask(rps, ~0u));

	spin_lock_irq(gt->irq_lock);
	gen6_gt_pm_disable_irq(gt, GEN6_PM_RPS_EVENTS);
	spin_unlock_irq(gt->irq_lock);

	intel_synchronize_irq(gt->i915);

	/*
	 * Now that we will not be generating any more work, flush any
	 * outstanding tasks. As we are called on the RPS idle path,
	 * we will reset the GPU to minimum frequencies, so the current
	 * state of the worker can be discarded.
	 */
	cancel_work_sync(&rps->work);

	rps_reset_interrupts(rps);
	GT_TRACE(gt, "interrupts:off\n");
}

static u32 rps_limits(struct intel_rps *rps, u8 val)
{
	u32 limits;

	/*
	 * Only set the down limit when we've reached the lowest level to avoid
	 * getting more interrupts, otherwise leave this clear. This prevents a
	 * race in the hw when coming out of rc6: There's a tiny window where
	 * the hw runs at the minimal clock before selecting the desired
	 * frequency, if the down threshold expires in that window we will not
	 * receive a down interrupt.
	 */
	if (GRAPHICS_VER(rps_to_i915(rps)) >= 9) {
		limits = rps->max_freq_softlimit << 23;
		if (val <= rps->min_freq_softlimit)
			limits |= rps->min_freq_softlimit << 14;
	} else {
		limits = rps->max_freq_softlimit << 24;
		if (val <= rps->min_freq_softlimit)
			limits |= rps->min_freq_softlimit << 16;
	}

	return limits;
}

static void rps_set_power(struct intel_rps *rps, int new_power)
{
	struct intel_gt *gt = rps_to_gt(rps);
	struct intel_uncore *uncore = gt->uncore;
	u32 threshold_up = 0, threshold_down = 0; /* in % */
	u32 ei_up = 0, ei_down = 0;

	lockdep_assert_held(&rps->power.mutex);

	if (new_power == rps->power.mode)
		return;

	threshold_up = 95;
	threshold_down = 85;

	/* Note the units here are not exactly 1us, but 1280ns. */
	switch (new_power) {
	case LOW_POWER:
		ei_up = 16000;
		ei_down = 32000;
		break;

	case BETWEEN:
		ei_up = 13000;
		ei_down = 32000;
		break;

	case HIGH_POWER:
		ei_up = 10000;
		ei_down = 32000;
		break;
	}

	GT_TRACE(gt,
		 "changing power mode [%d], up %d%% @ %dus, down %d%% @ %dus\n",
		 new_power, threshold_up, ei_up, threshold_down, ei_down);

	set(uncore, GEN6_RP_UP_EI,
	    intel_gt_ns_to_pm_interval(gt, ei_up * 1000));
	set(uncore, GEN6_RP_UP_THRESHOLD,
	    intel_gt_ns_to_pm_interval(gt, ei_up * threshold_up * 10));

	set(uncore, GEN6_RP_DOWN_EI,
	    intel_gt_ns_to_pm_interval(gt, ei_down * 1000));
	set(uncore, GEN6_RP_DOWN_THRESHOLD,
	    intel_gt_ns_to_pm_interval(gt, ei_down * threshold_down * 10));

	set(uncore, GEN6_RP_CONTROL,
	    (GRAPHICS_VER(gt->i915) > 9 ? 0 : GEN6_RP_MEDIA_TURBO) |
	    GEN6_RP_MEDIA_HW_NORMAL_MODE |
	    GEN6_RP_MEDIA_IS_GFX |
	    GEN6_RP_ENABLE |
	    GEN6_RP_UP_BUSY_AVG |
	    GEN6_RP_DOWN_IDLE_AVG);

	rps->power.mode = new_power;
	rps->power.up_threshold = threshold_up;
	rps->power.down_threshold = threshold_down;
}

static void gen6_rps_set_thresholds(struct intel_rps *rps, u8 val)
{
	int new_power;

	new_power = rps->power.mode;
	switch (rps->power.mode) {
	case LOW_POWER:
		if (val > rps->efficient_freq + 1 &&
		    val > rps->cur_freq)
			new_power = BETWEEN;
		break;

	case BETWEEN:
		if (val <= rps->efficient_freq &&
		    val < rps->cur_freq)
			new_power = LOW_POWER;
		else if (val >= rps->rp0_freq &&
			 val > rps->cur_freq)
			new_power = HIGH_POWER;
		break;

	case HIGH_POWER:
		if (val < (rps->rp1_freq + rps->rp0_freq) >> 1 &&
		    val < rps->cur_freq)
			new_power = BETWEEN;
		break;
	}
	/* Max/min bins are special */
	if (val <= rps->min_freq_softlimit)
		new_power = LOW_POWER;
	if (val >= rps->max_freq_softlimit)
		new_power = HIGH_POWER;

	mutex_lock(&rps->power.mutex);
	if (rps->power.interactive)
		new_power = HIGH_POWER;
	rps_set_power(rps, new_power);
	mutex_unlock(&rps->power.mutex);
}

void intel_rps_mark_interactive(struct intel_rps *rps, bool interactive)
{
	GT_TRACE(rps_to_gt(rps), "mark interactive: %s\n",
		 str_yes_no(interactive));

	mutex_lock(&rps->power.mutex);
	if (interactive) {
		if (!rps->power.interactive++ && intel_rps_is_active(rps))
			rps_set_power(rps, HIGH_POWER);
	} else {
		GEM_BUG_ON(!rps->power.interactive);
		rps->power.interactive--;
	}
	mutex_unlock(&rps->power.mutex);
}

static int gen6_rps_set(struct intel_rps *rps, u8 val)
{
	struct intel_uncore *uncore = rps_to_uncore(rps);
	struct drm_i915_private *i915 = rps_to_i915(rps);
	u32 swreq;

	GEM_BUG_ON(rps_uses_slpc(rps));

	if (GRAPHICS_VER(i915) >= 9)
		swreq = GEN9_FREQUENCY(val);
	else if (IS_HASWELL(i915) || IS_BROADWELL(i915))
		swreq = HSW_FREQUENCY(val);
	else
		swreq = (GEN6_FREQUENCY(val) |
			 GEN6_OFFSET(0) |
			 GEN6_AGGRESSIVE_TURBO);
	set(uncore, GEN6_RPNSWREQ, swreq);

	GT_TRACE(rps_to_gt(rps), "set val:%x, freq:%d, swreq:%x\n",
		 val, intel_gpu_freq(rps, val), swreq);

	return 0;
}

static int rps_set(struct intel_rps *rps, u8 val, bool update)
{
	int err;

	if (val == rps->last_freq)
		return 0;

	err = gen6_rps_set(rps, val);
	if (err)
		return err;

	if (update)
		gen6_rps_set_thresholds(rps, val);
	rps->last_freq = val;

	return 0;
}

void intel_rps_unpark(struct intel_rps *rps)
{
	if (!intel_rps_is_enabled(rps))
		return;

	GT_TRACE(rps_to_gt(rps), "unpark:%x\n", rps->cur_freq);

	/*
	 * Use the user's desired frequency as a guide, but for better
	 * performance, jump directly to RPe as our starting frequency.
	 */
	mutex_lock(&rps->lock);

	intel_rps_set_active(rps);
	intel_rps_set(rps,
		      clamp(rps->cur_freq,
			    rps->min_freq_softlimit,
			    rps->max_freq_softlimit));

	mutex_unlock(&rps->lock);

	rps->pm_iir = 0;
	if (intel_rps_has_interrupts(rps))
		rps_enable_interrupts(rps);
	if (intel_rps_uses_timer(rps))
		rps_start_timer(rps);
}

void intel_rps_park(struct intel_rps *rps)
{
	int adj;

	if (!intel_rps_is_enabled(rps))
		return;

	if (!intel_rps_clear_active(rps))
		return;

	if (intel_rps_uses_timer(rps))
		rps_stop_timer(rps);
	if (intel_rps_has_interrupts(rps))
		rps_disable_interrupts(rps);

	if (rps->last_freq <= rps->idle_freq)
		return;

	/*
	 * The punit delays the write of the frequency and voltage until it
	 * determines the GPU is awake. During normal usage we don't want to
	 * waste power changing the frequency if the GPU is sleeping (rc6).
	 * However, the GPU and driver is now idle and we do not want to delay
	 * switching to minimum voltage (reducing power whilst idle) as we do
	 * not expect to be woken in the near future and so must flush the
	 * change by waking the device.
	 *
	 * We choose to take the media powerwell (either would do to trick the
	 * punit into committing the voltage change) as that takes a lot less
	 * power than the render powerwell.
	 */
	intel_uncore_forcewake_get(rps_to_uncore(rps), FORCEWAKE_MEDIA);
	rps_set(rps, rps->idle_freq, false);
	intel_uncore_forcewake_put(rps_to_uncore(rps), FORCEWAKE_MEDIA);

	/*
	 * Since we will try and restart from the previously requested
	 * frequency on unparking, treat this idle point as a downclock
	 * interrupt and reduce the frequency for resume. If we park/unpark
	 * more frequently than the rps worker can run, we will not respond
	 * to any EI and never see a change in frequency.
	 *
	 * (Note we accommodate Cherryview's limitation of only using an
	 * even bin by applying it to all.)
	 */
	adj = rps->last_adj;
	if (adj < 0)
		adj *= 2;
	else /* CHV needs even encode values */
		adj = -2;
	rps->last_adj = adj;
	rps->cur_freq = max_t(int, rps->cur_freq + adj, rps->min_freq);
	if (rps->cur_freq < rps->efficient_freq) {
		rps->cur_freq = rps->efficient_freq;
		rps->last_adj = 0;
	}

	GT_TRACE(rps_to_gt(rps), "park:%x\n", rps->cur_freq);
}

u32 intel_rps_get_boost_frequency(struct intel_rps *rps)
{
	struct intel_guc_slpc *slpc;

	if (rps_uses_slpc(rps)) {
		slpc = rps_to_slpc(rps);

		return slpc->boost_freq;
	} else {
		return intel_gpu_freq(rps, rps->boost_freq);
	}
}

static int rps_set_boost_freq(struct intel_rps *rps, u32 val)
{
	bool boost = false;

	/* Validate against (static) hardware limits */
	val = intel_freq_opcode(rps, val);
	if (val < rps->min_freq || val > rps->max_freq)
		return -EINVAL;

	mutex_lock(&rps->lock);
	if (val != rps->boost_freq) {
		rps->boost_freq = val;
		boost = atomic_read(&rps->num_waiters);
	}
	mutex_unlock(&rps->lock);
	if (boost)
		schedule_work(&rps->work);

	return 0;
}

int intel_rps_set_boost_frequency(struct intel_rps *rps, u32 freq)
{
	struct intel_guc_slpc *slpc;

	if (rps_uses_slpc(rps)) {
		slpc = rps_to_slpc(rps);

		return intel_guc_slpc_set_boost_freq(slpc, freq);
	} else {
		return rps_set_boost_freq(rps, freq);
	}
}

void intel_rps_cancel_boost(struct intel_rps *rps)
{
	if (rps_uses_slpc(rps))
		intel_guc_slpc_dec_waiters(rps_to_slpc(rps));
	else
		atomic_dec(&rps->num_waiters);
}

void intel_rps_boost(struct intel_rps *rps)
{
	if (rps_uses_slpc(rps)) {
		struct intel_guc_slpc *slpc = rps_to_slpc(rps);

		if (slpc->min_freq_softlimit < slpc->boost_freq &&
		    !atomic_fetch_inc(&slpc->num_waiters))
			schedule_work(&slpc->boost_work);

		return;
	}

	if (atomic_fetch_inc(&rps->num_waiters))
		return;

	if (!intel_rps_is_active(rps))
		return;

	if (READ_ONCE(rps->cur_freq) < rps->boost_freq)
		schedule_work(&rps->work);

	WRITE_ONCE(rps->boosts, rps->boosts + 1); /* debug only */
}

void intel_rps_boost_for_request(struct i915_request *rq)
{
	if (i915_request_signaled(rq) || i915_request_has_waitboost(rq))
		return;

	/* Serializes with i915_request_retire() */
	if (!test_and_set_bit(I915_FENCE_FLAG_BOOST, &rq->fence.flags)) {
		RQ_TRACE(rq, "rps boost\n");
		intel_rps_boost(&READ_ONCE(rq->engine)->gt->rps);
	}
}

int intel_rps_set(struct intel_rps *rps, u8 val)
{
	int err;

	lockdep_assert_held(&rps->lock);
	GEM_BUG_ON(val > rps->max_freq);
	GEM_BUG_ON(val < rps->min_freq);

	if (intel_rps_is_active(rps)) {
		err = rps_set(rps, val, true);
		if (err)
			return err;

		/*
		 * Make sure we continue to get interrupts
		 * until we hit the minimum or maximum frequencies.
		 */
		if (intel_rps_has_interrupts(rps)) {
			struct intel_uncore *uncore = rps_to_uncore(rps);

			set(uncore,
			    GEN6_RP_INTERRUPT_LIMITS, rps_limits(rps, val));

			set(uncore, GEN6_PMINTRMSK, rps_pm_mask(rps, val));
		}
	}

	rps->cur_freq = val;
	return 0;
}

static u32 intel_rps_read_state_cap(struct intel_rps *rps)
{
	struct drm_i915_private *i915 = rps_to_i915(rps);
	struct intel_uncore *uncore = rps_to_uncore(rps);

	if (IS_PONTEVECCHIO(i915))
		return intel_uncore_read(uncore, PVC_RP_STATE_CAP);
	else if (IS_GEN9_LP(i915))
		return intel_uncore_read(uncore, BXT_RP_STATE_CAP);
	else
		return intel_uncore_read(uncore, GEN6_RP_STATE_CAP);
}

static void
mtl_get_freq_caps(struct intel_rps *rps, struct intel_rps_freq_caps *caps)
{
	struct intel_uncore *uncore = rps_to_uncore(rps);
	u32 rp_state_cap = rps_to_gt(rps)->type == GT_MEDIA ?
				intel_uncore_read(uncore, MTL_MEDIAP_STATE_CAP) :
				intel_uncore_read(uncore, MTL_RP_STATE_CAP);
	u32 rpe = rps_to_gt(rps)->type == GT_MEDIA ?
			intel_uncore_read(uncore, MTL_MPE_FREQUENCY) :
			intel_uncore_read(uncore, MTL_GT_RPE_FREQUENCY);

	/* MTL values are in units of 16.67 MHz */
	caps->rp0_freq = REG_FIELD_GET(MTL_RP0_CAP_MASK, rp_state_cap);
	caps->min_freq = REG_FIELD_GET(MTL_RPN_CAP_MASK, rp_state_cap);
	caps->rp1_freq = REG_FIELD_GET(MTL_RPE_MASK, rpe);
}

static void
__gen6_rps_get_freq_caps(struct intel_rps *rps, struct intel_rps_freq_caps *caps)
{
	struct drm_i915_private *i915 = rps_to_i915(rps);
	u32 rp_state_cap;

	rp_state_cap = intel_rps_read_state_cap(rps);

	/* static values from HW: RP0 > RP1 > RPn (min_freq) */
	if (IS_GEN9_LP(i915)) {
		caps->rp0_freq = (rp_state_cap >> 16) & 0xff;
		caps->rp1_freq = (rp_state_cap >>  8) & 0xff;
		caps->min_freq = (rp_state_cap >>  0) & 0xff;
	} else {
		caps->rp0_freq = (rp_state_cap >>  0) & 0xff;
		caps->rp1_freq = (rp_state_cap >>  8) & 0xff;
		caps->min_freq = (rp_state_cap >> 16) & 0xff;
	}

	if (IS_GEN9_BC(i915) || GRAPHICS_VER(i915) >= 11) {
		/*
		 * In this case rp_state_cap register reports frequencies in
		 * units of 50 MHz. Convert these to the actual "hw unit", i.e.
		 * units of 16.67 MHz
		 */
		caps->rp0_freq *= GEN9_FREQ_SCALER;
		caps->rp1_freq *= GEN9_FREQ_SCALER;
		caps->min_freq *= GEN9_FREQ_SCALER;
	}
}

/**
 * gen6_rps_get_freq_caps - Get freq caps exposed by HW
 * @rps: the intel_rps structure
 * @caps: returned freq caps
 *
 * Returned "caps" frequencies should be converted to MHz using
 * intel_gpu_freq()
 */
void gen6_rps_get_freq_caps(struct intel_rps *rps, struct intel_rps_freq_caps *caps)
{
	struct drm_i915_private *i915 = rps_to_i915(rps);

	if (IS_METEORLAKE(i915))
		return mtl_get_freq_caps(rps, caps);
	else
		return __gen6_rps_get_freq_caps(rps, caps);
}

static void gen6_rps_init(struct intel_rps *rps)
{
	struct drm_i915_private *i915 = rps_to_i915(rps);
	struct intel_rps_freq_caps caps;

	gen6_rps_get_freq_caps(rps, &caps);
	rps->rp0_freq = caps.rp0_freq;
	rps->rp1_freq = caps.rp1_freq;
	rps->min_freq = caps.min_freq;

	/* hw_max = RP0 until we check for overclocking */
	rps->max_freq = rps->rp0_freq;

	rps->efficient_freq = rps->rp1_freq;
	if (IS_HASWELL(i915) || IS_BROADWELL(i915) ||
	    IS_GEN9_BC(i915) || GRAPHICS_VER(i915) >= 11) {
		u32 ddcc_status = 0;
		u32 mult = 1;

		if (IS_GEN9_BC(i915) || GRAPHICS_VER(i915) >= 11)
			mult = GEN9_FREQ_SCALER;
		if (snb_pcode_read(rps_to_gt(rps)->uncore,
				   HSW_PCODE_DYNAMIC_DUTY_CYCLE_CONTROL,
				   &ddcc_status, NULL) == 0)
			rps->efficient_freq =
				clamp_t(u32,
					((ddcc_status >> 8) & 0xff) * mult,
					rps->min_freq,
					rps->max_freq);
	}
}

static bool rps_reset(struct intel_rps *rps)
{
	/* force a reset */
	rps->power.mode = -1;
	rps->last_freq = -1;

	if (rps_set(rps, rps->min_freq, true)) {
		gt_err(rps_to_gt(rps), "Failed to reset RPS to initial values\n");
		return false;
	}

	rps->cur_freq = rps->min_freq;
	return true;
}

/* See the Gen9_GT_PM_Programming_Guide doc for the below */
static bool gen9_rps_enable(struct intel_rps *rps)
{
	struct intel_gt *gt = rps_to_gt(rps);
	struct intel_uncore *uncore = gt->uncore;

	/* Program defaults and thresholds for RPS */
	if (GRAPHICS_VER(gt->i915) == 9)
		intel_uncore_write_fw(uncore, GEN6_RC_VIDEO_FREQ,
				      GEN9_FREQUENCY(rps->rp1_freq));

	intel_uncore_write_fw(uncore, GEN6_RP_IDLE_HYSTERSIS, 0xa);

	rps->pm_events = GEN6_PM_RP_UP_THRESHOLD | GEN6_PM_RP_DOWN_THRESHOLD;

	return rps_reset(rps);
}

static bool has_busy_stats(struct intel_rps *rps)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	for_each_engine(engine, rps_to_gt(rps), id) {
		if (!intel_engine_supports_stats(engine))
			return false;
	}

	return true;
}

void intel_rps_enable(struct intel_rps *rps)
{
	struct drm_i915_private *i915 = rps_to_i915(rps);
	struct intel_uncore *uncore = rps_to_uncore(rps);
	bool enabled = false;

	if (rps_uses_slpc(rps))
		return;

	intel_gt_check_clock_frequency(rps_to_gt(rps));

	intel_uncore_forcewake_get(uncore, FORCEWAKE_ALL);
	if (rps->max_freq <= rps->min_freq)
		/* leave disabled, no room for dynamic reclocking */;
	else
		enabled = gen9_rps_enable(rps);
	intel_uncore_forcewake_put(uncore, FORCEWAKE_ALL);
	if (!enabled)
		return;

	GT_TRACE(rps_to_gt(rps),
		 "min:%x, max:%x, freq:[%d, %d]\n",
		 rps->min_freq, rps->max_freq,
		 intel_gpu_freq(rps, rps->min_freq),
		 intel_gpu_freq(rps, rps->max_freq));

	GEM_BUG_ON(rps->max_freq < rps->min_freq);
	GEM_BUG_ON(rps->idle_freq > rps->max_freq);

	GEM_BUG_ON(rps->efficient_freq < rps->min_freq);
	GEM_BUG_ON(rps->efficient_freq > rps->max_freq);

	if (has_busy_stats(rps))
		intel_rps_set_timer(rps);
	else if (GRAPHICS_VER(i915) >= 6 && GRAPHICS_VER(i915) <= 11)
		intel_rps_set_interrupts(rps);
	else
		/* Ironlake currently uses intel_ips.ko */ {}

	intel_rps_set_enabled(rps);
}

static void gen6_rps_disable(struct intel_rps *rps)
{
	set(rps_to_uncore(rps), GEN6_RP_CONTROL, 0);
}

void intel_rps_disable(struct intel_rps *rps)
{
	intel_rps_clear_enabled(rps);
	intel_rps_clear_interrupts(rps);
	intel_rps_clear_timer(rps);

	gen6_rps_disable(rps);
}

int intel_gpu_freq(struct intel_rps *rps, int val)
{
	return DIV_ROUND_CLOSEST(val * GT_FREQUENCY_MULTIPLIER, GEN9_FREQ_SCALER);
}

int intel_freq_opcode(struct intel_rps *rps, int val)
{
	return DIV_ROUND_CLOSEST(val * GEN9_FREQ_SCALER, GT_FREQUENCY_MULTIPLIER);
}

static void rps_work(struct work_struct *work)
{
	struct intel_rps *rps = container_of(work, typeof(*rps), work);
	struct intel_gt *gt = rps_to_gt(rps);
	struct drm_i915_private *i915 = rps_to_i915(rps);
	bool client_boost = false;
	int new_freq, adj, min, max;
	u32 pm_iir = 0;

	spin_lock_irq(gt->irq_lock);
	pm_iir = fetch_and_zero(&rps->pm_iir) & rps->pm_events;
	client_boost = atomic_read(&rps->num_waiters);
	spin_unlock_irq(gt->irq_lock);

	/* Make sure we didn't queue anything we're not going to process. */
	if (!pm_iir && !client_boost)
		goto out;

	mutex_lock(&rps->lock);
	if (!intel_rps_is_active(rps)) {
		mutex_unlock(&rps->lock);
		return;
	}

	adj = rps->last_adj;
	new_freq = rps->cur_freq;
	min = rps->min_freq_softlimit;
	max = rps->max_freq_softlimit;
	if (client_boost)
		max = rps->max_freq;

	GT_TRACE(gt,
		 "pm_iir:%x, client_boost:%s, last:%d, cur:%x, min:%x, max:%x\n",
		 pm_iir, str_yes_no(client_boost),
		 adj, new_freq, min, max);

	if (client_boost && new_freq < rps->boost_freq) {
		new_freq = rps->boost_freq;
		adj = 0;
	} else if (pm_iir & GEN6_PM_RP_UP_THRESHOLD) {
		if (adj > 0)
			adj *= 2;
		else /* CHV needs even encode values */
			adj = IS_CHERRYVIEW(gt->i915) ? 2 : 1;

		if (new_freq >= rps->max_freq_softlimit)
			adj = 0;
	} else if (client_boost) {
		adj = 0;
	} else if (pm_iir & GEN6_PM_RP_DOWN_TIMEOUT) {
		if (rps->cur_freq > rps->efficient_freq)
			new_freq = rps->efficient_freq;
		else if (rps->cur_freq > rps->min_freq_softlimit)
			new_freq = rps->min_freq_softlimit;
		adj = 0;
	} else if (pm_iir & GEN6_PM_RP_DOWN_THRESHOLD) {
		if (adj < 0)
			adj *= 2;
		else /* CHV needs even encode values */
			adj = IS_CHERRYVIEW(gt->i915) ? -2 : -1;

		if (new_freq <= rps->min_freq_softlimit)
			adj = 0;
	} else { /* unknown event */
		adj = 0;
	}

	/*
	 * sysfs frequency limits may have snuck in while
	 * servicing the interrupt
	 */
	new_freq += adj;
	new_freq = clamp_t(int, new_freq, min, max);

	if (intel_rps_set(rps, new_freq)) {
		drm_dbg(&i915->drm, "Failed to set new GPU frequency\n");
		adj = 0;
	}
	rps->last_adj = adj;

	mutex_unlock(&rps->lock);

out:
	spin_lock_irq(gt->irq_lock);
	gen6_gt_pm_unmask_irq(gt, rps->pm_events);
	spin_unlock_irq(gt->irq_lock);
}

void gen11_rps_irq_handler(struct intel_rps *rps, u32 pm_iir)
{
	struct intel_gt *gt = rps_to_gt(rps);
	const u32 events = rps->pm_events & pm_iir;

	lockdep_assert_held(gt->irq_lock);

	if (unlikely(!events))
		return;

	GT_TRACE(gt, "irq events:%x\n", events);

	gen6_gt_pm_mask_irq(gt, events);

	rps->pm_iir |= events;
	schedule_work(&rps->work);
}

void gen6_rps_irq_handler(struct intel_rps *rps, u32 pm_iir)
{
	struct intel_gt *gt = rps_to_gt(rps);
	u32 events;

	events = pm_iir & rps->pm_events;
	if (events) {
		spin_lock(gt->irq_lock);

		GT_TRACE(gt, "irq events:%x\n", events);

		gen6_gt_pm_mask_irq(gt, events);
		rps->pm_iir |= events;

		schedule_work(&rps->work);
		spin_unlock(gt->irq_lock);
	}

	if (GRAPHICS_VER(gt->i915) >= 8)
		return;

	if (pm_iir & PM_VEBOX_USER_INTERRUPT)
		intel_engine_cs_irq(gt->engine[VECS0], pm_iir >> 10);

	if (pm_iir & PM_VEBOX_CS_ERROR_INTERRUPT)
		DRM_DEBUG("Command parser error, pm_iir 0x%08x\n", pm_iir);
}

void intel_rps_init_early(struct intel_rps *rps)
{
	mutex_init(&rps->lock);
	mutex_init(&rps->power.mutex);

	INIT_WORK(&rps->work, rps_work);
	timer_setup(&rps->timer, rps_timer, 0);

	atomic_set(&rps->num_waiters, 0);
}

void intel_rps_init(struct intel_rps *rps)
{
	struct drm_i915_private *i915 = rps_to_i915(rps);

	if (IS_SRIOV_VF(i915))
		return;

	if (!rps_supported(rps))
		return;

	if (rps_uses_slpc(rps))
		return;

	gen6_rps_init(rps);

	/* Derive initial user preferences/limits from the hardware limits */
	rps->max_freq_softlimit = rps->max_freq;
	rps_to_gt(rps)->rps_defaults.max_freq = rps->max_freq_softlimit;
	rps->min_freq_softlimit = rps->min_freq;
	rps_to_gt(rps)->rps_defaults.min_freq = rps->min_freq_softlimit;

	/* After setting max-softlimit, find the overclock max freq */
	if (GRAPHICS_VER(i915) == 6 || IS_IVYBRIDGE(i915) || IS_HASWELL(i915)) {
		u32 params = 0;

		snb_pcode_read(rps_to_gt(rps)->uncore, GEN6_READ_OC_PARAMS, &params, NULL);
		if (params & BIT(31)) { /* OC supported */
			drm_dbg(&i915->drm,
				"Overclocking supported, max: %dMHz, overclock: %dMHz\n",
				(rps->max_freq & 0xff) * 50,
				(params & 0xff) * 50);
			rps->max_freq = params & 0xff;
		}
	}

	/* Finally allow us to boost to max by default */
	rps->boost_freq = rps->max_freq;
	rps_to_gt(rps)->rps_defaults.boost_freq = rps->boost_freq;
	rps->idle_freq = rps->min_freq;

	/* Start in the middle, from here we will autotune based on workload */
	rps->cur_freq = rps->efficient_freq;

	rps->pm_intrmsk_mbz = 0;

	/*
	 * SNB,IVB,HSW can while VLV,CHV may hard hang on looping batchbuffer
	 * if GEN6_PM_UP_EI_EXPIRED is masked.
	 *
	 * TODO: verify if this can be reproduced on VLV,CHV.
	 */
	if (GRAPHICS_VER(i915) <= 7)
		rps->pm_intrmsk_mbz |= GEN6_PM_RP_UP_EI_EXPIRED;

	if (GRAPHICS_VER(i915) >= 8 && GRAPHICS_VER(i915) < 11)
		rps->pm_intrmsk_mbz |= GEN8_PMINTR_DISABLE_REDIRECT_TO_GUC;

	/* GuC needs ARAT expired interrupt unmasked */
	if (intel_uc_uses_guc_submission(&rps_to_gt(rps)->uc))
		rps->pm_intrmsk_mbz |= ARAT_EXPIRED_INTRMSK;
}

void intel_rps_sanitize(struct intel_rps *rps)
{
	if (IS_SRIOV_VF(rps_to_i915(rps)))
		return;

	if (!rps_supported(rps))
		return;

	if (rps_uses_slpc(rps))
		return;

	if (GRAPHICS_VER(rps_to_i915(rps)) >= 6)
		rps_disable_interrupts(rps);
}

static u32 intel_rps_read_rpstat(struct intel_rps *rps)
{
	struct drm_i915_private *i915 = rps_to_i915(rps);
	i915_reg_t rpstat;

	if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 70))
		rpstat = MTL_MIRROR_TARGET_WP1;
	else if (GRAPHICS_VER(i915) >= 12)
		rpstat = GEN12_RPSTAT1;
	else
		rpstat = GEN6_RPSTAT1;

	return intel_uncore_read(rps_to_gt(rps)->uncore, rpstat);
}

static u32 intel_rps_get_cagf(struct intel_rps *rps, u32 rpstat)
{
	struct drm_i915_private *i915 = rps_to_i915(rps);
	u32 cagf;

	if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 70))
		cagf = REG_FIELD_GET(MTL_CAGF_MASK, rpstat);
	else
		cagf = REG_FIELD_GET(GEN12_CAGF_MASK, rpstat);

	return cagf;
}

static u32 __read_cagf(struct intel_rps *rps, bool take_fw)
{
	struct drm_i915_private *i915 = rps_to_i915(rps);
	struct intel_uncore *uncore = rps_to_uncore(rps);
	i915_reg_t r = INVALID_MMIO_REG;
	u32 freq;

	/*
	 * For Gen12+ reading freq from HW does not need a forcewake and
	 * registers will return 0 freq when GT is in RC6
	 */
	if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 70))
		r = MTL_MIRROR_TARGET_WP1;
	else
		r = GEN12_RPSTAT1;

	freq = take_fw ? intel_uncore_read(uncore, r) : intel_uncore_read_fw(uncore, r);
	return intel_rps_get_cagf(rps, freq);
}

static u32 read_cagf(struct intel_rps *rps)
{
	return __read_cagf(rps, true);
}

u32 intel_rps_read_actual_frequency(struct intel_rps *rps)
{
	struct intel_runtime_pm *rpm = rps_to_uncore(rps)->rpm;
	intel_wakeref_t wakeref;
	u32 freq = 0;

	with_intel_runtime_pm_if_in_use(rpm, wakeref)
		freq = intel_gpu_freq(rps, read_cagf(rps));

	return freq;
}

u32 intel_rps_read_actual_frequency_fw(struct intel_rps *rps)
{
	return intel_gpu_freq(rps, __read_cagf(rps, false));
}

u32 intel_rps_read_chiplet_frequency(struct intel_rps *rps)
{
	struct intel_runtime_pm *rpm = rps_to_uncore(rps)->rpm;
	intel_wakeref_t wakeref;
	u32 val = 0;

	with_intel_runtime_pm_if_in_use(rpm, wakeref)
		val = intel_uncore_read_fw(rps_to_uncore(rps), GEN12_RPSTAT1);

	val = REG_FIELD_GET(PVC_RPSTAT1_CHIPLET_FREQ, val);
	return intel_gpu_freq(rps, val);
}

static u32 __rps_read_mmio(struct intel_gt *gt, i915_reg_t reg32)
{
	intel_wakeref_t wakeref;
	u32 val = 0;

	with_intel_runtime_pm(gt->uncore->rpm, wakeref)
		val = intel_uncore_read(gt->uncore, reg32);

	return val;
}

static u32 intel_rps_read_punit_req(struct intel_rps *rps)
{
	struct intel_gt *gt = rps_to_gt(rps);
	u32 pureq = __rps_read_mmio(gt, GEN6_RPNSWREQ);

	return pureq;
}

static u32 intel_rps_get_req(u32 pureq)
{
	u32 req = pureq >> GEN9_SW_REQ_UNSLICE_RATIO_SHIFT;

	return req;
}

u32 intel_rps_read_punit_req_frequency(struct intel_rps *rps)
{
	u32 freq = intel_rps_get_req(intel_rps_read_punit_req(rps));

	return intel_gpu_freq(rps, freq);
}

u32 intel_rps_get_requested_frequency(struct intel_rps *rps)
{
	if (rps_uses_slpc(rps))
		return intel_rps_read_punit_req_frequency(rps);
	else
		return intel_gpu_freq(rps, rps->cur_freq);
}

u32 intel_rps_get_max_frequency(struct intel_rps *rps)
{
	struct intel_guc_slpc *slpc = rps_to_slpc(rps);

	if (rps_uses_slpc(rps))
		return slpc->max_freq_softlimit;
	else
		return intel_gpu_freq(rps, rps->max_freq_softlimit);
}

/**
 * intel_rps_get_max_raw_freq - returns the max frequency in some raw format.
 * @rps: the intel_rps structure
 *
 * Returns the max frequency in a raw format. In newer platforms raw is in
 * units of 50 MHz.
 */
u32 intel_rps_get_max_raw_freq(struct intel_rps *rps)
{
	struct intel_guc_slpc *slpc = rps_to_slpc(rps);
	u32 freq;

	if (rps_uses_slpc(rps)) {
		return DIV_ROUND_CLOSEST(slpc->rp0_freq,
					 GT_FREQUENCY_MULTIPLIER);
	} else {
		freq = rps->max_freq;
		if (GRAPHICS_VER(rps_to_i915(rps)) >= 9) {
			/* Convert GT frequency to 50 MHz units */
			freq /= GEN9_FREQ_SCALER;
		}
		return freq;
	}
}

u32 intel_rps_get_rp0_frequency(struct intel_rps *rps)
{
	struct intel_guc_slpc *slpc = rps_to_slpc(rps);

	if (rps_uses_slpc(rps))
		return slpc->rp0_freq;
	else
		return intel_gpu_freq(rps, rps->rp0_freq);
}

u32 intel_rps_get_rp1_frequency(struct intel_rps *rps)
{
	struct intel_guc_slpc *slpc = rps_to_slpc(rps);

	if (rps_uses_slpc(rps))
		return slpc->rp1_freq;
	else
		return intel_gpu_freq(rps, rps->rp1_freq);
}

u32 intel_rps_get_rpn_frequency(struct intel_rps *rps)
{
	struct intel_guc_slpc *slpc = rps_to_slpc(rps);

	if (rps_uses_slpc(rps))
		return slpc->min_freq;
	else
		return intel_gpu_freq(rps, rps->min_freq);
}

static void rps_frequency_dump(struct intel_rps *rps, struct drm_printer *p)
{
	struct intel_gt *gt = rps_to_gt(rps);
	struct drm_i915_private *i915 = gt->i915;
	struct intel_uncore *uncore = gt->uncore;
	struct intel_rps_freq_caps caps;
	u32 rp_state_limits;
	u32 gt_perf_status;
	u32 rpmodectl, rpinclimit, rpdeclimit;
	u32 rpstat, cagf, reqf;
	u32 rpcurupei, rpcurup, rpprevup;
	u32 rpcurdownei, rpcurdown, rpprevdown;
	u32 rpupei, rpupt, rpdownei, rpdownt;
	u32 pm_ier, pm_imr, pm_isr, pm_iir, pm_mask;

	rp_state_limits = intel_uncore_read(uncore, GEN6_RP_STATE_LIMITS);
	gen6_rps_get_freq_caps(rps, &caps);
	if (IS_GEN9_LP(i915))
		gt_perf_status = intel_uncore_read(uncore, BXT_GT_PERF_STATUS);
	else
		gt_perf_status = intel_uncore_read(uncore, GEN6_GT_PERF_STATUS);

	/* RPSTAT1 is in the GT power well */
	intel_uncore_forcewake_get(uncore, FORCEWAKE_ALL);

	reqf = intel_uncore_read(uncore, GEN6_RPNSWREQ);
	if (GRAPHICS_VER(i915) >= 9) {
		reqf >>= 23;
	} else {
		reqf &= ~GEN6_TURBO_DISABLE;
		if (IS_HASWELL(i915) || IS_BROADWELL(i915))
			reqf >>= 24;
		else
			reqf >>= 25;
	}
	reqf = intel_gpu_freq(rps, reqf);

	rpmodectl = intel_uncore_read(uncore, GEN6_RP_CONTROL);
	rpinclimit = intel_uncore_read(uncore, GEN6_RP_UP_THRESHOLD);
	rpdeclimit = intel_uncore_read(uncore, GEN6_RP_DOWN_THRESHOLD);

	rpstat = intel_rps_read_rpstat(rps);
	rpcurupei = intel_uncore_read(uncore, GEN6_RP_CUR_UP_EI) & GEN6_CURICONT_MASK;
	rpcurup = intel_uncore_read(uncore, GEN6_RP_CUR_UP) & GEN6_CURBSYTAVG_MASK;
	rpprevup = intel_uncore_read(uncore, GEN6_RP_PREV_UP) & GEN6_CURBSYTAVG_MASK;
	rpcurdownei = intel_uncore_read(uncore, GEN6_RP_CUR_DOWN_EI) & GEN6_CURIAVG_MASK;
	rpcurdown = intel_uncore_read(uncore, GEN6_RP_CUR_DOWN) & GEN6_CURBSYTAVG_MASK;
	rpprevdown = intel_uncore_read(uncore, GEN6_RP_PREV_DOWN) & GEN6_CURBSYTAVG_MASK;

	rpupei = intel_uncore_read(uncore, GEN6_RP_UP_EI);
	rpupt = intel_uncore_read(uncore, GEN6_RP_UP_THRESHOLD);

	rpdownei = intel_uncore_read(uncore, GEN6_RP_DOWN_EI);
	rpdownt = intel_uncore_read(uncore, GEN6_RP_DOWN_THRESHOLD);

	cagf = intel_rps_read_actual_frequency(rps);

	intel_uncore_forcewake_put(uncore, FORCEWAKE_ALL);

	if (GRAPHICS_VER(i915) >= 11) {
		pm_ier = intel_uncore_read(uncore, GEN11_GPM_WGBOXPERF_INTR_ENABLE);
		pm_imr = intel_uncore_read(uncore, GEN11_GPM_WGBOXPERF_INTR_MASK);
		/*
		 * The equivalent to the PM ISR & IIR cannot be read
		 * without affecting the current state of the system
		 */
		pm_isr = 0;
		pm_iir = 0;
	} else if (GRAPHICS_VER(i915) >= 8) {
		pm_ier = intel_uncore_read(uncore, GEN8_GT_IER(2));
		pm_imr = intel_uncore_read(uncore, GEN8_GT_IMR(2));
		pm_isr = intel_uncore_read(uncore, GEN8_GT_ISR(2));
		pm_iir = intel_uncore_read(uncore, GEN8_GT_IIR(2));
	} else {
		pm_ier = intel_uncore_read(uncore, GEN6_PMIER);
		pm_imr = intel_uncore_read(uncore, GEN6_PMIMR);
		pm_isr = intel_uncore_read(uncore, GEN6_PMISR);
		pm_iir = intel_uncore_read(uncore, GEN6_PMIIR);
	}
	pm_mask = intel_uncore_read(uncore, GEN6_PMINTRMSK);

	drm_printf(p, "Video Turbo Mode: %s\n",
		   str_yes_no(rpmodectl & GEN6_RP_MEDIA_TURBO));
	drm_printf(p, "HW control enabled: %s\n",
		   str_yes_no(rpmodectl & GEN6_RP_ENABLE));
	drm_printf(p, "SW control enabled: %s\n",
		   str_yes_no((rpmodectl & GEN6_RP_MEDIA_MODE_MASK) == GEN6_RP_MEDIA_SW_MODE));

	drm_printf(p, "PM IER=0x%08x IMR=0x%08x, MASK=0x%08x\n",
		   pm_ier, pm_imr, pm_mask);
	if (GRAPHICS_VER(i915) <= 10)
		drm_printf(p, "PM ISR=0x%08x IIR=0x%08x\n",
			   pm_isr, pm_iir);
	drm_printf(p, "pm_intrmsk_mbz: 0x%08x\n",
		   rps->pm_intrmsk_mbz);
	drm_printf(p, "GT_PERF_STATUS: 0x%08x\n", gt_perf_status);
	drm_printf(p, "Render p-state ratio: %d\n",
		   (gt_perf_status & (GRAPHICS_VER(i915) >= 9 ? 0x1ff00 : 0xff00)) >> 8);
	drm_printf(p, "Render p-state VID: %d\n",
		   gt_perf_status & 0xff);
	drm_printf(p, "Render p-state limit: %d\n",
		   rp_state_limits & 0xff);
	drm_printf(p, "RPSTAT1: 0x%08x\n", rpstat);
	drm_printf(p, "RPMODECTL: 0x%08x\n", rpmodectl);
	drm_printf(p, "RPINCLIMIT: 0x%08x\n", rpinclimit);
	drm_printf(p, "RPDECLIMIT: 0x%08x\n", rpdeclimit);
	drm_printf(p, "RPNSWREQ: %dMHz\n", reqf);
	drm_printf(p, "CAGF: %dMHz\n", cagf);
	drm_printf(p, "RP CUR UP EI: %d (%lldns)\n",
		   rpcurupei,
		   intel_gt_pm_interval_to_ns(gt, rpcurupei));
	drm_printf(p, "RP CUR UP: %d (%lldns)\n",
		   rpcurup, intel_gt_pm_interval_to_ns(gt, rpcurup));
	drm_printf(p, "RP PREV UP: %d (%lldns)\n",
		   rpprevup, intel_gt_pm_interval_to_ns(gt, rpprevup));
	drm_printf(p, "Up threshold: %d%%\n",
		   rps->power.up_threshold);
	drm_printf(p, "RP UP EI: %d (%lldns)\n",
		   rpupei, intel_gt_pm_interval_to_ns(gt, rpupei));
	drm_printf(p, "RP UP THRESHOLD: %d (%lldns)\n",
		   rpupt, intel_gt_pm_interval_to_ns(gt, rpupt));

	drm_printf(p, "RP CUR DOWN EI: %d (%lldns)\n",
		   rpcurdownei,
		   intel_gt_pm_interval_to_ns(gt, rpcurdownei));
	drm_printf(p, "RP CUR DOWN: %d (%lldns)\n",
		   rpcurdown,
		   intel_gt_pm_interval_to_ns(gt, rpcurdown));
	drm_printf(p, "RP PREV DOWN: %d (%lldns)\n",
		   rpprevdown,
		   intel_gt_pm_interval_to_ns(gt, rpprevdown));
	drm_printf(p, "Down threshold: %d%%\n",
		   rps->power.down_threshold);
	drm_printf(p, "RP DOWN EI: %d (%lldns)\n",
		   rpdownei, intel_gt_pm_interval_to_ns(gt, rpdownei));
	drm_printf(p, "RP DOWN THRESHOLD: %d (%lldns)\n",
		   rpdownt, intel_gt_pm_interval_to_ns(gt, rpdownt));

	drm_printf(p, "Lowest (RPN) frequency: %dMHz\n",
		   intel_gpu_freq(rps, caps.min_freq));
	drm_printf(p, "Nominal (RP1) frequency: %dMHz\n",
		   intel_gpu_freq(rps, caps.rp1_freq));
	drm_printf(p, "Max non-overclocked (RP0) frequency: %dMHz\n",
		   intel_gpu_freq(rps, caps.rp0_freq));
	drm_printf(p, "Max overclocked frequency: %dMHz\n",
		   intel_gpu_freq(rps, rps->max_freq));

	drm_printf(p, "Current freq: %d MHz\n",
		   intel_gpu_freq(rps, rps->cur_freq));
	drm_printf(p, "Actual freq: %d MHz\n", cagf);
	drm_printf(p, "Idle freq: %d MHz\n",
		   intel_gpu_freq(rps, rps->idle_freq));
	drm_printf(p, "Min freq: %d MHz\n",
		   intel_gpu_freq(rps, rps->min_freq));
	drm_printf(p, "Boost freq: %d MHz\n",
		   intel_gpu_freq(rps, rps->boost_freq));
	drm_printf(p, "Max freq: %d MHz\n",
		   intel_gpu_freq(rps, rps->max_freq));
	drm_printf(p,
		   "efficient (RPe) frequency: %d MHz\n",
		   intel_gpu_freq(rps, rps->efficient_freq));
}

static void slpc_frequency_dump(struct intel_rps *rps, struct drm_printer *p)
{
	struct intel_gt *gt = rps_to_gt(rps);
	struct intel_uncore *uncore = gt->uncore;
	struct intel_rps_freq_caps caps;
	u32 pm_mask;

	gen6_rps_get_freq_caps(rps, &caps);
	pm_mask = intel_uncore_read(uncore, GEN6_PMINTRMSK);

	drm_printf(p, "PM MASK=0x%08x\n", pm_mask);
	drm_printf(p, "pm_intrmsk_mbz: 0x%08x\n",
		   rps->pm_intrmsk_mbz);
	drm_printf(p, "RPSTAT1: 0x%08x\n", intel_rps_read_rpstat(rps));
	drm_printf(p, "RPNSWREQ: %dMHz\n", intel_rps_get_requested_frequency(rps));
	drm_printf(p, "Lowest (RPN) frequency: %dMHz\n",
		   intel_gpu_freq(rps, caps.min_freq));
	drm_printf(p, "Nominal (RP1) frequency: %dMHz\n",
		   intel_gpu_freq(rps, caps.rp1_freq));
	drm_printf(p, "Max non-overclocked (RP0) frequency: %dMHz\n",
		   intel_gpu_freq(rps, caps.rp0_freq));
	drm_printf(p, "Current freq: %d MHz\n",
		   intel_rps_get_requested_frequency(rps));
	drm_printf(p, "Actual freq: %d MHz\n",
		   intel_rps_read_actual_frequency(rps));
	drm_printf(p, "Min freq: %d MHz\n",
		   intel_rps_get_min_frequency(rps));
	drm_printf(p, "Boost freq: %d MHz\n",
		   intel_rps_get_boost_frequency(rps));
	drm_printf(p, "Max freq: %d MHz\n",
		   intel_rps_get_max_frequency(rps));
	drm_printf(p,
		   "efficient (RPe) frequency: %d MHz\n",
		   intel_gpu_freq(rps, caps.rp1_freq));
}

void gen6_rps_frequency_dump(struct intel_rps *rps, struct drm_printer *p)
{
	if (rps_uses_slpc(rps))
		return slpc_frequency_dump(rps, p);
	else
		return rps_frequency_dump(rps, p);
}

static int set_max_freq(struct intel_rps *rps, u32 val)
{
	struct drm_i915_private *i915 = rps_to_i915(rps);
	int ret = 0;

	mutex_lock(&rps->lock);

	val = intel_freq_opcode(rps, val);
	if (val < rps->min_freq ||
	    val > rps->max_freq ||
	    val < rps->min_freq_softlimit) {
		ret = -EINVAL;
		goto unlock;
	}

	if (val > rps->rp0_freq)
		drm_dbg(&i915->drm, "User requested overclocking to %d\n",
			intel_gpu_freq(rps, val));

	rps->max_freq_softlimit = val;

	val = clamp_t(int, rps->cur_freq,
		      rps->min_freq_softlimit,
		      rps->max_freq_softlimit);

	/*
	 * We still need *_set_rps to process the new max_delay and
	 * update the interrupt limits and PMINTRMSK even though
	 * frequency request may be unchanged.
	 */
	intel_rps_set(rps, val);

unlock:
	mutex_unlock(&rps->lock);

	return ret;
}

int intel_rps_set_max_frequency(struct intel_rps *rps, u32 val)
{
	struct intel_guc_slpc *slpc = rps_to_slpc(rps);

	if (rps_uses_slpc(rps))
		return intel_guc_slpc_set_max_freq(slpc, val);
	else
		return set_max_freq(rps, val);
}

u32 intel_rps_get_min_frequency(struct intel_rps *rps)
{
	struct intel_guc_slpc *slpc = rps_to_slpc(rps);

	if (rps_uses_slpc(rps))
		return slpc->min_freq_softlimit;
	else
		return intel_gpu_freq(rps, rps->min_freq_softlimit);
}

/**
 * intel_rps_get_min_raw_freq - returns the min frequency in some raw format.
 * @rps: the intel_rps structure
 *
 * Returns the min frequency in a raw format. In newer platforms raw is in
 * units of 50 MHz.
 */
u32 intel_rps_get_min_raw_freq(struct intel_rps *rps)
{
	struct intel_guc_slpc *slpc = rps_to_slpc(rps);
	u32 freq;

	if (rps_uses_slpc(rps)) {
		return DIV_ROUND_CLOSEST(slpc->min_freq,
					 GT_FREQUENCY_MULTIPLIER);
	} else {
		freq = rps->min_freq;
		if (GRAPHICS_VER(rps_to_i915(rps)) >= 9) {
			/* Convert GT frequency to 50 MHz units */
			freq /= GEN9_FREQ_SCALER;
		}
		return freq;
	}
}

static int set_min_freq(struct intel_rps *rps, u32 val)
{
	int ret = 0;

	mutex_lock(&rps->lock);

	val = intel_freq_opcode(rps, val);
	if (val < rps->min_freq ||
	    val > rps->max_freq ||
	    val > rps->max_freq_softlimit) {
		ret = -EINVAL;
		goto unlock;
	}

	rps->min_freq_softlimit = val;

	val = clamp_t(int, rps->cur_freq,
		      rps->min_freq_softlimit,
		      rps->max_freq_softlimit);

	/*
	 * We still need *_set_rps to process the new min_delay and
	 * update the interrupt limits and PMINTRMSK even though
	 * frequency request may be unchanged.
	 */
	intel_rps_set(rps, val);

unlock:
	mutex_unlock(&rps->lock);

	return ret;
}

int intel_rps_set_min_frequency(struct intel_rps *rps, u32 val)
{
	struct intel_guc_slpc *slpc = rps_to_slpc(rps);

	if (rps_uses_slpc(rps))
		return intel_guc_slpc_set_min_freq(slpc, val);
	else
		return set_min_freq(rps, val);
}

static void intel_rps_set_manual(struct intel_rps *rps, bool enable)
{
	struct intel_uncore *uncore = rps_to_uncore(rps);
	u32 state = enable ? GEN9_RPSWCTL_ENABLE : GEN9_RPSWCTL_DISABLE;

	/* Allow punit to process software requests */
	intel_uncore_write(uncore, GEN6_RP_CONTROL, state);
}

void intel_rps_raise_unslice(struct intel_rps *rps)
{
	struct intel_uncore *uncore = rps_to_uncore(rps);

	if (!rps_supported(rps))
		return;

	mutex_lock(&rps->lock);

	if (rps_uses_slpc(rps)) {
		/* RP limits have not been initialized yet for SLPC path */
		struct intel_rps_freq_caps caps;

		gen6_rps_get_freq_caps(rps, &caps);

		intel_rps_set_manual(rps, true);
		intel_uncore_write(uncore, GEN6_RPNSWREQ,
				   ((caps.rp0_freq <<
				   GEN9_SW_REQ_UNSLICE_RATIO_SHIFT) |
				   GEN9_IGNORE_SLICE_RATIO));
		intel_rps_set_manual(rps, false);
	} else {
		intel_rps_set(rps, rps->rp0_freq);
	}

	mutex_unlock(&rps->lock);
}

void intel_rps_lower_unslice(struct intel_rps *rps)
{
	struct intel_uncore *uncore = rps_to_uncore(rps);

	if (!rps_supported(rps))
		return;

	mutex_lock(&rps->lock);

	if (rps_uses_slpc(rps)) {
		/* RP limits have not been initialized yet for SLPC path */
		struct intel_rps_freq_caps caps;

		gen6_rps_get_freq_caps(rps, &caps);

		intel_rps_set_manual(rps, true);
		intel_uncore_write(uncore, GEN6_RPNSWREQ,
				   ((caps.min_freq <<
				   GEN9_SW_REQ_UNSLICE_RATIO_SHIFT) |
				   GEN9_IGNORE_SLICE_RATIO));
		intel_rps_set_manual(rps, false);
	} else {
		intel_rps_set(rps, rps->min_freq);
	}

	mutex_unlock(&rps->lock);
}

u32 intel_rps_read_rapl_pl1(struct intel_rps *rps)
{
	struct drm_i915_private *i915 = rps_to_i915(rps);
	struct intel_gt *gt = rps_to_gt(rps);
	i915_reg_t rgadr;
	u32 rapl_pl1;

	if (IS_PONTEVECCHIO(i915)) {
		rgadr = PVC_RAPL_PL1_FREQ_LIMIT;
	} else if (IS_DG1(i915) || IS_DG2(i915)) {
		rgadr = GEN9_RAPL_PL1_FREQ_LIMIT;
	} else {
		MISSING_CASE(GRAPHICS_VER(i915));
		rgadr = INVALID_MMIO_REG;
	}

	if (!i915_mmio_reg_valid(rgadr))
		rapl_pl1 = 0;
	else
		rapl_pl1 = __rps_read_mmio(gt, rgadr);

	return rapl_pl1;
}

u32 intel_rps_get_rapl(struct intel_rps *rps, u32 rapl_pl1)
{
	struct drm_i915_private *i915 = rps_to_i915(rps);
	u32 rapl = 0;

	if (IS_PONTEVECCHIO(i915))
		rapl = rapl_pl1 & RAPL_PL1_FREQ_LIMIT_MASK;
	else if (IS_DG1(i915) || IS_DG2(i915))
		rapl = le32_get_bits(rapl_pl1, GEN9_RAPL_PL1_FREQ_LIMIT_MASK);
	else
		MISSING_CASE(GRAPHICS_VER(i915));

	return rapl;
}

u32 intel_rps_read_rapl_pl1_frequency(struct intel_rps *rps)
{
	u32 rapl_freq = intel_rps_get_rapl(rps, intel_rps_read_rapl_pl1(rps));

	return (rapl_freq >> 8) * GT_FREQUENCY_MULTIPLIER;
}

static u32 read_perf_limit_reasons(struct intel_gt *gt)
{
	return __rps_read_mmio(gt, intel_gt_perf_limit_reasons_reg(gt));
}

u32 intel_rps_read_throttle_reason_status(struct intel_rps *rps)
{
	struct intel_gt *gt = rps_to_gt(rps);
	u32 status = read_perf_limit_reasons(gt) & GT0_PERF_LIMIT_REASONS_MASK;

	return status;
}

u32 intel_rps_read_throttle_reason_pl1(struct intel_rps *rps)
{
	struct intel_gt *gt = rps_to_gt(rps);
	u32 pl1 = read_perf_limit_reasons(gt) & POWER_LIMIT_1_MASK;

	return pl1;
}

u32 intel_rps_read_throttle_reason_pl2(struct intel_rps *rps)
{
	struct intel_gt *gt = rps_to_gt(rps);
	u32 pl2 = read_perf_limit_reasons(gt) & POWER_LIMIT_2_MASK;

	return pl2;
}

u32 intel_rps_read_throttle_reason_pl4(struct intel_rps *rps)
{
	struct intel_gt *gt = rps_to_gt(rps);
	u32 pl4 = read_perf_limit_reasons(gt) & POWER_LIMIT_4_MASK;

	return pl4;
}

u32 intel_rps_read_throttle_reason_thermal(struct intel_rps *rps)
{
	struct intel_gt *gt = rps_to_gt(rps);
	u32 thermal = read_perf_limit_reasons(gt) & THERMAL_LIMIT_MASK;

	return thermal;
}

u32 intel_rps_read_throttle_reason_prochot(struct intel_rps *rps)
{
	struct intel_gt *gt = rps_to_gt(rps);
	u32 prochot = read_perf_limit_reasons(gt) & PROCHOT_MASK;

	return prochot;
}

u32 intel_rps_read_throttle_reason_ratl(struct intel_rps *rps)
{
	struct intel_gt *gt = rps_to_gt(rps);
	u32 ratl = read_perf_limit_reasons(gt) & RATL_MASK;

	return ratl;
}

u32 intel_rps_read_throttle_reason_vr_thermalert(struct intel_rps *rps)
{
	struct intel_gt *gt = rps_to_gt(rps);
	u32 thermalert = read_perf_limit_reasons(gt) & VR_THERMALERT_MASK;

	return thermalert;
}

u32 intel_rps_read_throttle_reason_vr_tdc(struct intel_rps *rps)
{
	struct intel_gt *gt = rps_to_gt(rps);
	u32 tdc = read_perf_limit_reasons(gt) & VR_TDC_MASK;

	return tdc;
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftest_slpc.c"
#endif
