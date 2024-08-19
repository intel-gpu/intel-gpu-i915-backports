/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2014-2016 Intel Corporation
 */

#include "display/intel_frontbuffer.h"
#include "gt/intel_gt.h"

#include "i915_drv.h"
#include "i915_gem_domain.h"
#include "i915_gem_gtt.h"
#include "i915_gem_ioctls.h"
#include "i915_gem_lmem.h"
#include "i915_gem_mman.h"
#include "i915_gem_object.h"
#include "i915_vma.h"

#define VTD_GUARD (168u * I915_GTT_PAGE_SIZE) /* 168 or tile-row PTE padding */

static int set_to_domain(struct drm_i915_gem_object *obj, bool write)
{
	return i915_gem_object_wait(obj,
				    I915_WAIT_INTERRUPTIBLE |
				    (write ? I915_WAIT_ALL : 0),
				    MAX_SCHEDULE_TIMEOUT);
}

int
i915_gem_object_set_to_wc_domain(struct drm_i915_gem_object *obj, bool write)
{
	return set_to_domain(obj, write);
}

/*
 * Changes the cache-level of an object across all VMA.
 * @obj: object to act on
 * @cache_level: new cache level to set for the object
 *
 * After this function returns, the object will be in the new cache-level
 * across all GTT and the contents of the backing storage will be coherent,
 * with respect to the new cache-level. In order to keep the backing storage
 * coherent for all users, we only allow a single cache level to be set
 * globally on the object and prevent it from being changed whilst the
 * hardware is reading from the object. That is if the object is currently
 * on the scanout it will be set to uncached (or equivalent display
 * cache coherency) and all non-MOCS GPU access will also be uncached so
 * that all direct access to the scanout remains coherent.
 */
int i915_gem_object_set_cache_level(struct drm_i915_gem_object *obj,
				    enum i915_cache_level cache_level)
{
	int ret;

	if (i915_gem_object_has_segments(obj))
		return -ENXIO;

	if (i915_gem_object_has_cache_level(obj, cache_level))
		return 0;

	ret = i915_gem_object_wait(obj,
				   I915_WAIT_INTERRUPTIBLE |
				   I915_WAIT_ALL,
				   MAX_SCHEDULE_TIMEOUT);
	if (ret)
		return ret;

	/* Always invalidate stale cachelines */
	i915_gem_object_set_cache_coherency(obj, cache_level);

	/* The cache-level will be applied when each vma is rebound. */
	return i915_gem_object_unbind(obj, NULL,
				      I915_GEM_OBJECT_UNBIND_ACTIVE |
				      I915_GEM_OBJECT_UNBIND_BARRIER);
}

int i915_gem_get_caching_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file)
{
	struct drm_i915_gem_caching *args = data;
	struct drm_i915_gem_object *obj;
	int err = 0;

	rcu_read_lock();
	obj = i915_gem_object_lookup_rcu(file, args->handle);
	if (!obj) {
		err = -ENOENT;
		goto out;
	}

	if (i915_gem_object_has_cache_level(obj, I915_CACHE_LLC) ||
	    i915_gem_object_has_cache_level(obj, I915_CACHE_L3_LLC))
		args->caching = I915_CACHING_CACHED;
	else if (i915_gem_object_has_cache_level(obj, I915_CACHE_WT))
		args->caching = I915_CACHING_DISPLAY;
	else
		args->caching = I915_CACHING_NONE;
out:
	rcu_read_unlock();
	return err;
}

