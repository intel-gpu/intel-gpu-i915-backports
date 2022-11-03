// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <linux/kmemleak.h>

#include "i915_buddy.h"

#include "i915_gem.h"
#include "i915_utils.h"

static struct kmem_cache *slab_blocks;

static struct i915_buddy_block *i915_block_alloc(struct i915_buddy_mm *mm,
						 struct i915_buddy_block *parent,
						 unsigned int order,
						 u64 offset)
{
	struct i915_buddy_block *block;

	GEM_BUG_ON(order > I915_BUDDY_MAX_ORDER);

	block = kmem_cache_zalloc(slab_blocks, GFP_KERNEL);
	if (!block)
		return NULL;

	block->header = offset;
	block->header |= order;
	block->parent = parent;

	GEM_BUG_ON(block->header & I915_BUDDY_HEADER_UNUSED);
	return block;
}

static void i915_block_free(struct i915_buddy_mm *mm,
			    struct i915_buddy_block *block)
{
	kmem_cache_free(slab_blocks, block);
}

static void mark_allocated(struct i915_buddy_block *block)
{
	block->header &= ~I915_BUDDY_HEADER_STATE;
	block->header |= I915_BUDDY_ALLOCATED;

	list_del(&block->link);
}

static void mark_free(struct i915_buddy_mm *mm,
		      struct i915_buddy_block *block)
{
	block->header &= ~I915_BUDDY_HEADER_STATE;
	block->header |= I915_BUDDY_FREE;

	list_add(&block->link,
		 &mm->free_list[i915_buddy_block_order(block)]);
}

static void mark_split(struct i915_buddy_block *block)
{
	block->header &= ~I915_BUDDY_HEADER_STATE;
	block->header |= I915_BUDDY_SPLIT;

	list_del(&block->link);
}

int i915_buddy_init(struct i915_buddy_mm *mm, u64 start, u64 end, u64 chunk)
{
	struct i915_buddy_block **roots;
	unsigned int i, max_order;
	u64 offset, size;
 
	if (GEM_WARN_ON(range_overflows(start, chunk, end)))
		return -EINVAL;

	if (chunk < PAGE_SIZE || !is_power_of_2(chunk))
		return -EINVAL;

	/*
	 * We want the addresses we return to be naturally aligned, i.e.
	 *
	 *     IS_ALIGNED(block->offset, block->size).
	 *
	 * This is important when we use large chunks (e.g. 1G) and
	 * require the physical address to also be aligned to the chunk,
	 * e.g. huge page support in ppGTT.
	 */
	offset = round_up(start, chunk);
	size = round_down(end, chunk);
	if (size <= offset)
		return -EINVAL;

	size -= offset;

	mm->size = size;
	mm->chunk_size = chunk;

	max_order = ilog2(size) - ilog2(chunk);
	GEM_BUG_ON(max_order > I915_BUDDY_MAX_ORDER);
	mm->max_order = 0;
 
	mm->free_list = kmalloc_array(max_order + 1,
				      sizeof(struct list_head),
				      GFP_KERNEL);
	if (!mm->free_list)
		return -ENOMEM;

	for (i = 0; i <= max_order; ++i)
		INIT_LIST_HEAD(&mm->free_list[i]);

	roots = kmalloc_array(2 * max_order + 1,
			      sizeof(*roots),
			      GFP_KERNEL);
	if (!roots)
		goto out_free_list;

	/*
	 * Split into power-of-two blocks, in case we are given a size that is
	 * not itself a power-of-two, or a base address that is not naturally
	 * aligned.
	 */
	i = 0;
	do {
		struct i915_buddy_block *root;
		unsigned long order;

		order = ilog2(size);
		if (offset)
			order = min(order, __ffs64(offset));
		GEM_BUG_ON(order < ilog2(chunk));
		GEM_BUG_ON(order > ilog2(chunk) + max_order);

		root = i915_block_alloc(mm, NULL, order - ilog2(chunk), offset);
		if (!root)
			goto out_free_roots;

		GEM_BUG_ON(i915_buddy_block_size(mm, root) < chunk);
		GEM_BUG_ON(i915_buddy_block_size(mm, root) > size);

		if (order > mm->max_order)
			mm->max_order = order;
 
		mark_free(mm, root);
		roots[i++] = root;
		GEM_BUG_ON(i > 2 * max_order + 1);
 
		offset += BIT_ULL(order);
		size -= BIT_ULL(order);
	} while (size);

	mm->roots = krealloc(roots, i * sizeof(*roots), GFP_KERNEL);
	if (!mm->roots) /* Can't reduce our allocation, keep it all! */
		mm->roots = roots;
	mm->n_roots = i;

	GEM_BUG_ON(mm->max_order < ilog2(chunk));
	mm->max_order -= ilog2(chunk);

	return 0;

out_free_roots:
	while (i--)
		i915_block_free(mm, roots[i]);
	kfree(roots);
out_free_list:
	kfree(mm->free_list);
	return -ENOMEM;
}

