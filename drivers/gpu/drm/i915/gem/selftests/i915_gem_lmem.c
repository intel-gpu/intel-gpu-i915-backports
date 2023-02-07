//SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "i915_selftest.h"
#include "selftests/i915_random.h"
#include "selftests/igt_flush_test.h"

static int igt_lmem_clear(void *arg)
{
	const u64 poison = make_u64(0xc5c55c5c, 0xa3a33a3a);
	struct drm_i915_private *i915 = arg;
	struct sg_table *pages;
	struct intel_gt *gt;
	I915_RND_STATE(prng);
	int err = 0;
	int id;

	pages = kmalloc(sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	if (sg_alloc_table(pages, 1, GFP_KERNEL)) {
		kfree(pages);
		return -ENOMEM;
	}

	for_each_gt(gt, i915, id) {
		struct intel_context *ce;
		u64 size;

		if (!gt->lmem)
			continue;

		ce = get_blitter_context(gt, BCS0);
		if (!ce)
			continue;

		for (size = SZ_4K; size <= min_t(u64, gt->lmem->total / 2, SZ_2G); size <<= 1) {
			struct i915_buddy_block *block;
			struct i915_request *rq;
			ktime_t dt = -ktime_get();
			LIST_HEAD(blocks);
			u64 offset;

			err = __intel_memory_region_get_pages_buddy(gt->lmem,
								    NULL,
								    size,
								    0,
								    &blocks);
			if (err) {
				pr_err("GT%d: failed to allocate %llx\n",
				       id, size);
				goto out;
			}

			block = list_first_entry(&blocks, typeof(*block), link);
			offset = i915_buddy_block_offset(block);

			sg_dma_address(pages->sgl) = offset;
			sg_dma_len(pages->sgl) = i915_buddy_block_size(&gt->lmem->mm, block);

			clear_cpu(gt->lmem, pages, poison);

			err = clear_blt(ce, pages, size, 0, &rq);
			if (rq) {
				i915_sw_fence_complete(&rq->submit);
				if (i915_request_wait(rq, 0, HZ) < 0)
					err = -ETIME;
				else
					err = rq->fence.error;
				i915_request_put(rq);
			}
			if (err == 0) {
				void * __iomem iova;
				u64 sample;

				offset -= gt->lmem->region.start;
				iova = io_mapping_map_wc(&gt->lmem->iomap, offset, size);

				offset = igt_random_offset(&prng, 0, sg_dma_len(pages->sgl), sizeof(sample), 1);
				memcpy_fromio(&sample, iova + offset, sizeof(sample));
				io_mapping_unmap(iova);

				if (sample) {
					pr_err("GT%d: read @%llx of [%llx + %llx] and found %llx instead of zero!\n",
					       id, offset,
					       i915_buddy_block_offset(block),
					       i915_buddy_block_size(&gt->lmem->mm, block),
					       sample);
					err = -EINVAL;
				}
			}

			dt += ktime_get();
			dev_info(gt->i915->drm.dev,
				 "GT%d: checked with size:%x, %lldMiB/s\n",
				 id, sg_dma_len(pages->sgl),
				 div_u64(mul_u32_u32(1000, sg_dma_len(pages->sgl)),
					 dt));

			__intel_memory_region_put_pages_buddy(gt->lmem, &blocks);
			if (err)
				goto out;
		}
	}

out:
	sg_free_table(pages);
	kfree(pages);

	if (igt_flush_test(i915))
		err = -EIO;

	return err;
}

int i915_gem_lmem_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_lmem_clear),
	};

	return i915_live_subtests(tests, i915);
}