int i915_gem_set_caching_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file)
{
	struct drm_i915_private *i915 = to_i915(dev);
	struct drm_i915_gem_caching *args = data;
	struct drm_i915_gem_object *obj;
	enum i915_cache_level level;
	int ret = 0;

	if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 70))
		return -EOPNOTSUPP;

	switch (args->caching) {
	case I915_CACHING_NONE:
		level = I915_CACHE_NONE;
		break;
	case I915_CACHING_CACHED:
		/*
		 * Due to a HW issue on BXT A stepping, GPU stores via a
		 * snooped mapping may leave stale data in a corresponding CPU
		 * cacheline, whereas normally such cachelines would get
		 * invalidated.
		 */
		if (!HAS_LLC(i915) && !HAS_SNOOP(i915))
			return -ENODEV;

		level = I915_CACHE_LLC;
		break;
	case I915_CACHING_DISPLAY:
		level = HAS_WT(i915) ? I915_CACHE_WT : I915_CACHE_NONE;
		break;
	default:
		return -EINVAL;
	}

	obj = i915_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	/*
	 * The caching mode of proxy object is handled by its generator, and
	 * not allowed to be changed by userspace.
	 */
	if (i915_gem_object_is_proxy(obj)) {
		ret = -ENXIO;
		goto out;
	}

	if (i915_gem_object_is_lmem(obj) && level != I915_CACHE_NONE) {
		ret = -EINVAL;
		goto out;
	}

	ret = i915_gem_object_lock_interruptible(obj, NULL);
	if (ret)
		goto out;

	ret = i915_gem_object_set_cache_level(obj, level);
	i915_gem_object_unlock(obj);

out:
	i915_gem_object_put(obj);
	return ret;
}

/*
 * Prepare buffer for display plane (scanout, cursors, etc). Can be called from
 * an uninterruptible phase (modesetting) and allows any flushes to be pipelined
 * (for pageflips). We only flush the caches while preparing the buffer for
 * display, the callers are responsible for frontbuffer flush.
 */
struct i915_vma *
i915_gem_object_pin_to_display_plane(struct drm_i915_gem_object *obj,
				     struct i915_gem_ww_ctx *ww,
				     struct i915_ggtt *ggtt,
				     const struct i915_ggtt_view *view,
				     u32 alignment,
				     unsigned int flags)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct i915_vma *vma;
	int ret;

	/* Frame buffer must be in LMEM (no migration yet) */
	if (HAS_LMEM(i915) && !i915_gem_object_is_lmem(obj))
		return ERR_PTR(-EINVAL);

	/*
	 * The display engine is not coherent with the LLC cache on gen6.  As
	 * a result, we make sure that the pinning that is about to occur is
	 * done with uncached PTEs. This is lowest common denominator for all
	 * chipsets.
	 *
	 * However for gen6+, we could do better by using the GFDT bit instead
	 * of uncaching, which would allow us to flush all the LLC-cached data
	 * with that bit in the PTE to main memory with just one PIPE_CONTROL.
	 */
	ret = i915_gem_object_set_cache_level(obj,
					      HAS_WT(i915) ?
					      I915_CACHE_WT : I915_CACHE_NONE);
	if (ret)
		return ERR_PTR(ret);

	/* VT-d may overfetch before/after the vma, so pad with scratch */
	if (intel_scanout_needs_vtd_wa(i915))
		flags |= PIN_OFFSET_GUARD | VTD_GUARD;

	/*
	 * As the user may map the buffer once pinned in the display plane
	 * (e.g. libkms for the bootup splash), we have to ensure that we
	 * always use map_and_fenceable for all scanout buffers. However,
	 * it may simply be too big to fit into mappable, in which case
	 * put it anyway and hope that userspace can cope (but always first
	 * try to preserve the existing ABI).
	 */
	vma = ERR_PTR(-ENOSPC);
	if (!view || view->type == I915_GGTT_VIEW_NORMAL)
		vma = i915_gem_object_ggtt_pin_ww(obj, ww,
						  ggtt, view,
						  0, alignment,
						  flags | PIN_NONBLOCK);
	if (IS_ERR(vma) && vma != ERR_PTR(-EDEADLK))
		vma = i915_gem_object_ggtt_pin_ww(obj, ww,
						  ggtt, view,
						  0, alignment,
						  flags);
	if (IS_ERR(vma))
		return vma;

	vma->display_alignment = max_t(u64, vma->display_alignment, alignment);
	i915_vma_mark_scanout(vma);

	return vma;
}

int
i915_gem_object_set_to_cpu_domain(struct drm_i915_gem_object *obj, bool write)
{
	return set_to_domain(obj, write);
}