void i915_buddy_fini(struct i915_buddy_mm *mm)
{
	int i;

	for (i = 0; i < mm->n_roots; ++i) {
		GEM_WARN_ON(!i915_buddy_block_is_free(mm->roots[i]));
		i915_block_free(mm, mm->roots[i]);
	}

	kfree(mm->roots);
	kfree(mm->free_list);
}

static int split_block(struct i915_buddy_mm *mm,
		       struct i915_buddy_block *block)
{
	unsigned int block_order = i915_buddy_block_order(block) - 1;
	u64 offset = i915_buddy_block_offset(block);

	GEM_BUG_ON(!i915_buddy_block_is_free(block));
	GEM_BUG_ON(!i915_buddy_block_order(block));

	block->left = i915_block_alloc(mm, block, block_order, offset);
	if (!block->left)
		return -ENOMEM;

	block->right = i915_block_alloc(mm, block, block_order,
					offset + (mm->chunk_size << block_order));
	if (!block->right) {
		i915_block_free(mm, block->left);
		return -ENOMEM;
	}

	mark_free(mm, block->left);
	mark_free(mm, block->right);

	mark_split(block);

	return 0;
}

static struct i915_buddy_block *
get_buddy(struct i915_buddy_block *block)
{
	struct i915_buddy_block *parent;

	parent = block->parent;
	if (!parent)
		return NULL;

	if (parent->left == block)
		return parent->right;

	return parent->left;
}

static void __i915_buddy_free(struct i915_buddy_mm *mm,
			      struct i915_buddy_block *block)
{
	struct i915_buddy_block *parent;

	while ((parent = block->parent)) {
		struct i915_buddy_block *buddy;

		buddy = get_buddy(block);

		if (!i915_buddy_block_is_free(buddy))
			break;

		list_del(&buddy->link);

		i915_block_free(mm, block);
		i915_block_free(mm, buddy);

		block = parent;
	}

	mark_free(mm, block);
}

void i915_buddy_free(struct i915_buddy_mm *mm,
		     struct i915_buddy_block *block)
{
	GEM_BUG_ON(!i915_buddy_block_is_allocated(block));
	__i915_buddy_free(mm, block);
}

void i915_buddy_free_list(struct i915_buddy_mm *mm, struct list_head *objects)
{
	struct i915_buddy_block *block, *on;

	list_for_each_entry_safe(block, on, objects, link) {
		i915_buddy_free(mm, block);
		cond_resched();
	}
	INIT_LIST_HEAD(objects);
}

/*
 * Allocate power-of-two block. The order value here translates to:
 *
 *   0 = 2^0 * mm->chunk_size
 *   1 = 2^1 * mm->chunk_size
 *   2 = 2^2 * mm->chunk_size
 *   ...
 */
