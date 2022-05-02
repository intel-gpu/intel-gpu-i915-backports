// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include "intel_memory_region.h"
#include "i915_gem_region.h"
#include "i915_drv.h"
#include "i915_trace.h"
#include "i915_gem_mman.h"

static void
__update_stat(struct i915_mm_swap_stat *stat,
	      unsigned long pages,
	      ktime_t start)
{
	if (stat) {
		start = ktime_get() - start;

		write_seqlock(&stat->lock);
		stat->time = ktime_add(stat->time, start);
		stat->pages += pages;
		write_sequnlock(&stat->lock);
	}
}

static int
i915_gem_object_swapout_pages(struct drm_i915_gem_object *obj,
			      struct sg_table *pages, unsigned int sizes)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct i915_mm_swap_stat *stat = NULL;
	struct drm_i915_gem_object *dst, *src;
	ktime_t start = ktime_get();
	int err = -EINVAL;

	GEM_BUG_ON(obj->swapto);
	GEM_BUG_ON(i915_gem_object_has_pages(obj));
	GEM_BUG_ON(obj->mm.madv != I915_MADV_WILLNEED);
	GEM_BUG_ON(obj->mm.region->type != INTEL_MEMORY_LOCAL);
	GEM_BUG_ON(!i915->params.enable_eviction);

	assert_object_held(obj);

	/* create a shadow object on smem region */
	if (HAS_FLAT_CCS(i915)) {
		dst = i915_gem_object_create_shmem(i915, (obj->base.size +
			(obj->base.size >> 8)));
		if (IS_ERR(dst))
			return PTR_ERR(dst);
	} else {
		dst = i915_gem_object_create_shmem(i915, obj->base.size);
		if (IS_ERR(dst))
			return PTR_ERR(dst);
	}

	/* Share the dma-resv between the shadow- and the parent object */
	dst->base.resv = obj->base.resv;
	assert_object_held(dst);

	/*
	 * create working object on the same region as 'obj',
	 * if 'obj' is used directly, it is set pages and is pinned
	 * again, other thread may wrongly use 'obj' pages.
	 */
	src = i915_gem_object_create_region(obj->mm.region,
					    obj->base.size, 0);
	if (IS_ERR(src)) {
		i915_gem_object_put(dst);
		return PTR_ERR(src);
	}

	/* set and pin working object pages */
	i915_gem_object_lock_isolated(src);
	__i915_gem_object_set_pages(src, pages, sizes);
	__i915_gem_object_pin_pages(src);

	/* copying the pages */
	if (i915->params.enable_eviction >= 2) {
		err = i915_window_blt_copy(dst, src, HAS_FLAT_CCS(i915));
		if (!err)
			stat = &i915->mm.blt_swap_stats.out;
	}

	if (err && err != -ERESTARTSYS && err != -EINTR &&
	    !HAS_FLAT_CCS(i915) &&
	    i915->params.enable_eviction != 2) {
		err = i915_gem_object_memcpy(dst, src);
		if (!err)
			stat = &i915->mm.memcpy_swap_stats.out;
	}

	__i915_gem_object_unpin_pages(src);
	__i915_gem_object_unset_pages(src);
	i915_gem_object_unlock(src);
	i915_gem_object_put(src);

	if (!err) {
		obj->swapto = dst;
	} else {
		if (err != -EINTR && err != -ERESTARTSYS)
			i915_log_driver_error(i915,
				      I915_DRIVER_ERROR_OBJECT_MIGRATION,
				      "Failed to swap-out object (%d)\n", err);
		i915_gem_object_put(dst);
	}

	__update_stat(stat, obj->base.size >> PAGE_SHIFT, start);

	return err;
}