/*
 * Called when user space prepares to use an object with the CPU, either
 * through the mmap ioctl's mapping or a GTT mapping.
 * @dev: drm device
 * @data: ioctl data blob
 * @file: drm file
 */
int
i915_gem_set_domain_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file)
{
	struct drm_i915_gem_set_domain *args = data;
	struct drm_i915_gem_object *obj;
	u32 read_domains = args->read_domains;
	u32 write_domain = args->write_domain;
	struct i915_gem_ww_ctx ww;
	int err;

	/* Only handle setting domains to types used by the CPU. */
	if ((write_domain | read_domains) & I915_GEM_GPU_DOMAINS)
		return -EINVAL;

	/*
	 * Having something in the write domain implies it's in the read
	 * domain, and only that read domain.  Enforce that in the request.
	 */
	if (write_domain && read_domains != write_domain)
		return -EINVAL;

	if (!read_domains)
		return 0;

	obj = i915_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	if (i915_gem_object_has_segments(obj)) {
		err = -ENXIO;
		goto out;
	}

	/*
	 * Try to flush the object off the GPU without holding the lock.
	 * We will repeat the flush holding the lock in the normal manner
	 * to catch cases where we are gazumped.
	 */
	err = i915_gem_object_wait(obj,
				   I915_WAIT_INTERRUPTIBLE |
				   I915_WAIT_PRIORITY |
				   (write_domain ? I915_WAIT_ALL : 0),
				   MAX_SCHEDULE_TIMEOUT);
	if (err)
		goto out;

	/*
	 * Proxy objects do not control access to the backing storage, ergo
	 * they cannot be used as a means to manipulate the cache domain
	 * tracking for that backing storage. The proxy object is always
	 * considered to be outside of any cache domain.
	 */
	if (i915_gem_object_is_proxy(obj)) {
		err = -ENXIO;
		goto out;
	}

	for_i915_gem_ww(&ww, err, true) {
		err = i915_gem_object_lock(obj, &ww);
		if (err)
			continue;

		/*
		 * Flush and acquire obj->pages so that we are coherent through
		 * direct access in memory with previous cached writes through
		 * shmemfs and that our cache domain tracking remains valid.
		 * For example, if the obj->filp was moved to swap without us
		 * being notified and releasing the pages, we would mistakenly
		 * continue to assume that the obj remained out of the CPU
		 * cached domain.
		 */
		err = i915_gem_object_pin_pages_sync(obj);
		if (err)
			continue;

		i915_gem_object_unpin_pages(obj);
	}

	if (!err && write_domain)
		i915_gem_object_invalidate_frontbuffer(obj, ORIGIN_CPU);

out:
	i915_gem_object_put(obj);
	return err;
}

/*
 * Pins the specified object's pages and synchronizes the object with
 * GPU accesses. Sets needs_clflush to non-zero if the caller should
 * flush the object from the CPU cache.
 */
int i915_gem_object_prepare_read(struct drm_i915_gem_object *obj,
				 unsigned int *needs_clflush)
{
	int ret;

	*needs_clflush = 0;
	if (!i915_gem_object_has_struct_page(obj))
		return -ENODEV;

	assert_object_held(obj);

	ret = i915_gem_object_wait(obj,
				   I915_WAIT_INTERRUPTIBLE,
				   MAX_SCHEDULE_TIMEOUT);
	if (ret)
		return ret;

	return i915_gem_object_pin_pages_sync(obj);
}

int i915_gem_object_prepare_write(struct drm_i915_gem_object *obj,
				  unsigned int *needs_clflush)
{
	int ret;

	*needs_clflush = 0;
	if (!i915_gem_object_has_struct_page(obj))
		return -ENODEV;

	assert_object_held(obj);

	ret = i915_gem_object_wait(obj,
				   I915_WAIT_INTERRUPTIBLE |
				   I915_WAIT_ALL,
				   MAX_SCHEDULE_TIMEOUT);
	if (ret)
		return ret;

	return i915_gem_object_pin_pages_sync(obj);
}