struct i915_buddy_block *
i915_buddy_alloc(struct i915_buddy_mm *mm, unsigned int order)
{
	struct i915_buddy_block *block = NULL;
	unsigned int i;
	int err;

	for (i = order; i <= mm->max_order; ++i) {
		block = list_first_entry_or_null(&mm->free_list[i],
						 struct i915_buddy_block,
						 link);
		if (block)
			break;
	}

	if (!block)
		return ERR_PTR(-ENOSPC);

	GEM_BUG_ON(!i915_buddy_block_is_free(block));

	while (i != order) {
		err = split_block(mm, block);
		if (unlikely(err))
			goto out_free;

		/* Go low */
		block = block->left;
		i--;
	}

	mark_allocated(block);
	kmemleak_update_trace(block);
	return block;

out_free:
	if (i != order)
		__i915_buddy_free(mm, block);
	return ERR_PTR(err);
}

static inline bool overlaps(u64 s1, u64 e1, u64 s2, u64 e2)
{
	return s1 <= e2 && e1 >= s2;
}

static inline bool contains(u64 s1, u64 e1, u64 s2, u64 e2)
{
	return s1 <= s2 && e1 >= e2;
}

/*
 * Allocate range. Note that it's safe to chain together multiple alloc_ranges
 * with the same blocks list.
 *
 * Intended for pre-allocating portions of the address space, for example to
 * reserve a block for the initial framebuffer or similar, hence the expectation
 * here is that i915_buddy_alloc() is still the main vehicle for
 * allocations, so if that's not the case then the drm_mm range allocator is
 * probably a much better fit, and so you should probably go use that instead.
 */
int i915_buddy_alloc_range(struct i915_buddy_mm *mm,
			   struct list_head *blocks,
			   u64 start, u64 size)
{
	struct i915_buddy_block *block;
	struct i915_buddy_block *buddy;
	LIST_HEAD(allocated);
	LIST_HEAD(dfs);
	u64 end;
	int err;
	int i;

	if (GEM_WARN_ON(start + size <= start))
		return -EINVAL;

	for (i = 0; i < mm->n_roots; ++i)
		list_add_tail(&mm->roots[i]->tmp_link, &dfs);

	end = start + size;
	start = round_down(start, mm->chunk_size);
	end = round_up(end, mm->chunk_size);
	end -= 1; /* inclusive bounds testing */

	do {
		u64 block_start;
		u64 block_end;

		block = list_first_entry_or_null(&dfs,
						 struct i915_buddy_block,
						 tmp_link);
		if (!block)
			break;

		list_del(&block->tmp_link);

		block_start = i915_buddy_block_offset(block);
		block_end = block_start + i915_buddy_block_size(mm, block) - 1;

		if (!overlaps(start, end, block_start, block_end))
			continue;

		if (i915_buddy_block_is_allocated(block)) {
			err = -ENOSPC;
			goto err_free;
		}

		if (contains(start, end, block_start, block_end)) {
			if (!i915_buddy_block_is_free(block)) {
				err = -ENOSPC;
				goto err_free;
			}

			mark_allocated(block);
			list_add_tail(&block->link, &allocated);
			continue;
		}

		if (!i915_buddy_block_is_split(block)) {
			err = split_block(mm, block);
			if (unlikely(err))
				goto err_undo;
		}

		list_add(&block->right->tmp_link, &dfs);
		list_add(&block->left->tmp_link, &dfs);
	} while (1);

	list_splice_tail(&allocated, blocks);
	return 0;

err_undo:
	/*
	 * We really don't want to leave around a bunch of split blocks, since
	 * bigger is better, so make sure we merge everything back before we
	 * free the allocated blocks.
	 */
	buddy = get_buddy(block);
	if (buddy &&
	    (i915_buddy_block_is_free(block) &&
	     i915_buddy_block_is_free(buddy)))
		__i915_buddy_free(mm, block);

err_free:
	i915_buddy_free_list(mm, &allocated);
	return err;
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftests/i915_buddy.c"
#endif

void i915_buddy_module_exit(void)
{
	kmem_cache_destroy(slab_blocks);
}

int __init i915_buddy_module_init(void)
{
	slab_blocks = KMEM_CACHE(i915_buddy_block, 0);
	if (!slab_blocks)
		return -ENOMEM;

	return 0;
}
