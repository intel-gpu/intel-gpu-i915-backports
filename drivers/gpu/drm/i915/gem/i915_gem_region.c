// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include "i915_drv.h"
#include "i915_drm_client.h"
#include "i915_gem_mman.h"
#include "i915_gem_region.h"
#include "i915_trace.h"
#include "intel_memory_region.h"

#define FORCE_CHUNKS (I915_ALLOC_CHUNK_1G | I915_ALLOC_CHUNK_2M | I915_ALLOC_CHUNK_64K | I915_ALLOC_CHUNK_4K)

int
i915_gem_object_put_pages_buddy(struct drm_i915_gem_object *obj,
				struct scatterlist *pages,
				bool dirty)
{
	struct intel_memory_region *mem = obj->mm.region.mem;

	__intel_memory_region_put_pages_buddy(mem, &obj->mm.blocks, dirty);
	i915_drm_client_make_resident(obj, false);

	sg_table_inline_free(pages);
	return 0;
}

struct scatterlist *
i915_gem_object_get_pages_buddy(struct drm_i915_gem_object *obj)
{
	const u64 max_segment = rounddown_pow_of_two(UINT_MAX);
	struct intel_memory_region *mem = obj->mm.region.mem;
	struct list_head *blocks = &obj->mm.blocks;
	resource_size_t size = obj->base.size;
	resource_size_t prev_end;
	struct i915_buddy_block *block;
	struct scatterlist *sg, *chain;
	struct scatterlist *sgt;
	unsigned int flags;
	pgoff_t num_pages; /* implicitly limited by sg_alloc_table */
	int ret;

	if (!safe_conversion(&num_pages, /* worst case number of sg required */
			     round_up(size, mem->min_page_size) >>
			     ilog2(mem->min_page_size)))
		 return ERR_PTR(-E2BIG);

	if (size > mem->total)
		return ERR_PTR(-E2BIG);

	sgt = __sg_table_inline_create(I915_GFP_ALLOW_FAIL);
	if (unlikely(!sgt))
		return ERR_PTR(-ENOMEM);

	sg_init_inline(sgt);

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
	if (obj->flags & I915_BO_ALLOC_USER)
		flags |= I915_BUDDY_ALLOC_ALLOW_ACTIVE;
	if (obj->flags & (I915_BO_ALLOC_USER | I915_BO_CPU_CLEAR))
		flags |= I915_BUDDY_ALLOC_WANT_CLEAR;
	if (obj->swapto) {
		flags &= ~I915_BUDDY_ALLOC_WANT_CLEAR;
		flags |= I915_BUDDY_ALLOC_ALLOW_ACTIVE;
	}
	if (obj->flags & I915_BO_SYNC_HINT)
		flags &= ~I915_BUDDY_ALLOC_ALLOW_ACTIVE;

	ret = __intel_memory_region_get_pages_buddy(mem,
						    i915_gem_get_locking_ctx(obj),
						    size, obj->mm.region.age, flags,
						    blocks);
	if (ret)
		goto err_free_sg;

	GEM_BUG_ON(list_empty(blocks));

	sg = sgt;
	chain = sg + SG_NUM_INLINE - 1;
	prev_end = (resource_size_t)-1;
	sg->length = 0;

	list_for_each_entry(block, blocks, link) {
		u64 block_size, offset;

		block_size = min_t(u64, size,
				   i915_buddy_block_size(&mem->mm, block));
		offset = i915_buddy_block_offset(block);

		while (block_size) {
			u64 len;

			if (flags & FORCE_CHUNKS || offset != prev_end || sg->length >= max_segment) {
				if (sg->length) {
					sg_dma_len(sg) = sg->length;
					sg_page_sizes(sgt) |= sg->length;

					if (sg == chain) {
						unsigned int x;

						x = min_t(unsigned int,
							  num_pages - sg_capacity(sgt) + 1,
							  SG_MAX_SINGLE_ALLOC);
						chain = sg_pool_alloc(x, I915_GFP_ALLOW_FAIL);
						if (unlikely(!chain)) {
							ret = -ENOMEM;
							goto err_free_sg;
						}

						__sg_chain(sg, memcpy(chain, sg, sizeof(*sg)));
						GEM_BUG_ON(sg_chain_ptr(sg) != chain);

						sg = chain;
						chain += x - 1;
						sg_capacity(sgt) += x - 1;
					}
					GEM_BUG_ON(sg_is_last(sg));
					sg++;
				}

				sg->page_link = 0;
				sg->offset = 0;
				sg->length = 0;
				sg_dma_address(sg) = offset;
				sg_count(sgt)++;
			}

			len = min(block_size, max_segment - sg->length);
			sg->length += len;

			offset += len;
			block_size -= len;

			prev_end = offset;
		}
	}

	sg_dma_len(sg) = sg->length;
	sg_page_sizes(sgt) |= sg->length;
	sg_mark_end(sg);

	i915_drm_client_make_resident(obj, true);
	return sgt;

err_free_sg:
	sg_table_inline_free(sgt);
	return ERR_PTR(ret);
}
 
void i915_gem_object_init_memory_region(struct drm_i915_gem_object *obj,
					struct intel_memory_region *mem)
{
	GEM_BUG_ON(i915_gem_object_has_pages(obj));

	obj->mm.region.mem = intel_memory_region_get(mem);
	INIT_LIST_HEAD(&obj->mm.blocks);

	if (obj->base.size <= mem->min_page_size)
		obj->flags |= I915_BO_ALLOC_CONTIGUOUS;
}

void i915_gem_object_release_memory_region(struct drm_i915_gem_object *obj)
{
	struct intel_memory_region *mem;

	mem = fetch_and_zero(&obj->mm.region.mem);
	if (!mem)
		return;

	/* Added to the region list before get_pages failed? */
	if (!list_empty(&obj->mm.region.link)) {
		spin_lock_irq(&mem->objects.lock);
		list_del_init(&obj->mm.region.link);
		spin_unlock_irq(&mem->objects.lock);
	}

	intel_memory_region_put(mem);
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

	if (i915_gem_object_size_2big(size) || size > mem->total)
		return ERR_PTR(-E2BIG);

	obj = i915_gem_object_alloc();
	if (!obj)
		return ERR_PTR(-ENOMEM);

	err = mem->ops->init_object(mem, obj, size, flags);
	if (err)
		goto err_object_free;

	trace_i915_gem_object_create(obj, 0);
	return obj;

err_object_free:
	i915_gem_object_free(obj);
	return ERR_PTR(err);
}
