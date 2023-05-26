// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

#include <linux/dma-resv.h>

#include "gem/i915_gem_object.h"

#include "i915_gem_ww.h"
#include "intel_memory_region.h"

void i915_gem_ww_ctx_init(struct i915_gem_ww_ctx *ww, bool intr)
{
	ww_acquire_init(&ww->ctx, &reservation_ww_class);
	INIT_LIST_HEAD(&ww->obj_list);
	INIT_LIST_HEAD(&ww->eviction_list);

	ww->region.mem = NULL;
	ww->region.next = NULL;

	ww->intr = intr;
	ww->contended = NULL;
	ww->contended_evict = false;
}

static void i915_gem_ww_ctx_remove_regions(struct i915_gem_ww_ctx *ww)
{
	struct i915_gem_ww_region *r = &ww->region;

	if (!r->mem)
		return;

	do {
		struct i915_gem_ww_region *next = r->next;
		struct intel_memory_region *mr = r->mem;
		struct drm_i915_gem_object *obj, *on;

		spin_lock(&mr->objects.lock);
		list_del(&r->link);
		list_for_each_entry_safe(obj, on, &r->locked, mm.region.link) {
			struct list_head *list;

			if (obj->mm.madv == I915_MADV_WILLNEED)
				list = &mr->objects.list;
			else
				list = &mr->objects.purgeable;

			list_add_tail(&obj->mm.region.link, list);
		}
		spin_unlock(&mr->objects.lock);
		if (r != &ww->region)
			kfree(r);

		r = next;
	} while (r);

	ww->region.mem = NULL;
	ww->region.next = NULL;
}

static void put_obj_list(struct list_head *list)
{
	struct drm_i915_gem_object *obj, *next;

	list_for_each_entry_safe(obj, next, list, obj_link) {
		i915_gem_object_unlock(obj);
		i915_gem_object_put(obj);
	}
	INIT_LIST_HEAD(list);
}

void i915_gem_ww_ctx_unlock_evictions(struct i915_gem_ww_ctx *ww)
{
	put_obj_list(&ww->eviction_list);
}

static void i915_gem_ww_ctx_unlock_all(struct i915_gem_ww_ctx *ww)
{
	i915_gem_ww_ctx_remove_regions(ww);

	put_obj_list(&ww->obj_list);

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
	GEM_BUG_ON(ww->contended);
	ww_acquire_fini(&ww->ctx);
}

int __must_check i915_gem_ww_ctx_backoff(struct i915_gem_ww_ctx *ww)
{
	struct drm_i915_gem_object *obj = ww->contended;
	int ret = 0;

	if (GEM_WARN_ON(!obj))
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
