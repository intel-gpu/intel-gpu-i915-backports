/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "gem/i915_gem_internal.h"
#include "gem/i915_gem_pm.h"
#include "gem/selftests/igt_gem_utils.h"
#include "gem/selftests/mock_context.h"
#include "gt/intel_gt.h"

#include "i915_selftest.h"

#include "igt_flush_test.h"
#include "lib_sw_fence.h"
#include "mock_drm.h"

static int igt_evict_contexts(void *arg)
{
	const u64 PRETEND_GGTT_SIZE = 16ull << 20;
	struct intel_gt *gt = arg;
	struct i915_ggtt *ggtt = gt->ggtt;
	struct drm_i915_private *i915 = gt->i915;
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	struct reserved {
		struct drm_mm_node node;
		struct reserved *next;
	} *reserved = NULL;
	intel_wakeref_t wakeref;
	struct drm_mm_node hole;
	unsigned long count;
	int err;

	/*
	 * The purpose of this test is to verify that we will trigger an
	 * eviction in the GGTT when constructing a request that requires
	 * additional space in the GGTT for pinning the context. This space
	 * is not directly tied to the request so reclaiming it requires
	 * extra work.
	 *
	 * As such this test is only meaningful for full-ppgtt environments
	 * where the GTT space of the request is separate from the GGTT
	 * allocation required to build the request.
	 */

	wakeref = intel_runtime_pm_get(&i915->runtime_pm);

	/* Reserve a block so that we know we have enough to fit a few rq */
	memset(&hole, 0, sizeof(hole));
	mutex_lock(&ggtt->vm.mutex);
	err = i915_gem_gtt_insert(&ggtt->vm, &hole,
				  PRETEND_GGTT_SIZE, 0, I915_COLOR_UNEVICTABLE,
				  0, ggtt->vm.total,
				  PIN_NOEVICT);
	if (err)
		goto out_locked;

	/* Make the GGTT appear small by filling it with unevictable nodes */
	count = 0;
	do {
		struct reserved *r;

		mutex_unlock(&ggtt->vm.mutex);
		r = kcalloc(1, sizeof(*r), GFP_KERNEL);
		mutex_lock(&ggtt->vm.mutex);
		if (!r) {
			err = -ENOMEM;
			goto out_locked;
		}

		if (i915_gem_gtt_insert(&ggtt->vm, &r->node,
					1ul << 20, 0, I915_COLOR_UNEVICTABLE,
					0, ggtt->vm.total,
					PIN_NOEVICT)) {
			kfree(r);
			break;
		}

		r->next = reserved;
		reserved = r;

		count++;
	} while (1);
	drm_mm_remove_node(&hole);
	mutex_unlock(&ggtt->vm.mutex);
	pr_info("Filled GGTT with %lu 1MiB nodes\n", count);

	/* Overfill the GGTT with context objects and so try to evict one. */
	for_each_engine(engine, gt, id) {
		struct i915_sw_fence fence;
		struct i915_request *last = NULL;

		count = 0;
		onstack_fence_init(&fence);
		do {
			struct intel_context *ce;
			struct i915_request *rq;

			ce = intel_context_create(engine);
			if (IS_ERR(ce))
				break;

			/* We will need some GGTT space for the rq's context */
			igt_evict_ctl.fail_if_busy = true;
			rq = intel_context_create_request(ce);
			igt_evict_ctl.fail_if_busy = false;
			intel_context_put(ce);

			if (IS_ERR(rq)) {
				/* When full, fail_if_busy will trigger EBUSY */
				if (PTR_ERR(rq) != -EBUSY) {
					pr_err("Unexpected error from request alloc (on %s): %d\n",
					       engine->name,
					       (int)PTR_ERR(rq));
					err = PTR_ERR(rq);
				}
				break;
			}

			/* Keep every request/ctx pinned until we are full */
			err = i915_sw_fence_await_sw_fence_gfp(&rq->submit,
							       &fence,
							       GFP_KERNEL);
			if (err < 0)
				break;

			i915_request_add(rq);
			count++;
			if (last)
				i915_request_put(last);
			last = i915_request_get(rq);
			err = 0;
		} while(1);
		onstack_fence_fini(&fence);
		pr_info("Submitted %lu contexts/requests on %s\n",
			count, engine->name);
		if (err)
			break;
		if (last) {
			if (i915_request_wait(last, 0, HZ) < 0) {
				err = -EIO;
				i915_request_put(last);
				pr_err("Failed waiting for last request (on %s)",
				       engine->name);
				break;
			}
			i915_request_put(last);
		}
		err = intel_gt_wait_for_idle(engine->gt, HZ * 3);
		if (err) {
			pr_err("Failed to idle GT (on %s)", engine->name);
			break;
		}
	}

	mutex_lock(&ggtt->vm.mutex);
out_locked:
	if (igt_flush_test(i915))
		err = -EIO;
	while (reserved) {
		struct reserved *next = reserved->next;

		drm_mm_remove_node(&reserved->node);
		kfree(reserved);

		reserved = next;
	}
	if (drm_mm_node_allocated(&hole))
		drm_mm_remove_node(&hole);
	mutex_unlock(&ggtt->vm.mutex);
	intel_runtime_pm_put(&i915->runtime_pm, wakeref);

	return err;
}

int i915_gem_evict_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_evict_contexts),
	};

	if (intel_gt_is_wedged(to_gt(i915)))
		return 0;

	return intel_gt_live_subtests(tests, to_gt(i915));
}
