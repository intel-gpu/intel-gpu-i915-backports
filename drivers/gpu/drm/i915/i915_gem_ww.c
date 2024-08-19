// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

#include <linux/dma-resv.h>
#include <linux/stackdepot.h>

#include "gem/i915_gem_object.h"

#include "i915_gem_ww.h"
#include "intel_memory_region.h"

#define BUFSZ 4096

void i915_gem_ww_contended(struct i915_gem_ww_ctx *ww, struct drm_i915_gem_object *obj, bool evict)
{
#if IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GEM)
	unsigned long entries[32];
	unsigned int n;

	n = stack_trace_save(entries, ARRAY_SIZE(entries), 1);
	ww->stack = stack_depot_save(entries, n, GFP_NOWAIT | __GFP_NOWARN);
#endif
	ww->contended = i915_gem_object_get(obj);
	ww->evict = evict;
}

void i915_gem_ww_ctx_init(struct i915_gem_ww_ctx *ww, bool intr)
{
	ww_acquire_init(&ww->ctx, &reservation_ww_class);
	INIT_LIST_HEAD(&ww->obj_list);

	ww->intr = intr;
	ww->contended = NULL;
}

static void i915_gem_ww_ctx_unlock_all(struct i915_gem_ww_ctx *ww, bool lru)
{
	struct drm_i915_gem_object *obj, *next;

	list_for_each_entry_safe(obj, next, &ww->obj_list, obj_link) {
		if (lru)
			WRITE_ONCE(obj->mm.region.age, jiffies);
		i915_gem_object_unlock(obj);
		i915_gem_object_put(obj);
	}
	INIT_LIST_HEAD(&ww->obj_list);
}

void i915_gem_ww_unlock_single(struct drm_i915_gem_object *obj)
{
	WRITE_ONCE(obj->mm.region.age, jiffies);
	list_del(&obj->obj_link);
	i915_gem_object_unlock(obj);
	i915_gem_object_put(obj);
}

void i915_gem_ww_ctx_fini(struct i915_gem_ww_ctx *ww)
{
	i915_gem_ww_ctx_unlock_all(ww, true);
#if IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GEM)
	if (unlikely(ww->contended && ww->stack)) {
		char buf[512];

		stack_depot_snprint(ww->stack, buf, sizeof(buf), 0);
		GEM_TRACE_ERR("ww.contended still held, 'locked' at %s\n", buf);
	}
#endif
	GEM_BUG_ON(ww->contended);
	ww_acquire_fini(&ww->ctx);
}

int __must_check i915_gem_ww_ctx_backoff(struct i915_gem_ww_ctx *ww)
{
	struct drm_i915_gem_object *obj = ww->contended;
	int ret = 0;

	if (GEM_WARN_ON(!obj))
		return -EINVAL;

	WRITE_ONCE(obj->mm.region.age, jiffies);
	i915_gem_ww_ctx_unlock_all(ww, false);

	if (ww->intr)
		ret = dma_resv_lock_slow_interruptible(obj->base.resv, &ww->ctx);
	else
		dma_resv_lock_slow(obj->base.resv, &ww->ctx);
	if (ret) {
		i915_gem_object_put(obj);
		goto out;
	}

	/*
	 * Unlocking the contended lock again, if it was locked for eviction.
	 * We will most likely not need it in the retried transaction.
	 */
	if (ww->evict) {
		dma_resv_unlock(obj->base.resv);
		i915_gem_object_put(obj);
	} else {
		list_add_tail(&obj->obj_link, &ww->obj_list);
	}

out:
	ww->contended = NULL;
	return ret;
}

int
__i915_gem_object_lock_to_evict(struct drm_i915_gem_object *obj,
				struct i915_gem_ww_ctx *ww)
{
	int err;

	if (ww)
		err = dma_resv_lock_interruptible(obj->base.resv, &ww->ctx);
	else
		err = dma_resv_trylock(obj->base.resv) ? 0 : -EBUSY;
	if (unlikely(err == -EDEADLK))
		i915_gem_ww_contended(ww, obj, true);

	return err;
}