static int
i915_gem_object_swapin_pages(struct drm_i915_gem_object *obj,
			     struct sg_table *pages, unsigned int sizes)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct i915_mm_swap_stat *stat = NULL;
	struct drm_i915_gem_object *dst, *src;
	ktime_t start = ktime_get();
	int err = -EINVAL;

	GEM_BUG_ON(!obj->swapto);
	GEM_BUG_ON(i915_gem_object_has_pages(obj));
	GEM_BUG_ON(obj->mm.madv != I915_MADV_WILLNEED);
	GEM_BUG_ON(obj->mm.region->type != INTEL_MEMORY_LOCAL);
	GEM_BUG_ON(!i915->params.enable_eviction);

	assert_object_held(obj);

	src = obj->swapto;

	/*
	 * create working object on the same region as 'obj',
	 * if 'obj' is used directly, it is set pages and is pinned
	 * again, other thread may wrongly use 'obj' pages.
	 */
	dst = i915_gem_object_create_region(obj->mm.region,
					    obj->base.size, 0);
	if (IS_ERR(dst)) {
		err = PTR_ERR(dst);
		return err;
	}

	/* @scr is sharing @obj's reservation object */
	assert_object_held(src);

	/* set and pin working object pages */
	i915_gem_object_lock_isolated(dst);
	__i915_gem_object_set_pages(dst, pages, sizes);
	__i915_gem_object_pin_pages(dst);

	/* copying the pages */
	if (i915->params.enable_eviction >= 2) {
		err = i915_window_blt_copy(dst, src, HAS_FLAT_CCS(i915));
		if (!err)
			stat = &i915->mm.blt_swap_stats.in;
	}

	if (err && err != -ERESTARTSYS && err != -EINTR &&
	    !HAS_FLAT_CCS(i915) &&
	    i915->params.enable_eviction != 2) {
		err = i915_gem_object_memcpy(dst, src);
		if (!err)
			stat = &i915->mm.memcpy_swap_stats.in;
	}

	__i915_gem_object_unpin_pages(dst);
	__i915_gem_object_unset_pages(dst);
	i915_gem_object_unlock(dst);
	i915_gem_object_put(dst);

	if (!err) {
		obj->swapto = NULL;
		i915_gem_object_put(src);
	} else if (err != -EINTR && err != -ERESTARTSYS) {
		i915_log_driver_error(i915, I915_DRIVER_ERROR_OBJECT_MIGRATION,
				      "Failed to swap-in object (%d)\n", err);
	}

	__update_stat(stat, obj->base.size >> PAGE_SHIFT, start);

	return err;
}

int
i915_gem_object_put_pages_buddy(struct drm_i915_gem_object *obj,
				struct sg_table *pages)
{
	/* if need to save the page contents, swap them out */
	if (obj->do_swapping) {
		unsigned int sizes = obj->mm.page_sizes.phys;
		int err;

		GEM_BUG_ON(obj->mm.madv != I915_MADV_WILLNEED);
		GEM_BUG_ON(i915_gem_object_is_volatile(obj));

		/* if swapout failed, caller takes care of pages */
		err = i915_gem_object_swapout_pages(obj, pages, sizes);
		if (err)
			return err;
	}

	__intel_memory_region_put_pages_buddy(obj->mm.region, &obj->mm.blocks);

	obj->mm.dirty = false;
	sg_free_table(pages);
	kfree(pages);

	return 0;
}

