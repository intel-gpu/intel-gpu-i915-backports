/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2018 Intel Corporation
 */

#include <linux/random.h>
#include <linux/suspend.h>

#include "gem/i915_gem_internal.h"
#include "gem/i915_gem_pm.h"
#include "gem/selftests/igt_gem_utils.h"
#include "gem/selftests/mock_context.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_pm.h"

#include "i915_selftest.h"

#include "igt_flush_test.h"
#include "mock_drm.h"

static int switch_to_context(struct i915_gem_context *ctx)
{
	struct i915_gem_engines_iter it;
	struct intel_context *ce;
	int err = 0;

	for_each_gem_engine(ce, i915_gem_context_lock_engines(ctx), it) {
		struct i915_request *rq;

		rq = intel_context_create_request(ce);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			break;
		}

		i915_request_add(rq);
	}
	i915_gem_context_unlock_engines(ctx);

	return err;
}

static void trash_stolen(struct drm_i915_private *i915)
{
	struct i915_ggtt *ggtt = to_gt(i915)->ggtt;
	const u64 slot = ggtt->error_capture.start;
	const resource_size_t size = resource_size(&i915->dsm);
	unsigned long page;
	u32 prng = 0x12345678;

	/* XXX: fsck. needs some more thought... */
	if (!i915_ggtt_has_aperture(ggtt))
		return;

	for (page = 0; page < size; page += PAGE_SIZE) {
		const dma_addr_t dma = i915->dsm.start + page;
		u32 __iomem *s;
		int x;

		ggtt->vm.insert_page(&ggtt->vm, dma, slot, I915_CACHE_NONE, 0);

		s = io_mapping_map_atomic_wc(&ggtt->iomap, slot);
		for (x = 0; x < PAGE_SIZE / sizeof(u32); x++) {
			prng = next_pseudo_random32(prng);
			iowrite32(prng, &s[x]);
		}
		io_mapping_unmap_atomic(s);
	}

	ggtt->vm.clear_range(&ggtt->vm, slot, PAGE_SIZE);
}

static void simulate_hibernate(struct drm_i915_private *i915)
{
	intel_wakeref_t wakeref;

	wakeref = intel_runtime_pm_get(&i915->runtime_pm);

	/*
	 * As a final sting in the tail, invalidate stolen. Under a real S4,
	 * stolen is lost and needs to be refilled on resume. However, under
	 * CI we merely do S4-device testing (as full S4 is too unreliable
	 * for automated testing across a cluster), so to simulate the effect
	 * of stolen being trashed across S4, we trash it ourselves.
	 */
	trash_stolen(i915);

	intel_runtime_pm_put(&i915->runtime_pm, wakeref);
}

static int do_prepare(struct drm_i915_private *i915)
{
	i915_gem_suspend(i915);

	return 0;
}

static suspend_state_t set_pm_target(suspend_state_t target)
{
#ifdef CONFIG_PM_SLEEP
	return xchg(&pm_suspend_target_state, target);
#else
	return PM_SUSPEND_ON;
#endif
}

static suspend_state_t do_suspend(struct drm_i915_private *i915)
{
	suspend_state_t old = set_pm_target(PM_SUSPEND_MEM);
	intel_wakeref_t wakeref;

	with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
		i915_gem_suspend_late(i915);
	}

	return old;
}

static suspend_state_t do_hibernate(struct drm_i915_private *i915)
{
	suspend_state_t old = set_pm_target(PM_SUSPEND_MAX);
	intel_wakeref_t wakeref;

	with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
		i915_gem_freeze(i915);
		i915_gem_freeze_late(i915);
	}

	return old;
}

static void do_resume(struct drm_i915_private *i915, suspend_state_t saved)
{
	intel_wakeref_t wakeref;

	/*
	 * Both suspend and hibernate follow the same wakeup path and assume
	 * that runtime-pm just works.
	 */
	with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
		i915_gem_resume_early(i915);
		i915_gem_resume(i915);
	}

	set_pm_target(saved);
}

static int igt_gem_suspend(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct i915_gem_context *ctx;
	suspend_state_t saved;
	struct file *file;
	int err;

	file = mock_file(i915);
	if (IS_ERR(file))
		return PTR_ERR(file);

	err = -ENOMEM;
	ctx = live_context(i915, file);
	if (!IS_ERR(ctx))
		err = switch_to_context(ctx);
	if (err)
		goto out;

	err = do_prepare(i915);
	if (err)
		goto out;

	saved = do_suspend(i915);

	/* Here be dragons! Note that with S3RST any S3 may become S4! */
	simulate_hibernate(i915);

	do_resume(i915, saved);

	err = switch_to_context(ctx);
out:
	fput(file);
	return err;
}

static int igt_gem_hibernate(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct i915_gem_context *ctx;
	suspend_state_t saved;
	struct file *file;
	int err;

	file = mock_file(i915);
	if (IS_ERR(file))
		return PTR_ERR(file);

	err = -ENOMEM;
	ctx = live_context(i915, file);
	if (!IS_ERR(ctx))
		err = switch_to_context(ctx);
	if (err)
		goto out;

	err = do_prepare(i915);
	if (err)
		goto out;

	saved = do_hibernate(i915);

	/* Here be dragons! */
	simulate_hibernate(i915);

	do_resume(i915, saved);

	err = switch_to_context(ctx);
out:
	fput(file);
	return err;
}

static int igt_gem_ww_ctx(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct drm_i915_gem_object *obj, *obj2;
	struct i915_gem_ww_ctx ww;
	int err = 0;

	obj = i915_gem_object_create_internal(i915, PAGE_SIZE);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	obj2 = i915_gem_object_create_internal(i915, PAGE_SIZE);
	if (IS_ERR(obj2)) {
		err = PTR_ERR(obj2);
		goto put1;
	}

	i915_gem_ww_ctx_init(&ww, true);
retry:
	/* Lock the objects, twice for good measure (-EALREADY handling) */
	err = i915_gem_object_lock(obj, &ww);
	if (!err)
		err = i915_gem_object_lock_interruptible(obj, &ww);
	if (!err)
		err = i915_gem_object_lock_interruptible(obj2, &ww);
	if (!err)
		err = i915_gem_object_lock(obj2, &ww);

	if (err == -EDEADLK) {
		err = i915_gem_ww_ctx_backoff(&ww);
		if (!err)
			goto retry;
	}
	i915_gem_ww_ctx_fini(&ww);
	i915_gem_object_put(obj2);
put1:
	i915_gem_object_put(obj);
	return err;
}

int i915_gem_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_gem_suspend),
		SUBTEST(igt_gem_hibernate),
	};

	if (intel_gt_is_wedged(to_gt(i915)))
		return 0;

	return i915_live_subtests(tests, i915);
}

int i915_gem_obj_lock_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_gem_ww_ctx),
	};

	if (intel_gt_is_wedged(to_gt(i915)))
		return 0;

	return i915_live_subtests(tests, i915);
}
