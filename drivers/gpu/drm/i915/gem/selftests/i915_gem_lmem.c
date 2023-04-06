//SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */
#include <linux/pm_qos.h>
#include <drm/i915_pciids.h>

#include "gt/intel_gt_clock_utils.h"
#include "gt/intel_rps.h"

#include "i915_selftest.h"
#include "selftests/i915_random.h"
#include "selftests/igt_flush_test.h"

#define CPU_LATENCY 0 /* -1 to disable pm_qos, 0 to disable cstates */

#define MBs(x) (void *)(((x) * 1000000ull) >> 20)
static const struct pci_device_id clear_bandwidth[] = {
	INTEL_DG1_IDS(MBs(66000)),
	INTEL_DG2_G10_IDS(MBs(360000)),
	INTEL_DG2_G11_IDS(MBs(33000)),
	INTEL_DG2_G12_IDS(MBs(250000)),
	INTEL_ATS_M75_IDS(MBs(111700)),
	INTEL_ATS_M150_IDS(MBs(360000)),
	INTEL_PVC_IDS(MBs(800000)),
	{},
};

static int igt_lmem_touch(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct drm_printer p = drm_info_printer(i915->drm.dev);
	struct intel_gt *gt;
	int id, err;

	for_each_gt(gt, i915, id) {
		err = i915_gem_clear_all_lmem(gt, &p);
		if (err)
			return err;
	}

	return 0;
}

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
		intel_wakeref_t wf;
		u64 size;

		if (!gt->lmem)
			continue;

		ce = get_blitter_context(gt, BCS0);
		if (!ce)
			continue;

		wf = intel_gt_pm_get(gt);
		intel_rps_boost(&gt->rps);

		for (size = SZ_4K; size <= min_t(u64, gt->lmem->total / 2, SZ_2G); size <<= 1) {
			struct i915_buddy_block *block;
			struct i915_request *rq;
			ktime_t dt = -ktime_get();
			LIST_HEAD(blocks);
			ktime_t sync, cpu;
			u64 offset;
			u64 cycles;

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

			cycles = -READ_ONCE(gt->counters.map[INTEL_GT_CLEAR_CYCLES]);
			sync = -ktime_get();
			err = clear_blt(ce, pages, size, 0, &rq);
			if (rq) {
				i915_sw_fence_complete(&rq->submit);
				if (i915_request_wait(rq, 0, HZ) < 0)
					err = -ETIME;
				else
					err = rq->fence.error;
				i915_request_put(rq);
			}
			sync += ktime_get();
			cycles += READ_ONCE(gt->counters.map[INTEL_GT_CLEAR_CYCLES]);
			cycles = intel_gt_clock_interval_to_ns(gt, cycles);
			if (cycles) {
				dev_info(gt->i915->drm.dev,
					 "GT%d: checked with size:%x, CPU write:%lldMiB/s, GPU write:%lldMiB/s, overhead:%lldns\n",
					 id, sg_dma_len(pages->sgl),
					 div_u64(mul_u32_u32(1000, sg_dma_len(pages->sgl)), cpu),
					 div_u64(mul_u32_u32(1000, sg_dma_len(pages->sgl)), cycles),
					 sync - cycles);
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

		intel_rps_cancel_boost(&gt->rps);
		intel_gt_pm_put(gt, wf);
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
		SUBTEST(igt_lmem_touch),
		SUBTEST(igt_lmem_clear),
	};

	return i915_live_subtests(tests, i915);
}
