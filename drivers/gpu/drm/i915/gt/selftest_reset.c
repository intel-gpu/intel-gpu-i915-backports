// SPDX-License-Identifier: MIT
/*
 * Copyright © 2018 Intel Corporation
 */

#include <linux/crc32.h>

#include "gem/i915_gem_stolen.h"

#include "i915_memcpy.h"
#include "i915_selftest.h"
#include "intel_gpu_commands.h"
#include "selftests/igt_reset.h"
#include "selftests/igt_atomic.h"
#include "selftests/igt_spinner.h"

static int igt_global_reset(void *arg)
{
	struct intel_gt *gt = arg;
	unsigned int reset_count;
	intel_wakeref_t wakeref;
	int err = 0;

	/* Check that we can issue a global GPU reset */

	igt_global_reset_lock(gt);
	wakeref = intel_runtime_pm_get(gt->uncore->rpm);

	reset_count = i915_reset_count(&gt->i915->gpu_error);

	intel_gt_reset(gt, ALL_ENGINES, NULL);

	if (i915_reset_count(&gt->i915->gpu_error) == reset_count) {
		pr_err("No GPU reset recorded!\n");
		err = -EINVAL;
	}

	intel_runtime_pm_put(gt->uncore->rpm, wakeref);
	igt_global_reset_unlock(gt);

	if (intel_gt_is_wedged(gt))
		err = -EIO;

	return err;
}

static int igt_wedged_reset(void *arg)
{
	struct intel_gt *gt = arg;
	intel_wakeref_t wakeref;

	/* Check that we can recover a wedged device with a GPU reset */

	igt_global_reset_lock(gt);
	wakeref = intel_runtime_pm_get(gt->uncore->rpm);

	intel_gt_set_wedged(gt);

	GEM_BUG_ON(!intel_gt_is_wedged(gt));
	intel_gt_reset(gt, ALL_ENGINES, NULL);

	intel_runtime_pm_put(gt->uncore->rpm, wakeref);
	igt_global_reset_unlock(gt);

	return intel_gt_is_wedged(gt) ? -EIO : 0;
}

static int igt_atomic_reset(void *arg)
{
	struct intel_gt *gt = arg;
	const typeof(*igt_atomic_phases) *p;
	intel_wakeref_t wakeref;
	int err = 0;

	/* Check that the resets are usable from atomic context */

	wakeref = intel_gt_pm_get(gt);
	igt_global_reset_lock(gt);

	/* Flush any requests before we get started and check basics */
	if (!igt_force_reset(gt))
		goto unlock;

	for (p = igt_atomic_phases; p->name; p++) {
		intel_engine_mask_t awake;

		GEM_TRACE("__intel_gt_reset under %s\n", p->name);

		awake = reset_prepare(gt);
		p->critical_section_begin();

		err = __intel_gt_reset(gt, ALL_ENGINES);

		p->critical_section_end();
		reset_finish(gt, awake);

		if (err) {
			pr_err("__intel_gt_reset failed under %s\n", p->name);
			break;
		}
	}

	/* As we poke around the guts, do a full reset before continuing. */
	igt_force_reset(gt);

unlock:
	igt_global_reset_unlock(gt);
	intel_gt_pm_put(gt, wakeref);

	return err;
}

static int igt_atomic_engine_reset(void *arg)
{
	struct intel_gt *gt = arg;
	const typeof(*igt_atomic_phases) *p;
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	intel_wakeref_t wakeref;
	int err = 0;

	/* Check that the resets are usable from atomic context */

	if (!intel_has_reset_engine(gt))
		return 0;

	if (intel_uc_uses_guc_submission(&gt->uc))
		return 0;

	wakeref = intel_gt_pm_get(gt);
	igt_global_reset_lock(gt);

	/* Flush any requests before we get started and check basics */
	if (!igt_force_reset(gt))
		goto out_unlock;

	for_each_engine(engine, gt, id) {
		struct tasklet_struct *t = &engine->sched_engine->tasklet;

		if (t->func)
			tasklet_disable(t);
		intel_engine_pm_get(engine);

		for (p = igt_atomic_phases; p->name; p++) {
			GEM_TRACE("intel_engine_reset(%s) under %s\n",
				  engine->name, p->name);
			if (strcmp(p->name, "softirq"))
				local_bh_disable();

			p->critical_section_begin();
			err = __intel_engine_reset_bh(engine, NULL);
			p->critical_section_end();

			if (strcmp(p->name, "softirq"))
				local_bh_enable();

			if (err) {
				pr_err("intel_engine_reset(%s) failed under %s\n",
				       engine->name, p->name);
				break;
			}
		}

		intel_engine_pm_put(engine);
		if (t->func) {
			tasklet_enable(t);
			tasklet_hi_schedule(t);
		}
		if (err)
			break;
	}

	/* As we poke around the guts, do a full reset before continuing. */
	igt_force_reset(gt);

out_unlock:
	igt_global_reset_unlock(gt);
	intel_gt_pm_put(gt, wakeref);

	return err;
}

int intel_reset_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_global_reset), /* attempt to recover GPU first */
		SUBTEST(igt_wedged_reset),
		SUBTEST(igt_atomic_reset),
		SUBTEST(igt_atomic_engine_reset),
	};
	struct intel_gt *gt;
	unsigned int i;
	int ret = 0;

	for_each_gt(gt, i915, i) {
		if (!intel_has_gpu_reset(gt))
			continue;
		if (intel_gt_is_wedged(gt))
			ret |= -EIO;
		else
			ret |= intel_gt_live_subtests(tests, gt);
		if (ret)
			break;
	}

	return ret;
}