int
i915_gem_object_get_pages_buddy(struct drm_i915_gem_object *obj)
{
	const u64 max_segment = i915_sg_segment_size();
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct intel_memory_region *mem = obj->mm.region;
	struct list_head *blocks = &obj->mm.blocks;
	resource_size_t size = obj->base.size;
	resource_size_t prev_end;
	struct i915_buddy_block *block;
	unsigned int flags;
	struct sg_table *st;
	struct scatterlist *sg;
	unsigned int sg_page_sizes;
	pgoff_t num_pages; /* implicitly limited by sg_alloc_table */
	int ret;
	struct i915_gem_ww_ctx *ww = i915_gem_get_locking_ctx(obj);

	/* XXX: Check if we have any post. This is nasty hack, see gem_create */
	if (obj->mm.gem_create_posted_err)
		return obj->mm.gem_create_posted_err;

	if (!safe_conversion(&num_pages,
			     round_up(obj->base.size, mem->min_page_size) >>
			     ilog2(mem->min_page_size)))
		return -E2BIG;

	st = kmalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	if (sg_alloc_table(st, num_pages, GFP_KERNEL)) {
		kfree(st);
		return -ENOMEM;
	}

	flags = 0;
	if (obj->flags & I915_BO_ALLOC_CHUNK_1G) {
		flags = I915_ALLOC_CHUNK_1G;
	} else if (obj->flags & I915_BO_ALLOC_CHUNK_2M) {
		flags = I915_ALLOC_CHUNK_2M;
	} else if (obj->flags & I915_BO_ALLOC_CHUNK_64K) {
		flags = I915_ALLOC_CHUNK_64K;
	} else if (obj->flags & I915_BO_ALLOC_CHUNK_4K) {
		flags = I915_ALLOC_CHUNK_4K;
	} else if (!(obj->flags & I915_BO_ALLOC_IGNORE_MIN_PAGE_SIZE)) {
		flags = I915_ALLOC_CHUNK_MIN_PAGE_SIZE;
	}
	if (obj->flags & I915_BO_ALLOC_CONTIGUOUS)
		flags |= I915_ALLOC_CONTIGUOUS;

	ret = __intel_memory_region_get_pages_buddy(mem, ww, size, flags,
						    blocks);
	if (ret)
		goto err_free_sg;

	GEM_BUG_ON(list_empty(blocks));

	sg = st->sgl;
	st->nents = 0;
	sg_page_sizes = 0;
	prev_end = (resource_size_t)-1;

	list_for_each_entry(block, blocks, link) {
		u64 block_size, offset;

		block_size = min_t(u64, size,
				   i915_buddy_block_size(&mem->mm, block));
		offset = i915_buddy_block_offset(block);

		while (block_size) {
			u64 len;

			if (offset != prev_end || sg->length >= max_segment) {
				/* Check we have not overflown our prealloc */
				GEM_BUG_ON(st->nents >= num_pages);

				if (st->nents) {
					sg_page_sizes |= sg->length;
					sg = __sg_next(sg);
				}

				sg_dma_address(sg) = offset;
				sg_dma_len(sg) = 0;
				sg->length = 0;
				st->nents++;
			}

			len = min(block_size, max_segment - sg->length);
			sg->length += len;
			sg_dma_len(sg) += len;

			offset += len;
			block_size -= len;

			prev_end = offset;
		}
	}

	sg_page_sizes |= sg->length;
	sg_mark_end(sg);
	i915_sg_trim(st);

	/* if we saved the page contents, swap them in */
	if (obj->swapto) {
		GEM_BUG_ON(i915_gem_object_is_volatile(obj));
		GEM_BUG_ON(!i915->params.enable_eviction);

		ret = i915_gem_object_swapin_pages(obj, st,
						   sg_page_sizes);
		if (ret) {
			/* swapin failed, free the pages */
			__intel_memory_region_put_pages_buddy(mem, blocks);
			if (ret != -EDEADLK && ret != -EINTR &&
			    ret != -ERESTARTSYS)
				ret = -ENXIO;
			goto err_free_sg;
		}
	} else if (obj->flags & I915_BO_ALLOC_CPU_CLEAR) {
		struct scatterlist *sg;
		unsigned long i;

		for_each_sg(st->sgl, sg, st->nents, i) {
			unsigned int length;
			void __iomem *vaddr;
			dma_addr_t daddr;

			daddr = sg_dma_address(sg);
			daddr -= mem->region.start;
			length = sg_dma_len(sg);

			vaddr = io_mapping_map_wc(&mem->iomap, daddr, length);
			memset64((void __force *)vaddr, 0, length / sizeof(u64));
			io_mapping_unmap(vaddr);
		}

		wmb();
	}

	__i915_gem_object_set_pages(obj, st, sg_page_sizes);

	return 0;

err_free_sg:
	sg_free_table(st);
	kfree(st);
	return ret;
}

void i915_gem_object_init_memory_region(struct drm_i915_gem_object *obj,
					struct intel_memory_region *mem)
{
	INIT_LIST_HEAD(&obj->mm.blocks);
	WARN_ON(i915_gem_object_has_pages(obj));
	obj->mm.region = intel_memory_region_get(mem);

	if (obj->base.size <= mem->min_page_size)
		obj->flags |= I915_BO_ALLOC_CONTIGUOUS;
}

void i915_gem_object_release_memory_region(struct drm_i915_gem_object *obj)
{
	intel_memory_region_put(obj->mm.region);
}

struct drm_i915_gem_object *
i915_gem_object_create_region(struct intel_memory_region *mem,
			      resource_size_t size,
			      unsigned int flags)
{
	struct drm_i915_gem_object *obj;
	int err;

	/*
	 * NB: Our use of resource_size_t for the size stems from using struct
	 * resource for the mem->region. We might need to revisit this in the
	 * future.
	 */

	GEM_BUG_ON(flags & ~I915_BO_ALLOC_FLAGS);

	if (!mem)
		return ERR_PTR(-ENODEV);

	if (flags & I915_BO_ALLOC_CHUNK_2M)
		size = round_up(size, SZ_2M);
	else if (flags & I915_BO_ALLOC_CHUNK_64K)
		size = round_up(size, SZ_64K);
	else if (!(flags & I915_BO_ALLOC_IGNORE_MIN_PAGE_SIZE))
		size = round_up(size, mem->min_page_size);

	GEM_BUG_ON(!size);
	GEM_BUG_ON(!IS_ALIGNED(size, I915_GTT_MIN_ALIGNMENT));

	if (i915_gem_object_size_2big(size))
		return ERR_PTR(-E2BIG);

	obj = i915_gem_object_alloc();
	if (!obj)
		return ERR_PTR(-ENOMEM);

	err = mem->ops->init_object(mem, obj, size, flags);
	if (err)
		goto err_object_free;

	trace_i915_gem_object_create(obj);
	return obj;

err_object_free:
	i915_gem_object_free(obj);
	return ERR_PTR(err);
}
