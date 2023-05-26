//SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <linux/pm_qos.h>

#include <drm/i915_pciids.h>

#include "gt/intel_gt_clock_utils.h"
#include "gt/intel_rps.h"

#include "i915_selftest.h"
#include "intel_pcode.h"
#include "selftests/i915_random.h"
#include "selftests/igt_flush_test.h"

#define CPU_LATENCY 0 /* -1 to disable pm_qos, 0 to disable cstates */

#define _MBs(x) (((x) * 1000000ull) >> 20)
#define MBs(x) (void *)_MBs(x)
static const struct pci_device_id clear_bandwidth[] = {
	INTEL_DG1_IDS(MBs(66000)),
	INTEL_DG2_G10_IDS(MBs(360000)),
	INTEL_DG2_G11_IDS(MBs(33000)),
	INTEL_DG2_G12_IDS(MBs(250000)),
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

static int __igt_lmem_clear(struct drm_i915_private *i915, bool measure)
{
	const u64 poison = make_u64(0xc5c55c5c, 0xa3a33a3a);
	struct pm_qos_request qos;
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

	if (CPU_LATENCY >= 0)
		cpu_latency_qos_add_request(&qos, CPU_LATENCY);

	for_each_gt(gt, i915, id) {
		struct intel_context *ce;
		unsigned int max_bw = 0;
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
			ktime_t cpu, gpu, sync;
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
				break;
			}

			block = list_first_entry(&blocks, typeof(*block), link);
			offset = i915_buddy_block_offset(block);

			sg_dma_address(pages->sgl) = offset;
			sg_dma_len(pages->sgl) = i915_buddy_block_size(&gt->lmem->mm, block);

			cpu = -ktime_get();
			clear_cpu(gt->lmem, pages, poison);
			cpu += ktime_get();

			gpu = -READ_ONCE(gt->counters.map[INTEL_GT_CLEAR_CYCLES]);
			sync = -ktime_get();
			err = clear_blt(ce, NULL, pages, size, 0, &rq);
			if (rq) {
				i915_sw_fence_complete(&rq->submit);
				if (i915_request_wait(rq, I915_WAIT_INTERRUPTIBLE, HZ) < 0)
					err = -ETIME;
				else
					err = rq->fence.error;
				i915_request_put(rq);
			}
			sync += ktime_get();
			gpu += READ_ONCE(gt->counters.map[INTEL_GT_CLEAR_CYCLES]);

			gpu = intel_gt_clock_interval_to_ns(gt, gpu);
			if (gpu) {
				unsigned int sz = sg_dma_len(pages->sgl);
				unsigned int cpu_bw , gpu_bw;

				cpu_bw = div_u64(mul_u64_u32_shr(sz, NSEC_PER_SEC, 20), cpu);
				gpu_bw = div_u64(mul_u64_u32_shr(sz, NSEC_PER_SEC, 20), gpu);

				dev_info(gt->i915->drm.dev,
					 "GT%d: checked with size:%x, CPU write:%dMiB/s, GPU write:%dMiB/s, overhead:%lldns, freq:%dMHz\n",
					 id, sz, cpu_bw, gpu_bw, sync - gpu,
					 intel_rps_read_actual_frequency(&gt->rps));

				max_bw = max(max_bw, gpu_bw);
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

			__intel_memory_region_put_pages_buddy(gt->lmem, &blocks);
			if (err)
				break;
		}

		intel_rps_cancel_boost(&gt->rps);
		intel_gt_pm_put(gt, wf);
		if (err)
			break;

		if (measure && max_bw) {
			struct pci_dev *pdev = to_pci_dev(gt->i915->drm.dev);
			unsigned int expected = 0;

			if (IS_PONTEVECCHIO(gt->i915)) {
				u32 val;

				if (snb_pcode_read_p(gt->uncore,
						     XEHPSDV_PCODE_FREQUENCY_CONFIG,
						     PCODE_MBOX_FC_SC_READ_FUSED_P0,
						     PCODE_MBOX_DOMAIN_HBM,
						     &val) == 0)
					expected = _MBs(2 * 128 * val * GT_FREQUENCY_MULTIPLIER);
			} else if (HAS_LMEM_MAX_BW(gt->i915)) {
				u32 val;

				if (snb_pcode_read_p(&i915->uncore, PCODE_MEMORY_CONFIG,
						     MEMORY_CONFIG_SUBCOMMAND_READ_MAX_BANDWIDTH,
						     0x0,
						     &val) == 0)
					expected = _MBs(val);
			} else {
				const struct pci_device_id *match;

				match = pci_match_id(clear_bandwidth, pdev);
				if (match)
					expected = (uintptr_t)match->driver_data;
			}

			if (7 * max_bw > expected * 8) {
				dev_warn(gt->i915->drm.dev,
					 "[0x%04x.%d] Peak bw measured:%d MiB/s, beyond expected %d MiB/s\n",
					 pdev->device, pdev->revision,
					 max_bw, expected);
			} else if (8 * max_bw < expected * 7) {
				dev_err(gt->i915->drm.dev,
					"[0x%04x.%d] Peak bw measured:%d MiB/s, expected at least 87.5%% of %d MiB/s [%d MiB/s]\n",
					pdev->device, pdev->revision,
					max_bw, expected,
					7 * expected >> 3);
				err = -ENXIO;
				break;
			}
		}
	}

	if (CPU_LATENCY >= 0)
		cpu_latency_qos_remove_request(&qos);

	sg_free_table(pages);
	kfree(pages);

	if (igt_flush_test(i915))
		err = -EIO;
	if (err == -ETIME)
		err = 0;

	return err;
}

static int igt_lmem_clear(void *arg)
{
	return __igt_lmem_clear(arg, false);
}

static int igt_lmem_speed(void *arg)
{
	return __igt_lmem_clear(arg, true);
}

int i915_gem_lmem_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_lmem_touch),
		SUBTEST(igt_lmem_clear),
	};

	return i915_live_subtests(tests, i915);
}

int i915_gem_lmem_wip_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_lmem_speed),
	};

	return i915_live_subtests(tests, i915);
}
