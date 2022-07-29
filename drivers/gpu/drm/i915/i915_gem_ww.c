// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */
#include <linux/dma-resv.h>
#include "i915_gem_ww.h"
#include "gem/i915_gem_object.h"

void i915_gem_ww_ctx_init(struct i915_gem_ww_ctx *ww, bool intr)
{
	ww_acquire_init(&ww->ctx, &reservation_ww_class);
	INIT_LIST_HEAD(&ww->obj_list);
	INIT_LIST_HEAD(&ww->eviction_list);
	ww->intr = intr;
	ww->contended = NULL;
	ww->contended_evict = false;
}

void i915_gem_ww_ctx_unlock_evictions(struct i915_gem_ww_ctx *ww)
{
	struct drm_i915_gem_object *obj, *next;

	list_for_each_entry_safe(obj, next, &ww->eviction_list, obj_link) {
		list_del(&obj->obj_link);
		GEM_WARN_ON(!obj->evict_locked);
		i915_gem_object_unlock(obj);
		i915_gem_object_put(obj);
	}
}

static void i915_gem_ww_ctx_unlock_all(struct i915_gem_ww_ctx *ww)
{
	struct drm_i915_gem_object *obj, *next;

	list_for_each_entry_safe(obj, next, &ww->obj_list, obj_link) {
		list_del(&obj->obj_link);
		GEM_WARN_ON(obj->evict_locked);
		i915_gem_object_unlock(obj);
		i915_gem_object_put(obj);
	}

	i915_gem_ww_ctx_unlock_evictions(ww);
}

void i915_gem_ww_unlock_single(struct drm_i915_gem_object *obj)
{
	list_del(&obj->obj_link);
	i915_gem_object_unlock(obj);
	i915_gem_object_put(obj);
}

void i915_gem_ww_ctx_fini(struct i915_gem_ww_ctx *ww)
{
	i915_gem_ww_ctx_unlock_all(ww);
	WARN_ON(ww->contended);
	ww_acquire_fini(&ww->ctx);
}

int __must_check i915_gem_ww_ctx_backoff(struct i915_gem_ww_ctx *ww)
{
	struct drm_i915_gem_object *obj = ww->contended;
	int ret = 0;

	if (WARN_ON(!obj))
		return -EINVAL;

	i915_gem_ww_ctx_unlock_all(ww);
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
	if (ww->contended_evict) {
		dma_resv_unlock(obj->base.resv);
		i915_gem_object_put(obj);
	} else {
		obj->evict_locked = false;
		list_add_tail(&obj->obj_link, &ww->obj_list);
	}

out:
	ww->contended = NULL;

	return ret;
}
