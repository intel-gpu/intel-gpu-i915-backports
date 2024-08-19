/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2014-2016 Intel Corporation
 */

#include "i915_drv.h"
#include "i915_gem_internal.h"
#include "i915_gem_object.h"

static int nop_get_pages(struct drm_i915_gem_object *obj)
{
	return -EACCES;
}

static int nop_put_pages(struct drm_i915_gem_object *obj, struct sg_table *pages)
{
	return 0;
}

static const struct drm_i915_gem_object_ops private_ops = {
	.name = "i915_gem_object_private",
	.get_pages = nop_get_pages,
	.put_pages = nop_put_pages,
};

struct drm_i915_gem_object *
i915_gem_object_create_private(struct drm_i915_private *i915)
{
	struct drm_i915_gem_object *obj;

	obj = i915_gem_object_alloc();
	if (!obj)
		return ERR_PTR(-ENOMEM);

	drm_gem_private_object_init(&i915->drm, &obj->base, 0);
	i915_gem_object_init(obj, &private_ops, 0);

	return obj;
}

/**
 * i915_gem_object_create_internal: create an object with volatile pages
 * @i915: the i915 device
 * @size: the size in bytes of backing storage to allocate for the object
 *
 * Creates a new object that wraps some internal memory for private use.
 * This object is not backed by swappable storage, and as such its contents
 * are volatile and only valid whilst pinned. If the object is reaped by the
 * shrinker, its pages and data will be discarded. Equally, it is not a full
 * GEM object and so not valid for access from userspace. This makes it useful
 * for hardware interfaces like ringbuffers (which are pinned from the time
 * the request is written to the time the hardware stops accessing it), but
 * not for contexts (which need to be preserved when not active for later
 * reuse). Note that it is not cleared upon allocation.
 */
struct drm_i915_gem_object *
i915_gem_object_create_internal(struct drm_i915_private *i915,
				phys_addr_t size)
{
	struct drm_i915_gem_object *obj;

	GEM_BUG_ON(!size);
	GEM_BUG_ON(!IS_ALIGNED(size, PAGE_SIZE));

	obj = i915_gem_object_create_shmem(i915, size);
	if (IS_ERR(obj))
		return obj;

	/*
	 * Mark the object as volatile, such that the pages are marked as
	 * dontneed whilst they are still pinned. As soon as they are unpinned
	 * they are allowed to be reaped by the shrinker, and the caller is
	 * expected to repopulate - the contents of this object are only valid
	 * whilst active and pinned.
	 */
	i915_gem_object_set_volatile(obj);
	return obj;
}
