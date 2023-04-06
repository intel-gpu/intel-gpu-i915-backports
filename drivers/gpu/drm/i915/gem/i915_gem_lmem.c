// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <uapi/drm/i915_drm.h>

#include "gt/gen8_engine_cs.h"
#include "gt/intel_context.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_engine_regs.h"
#include "gt/intel_gpu_commands.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_pm.h"
#include "gt/intel_ring.h"

#include "intel_memory_region.h"
#include "gem/i915_gem_region.h"
#include "gem/i915_gem_lmem.h"
#include "gt/intel_rps.h"
#include "gt/intel_gt_clock_utils.h"
#include "i915_drv.h"

static u32 *emit_timestamp(struct i915_request *rq, u32 *cs, int gpr)
{
	*cs++ = MI_LOAD_REGISTER_REG | MI_LRR_SOURCE_CS_MMIO | MI_LRI_LRM_CS_MMIO;
	*cs++ = i915_mmio_reg_offset(RING_TIMESTAMP_UDW(0));
	*cs++ = i915_mmio_reg_offset(GEN8_RING_CS_GPR_UDW(0, gpr));

	*cs++ = MI_LOAD_REGISTER_REG | MI_LRR_SOURCE_CS_MMIO | MI_LRI_LRM_CS_MMIO;
	*cs++ = i915_mmio_reg_offset(RING_TIMESTAMP(0));
	*cs++ = i915_mmio_reg_offset(GEN8_RING_CS_GPR(0, gpr));

	return cs;
}

static int emit_start_timestamp(struct i915_request *rq)
{
	u32 *cs;

	cs = intel_ring_begin(rq, 6);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	cs = emit_timestamp(rq, cs, 0);

	intel_ring_advance(rq, cs);
	return 0;
}

static u32 *emit_mem_fence(struct i915_request *rq, u32 *cs)
{
	u32 scratch = i915_ggtt_offset(rq->engine->gt->scratch);

	return gen8_emit_ggtt_write(cs, 0, scratch, 0);
}

static int emit_update_counters(struct i915_request *rq, u64 size)
{
	u32 global = i915_ggtt_offset(rq->engine->gt->counters.vma);
	u32 *cs;

	cs = intel_ring_begin(rq, 26);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	cs = emit_mem_fence(rq, cs);
	cs = emit_timestamp(rq, cs, 1);

	/* Compute elapsed time (end - start) */
	*cs++ = MI_MATH(4);
	*cs++ = MI_MATH_LOAD(MI_MATH_REG_SRCA, MI_MATH_REG(1));
	*cs++ = MI_MATH_LOAD(MI_MATH_REG_SRCB, MI_MATH_REG(0));
	*cs++ = MI_MATH_SUB;
	*cs++ = MI_MATH_STORE(MI_MATH_REG(0), MI_MATH_REG_ACCU);

	/* Increment cycle counters */
	*cs++ = MI_ATOMIC | MI_ATOMIC_ADD64 | MI_ATOMIC64 | MI_USE_GGTT;
	*cs++ = global + INTEL_GT_CLEAR_CYCLES * sizeof(u64);
	*cs++ = 0;

	/* Increment byte counters */
	*cs++ = MI_LOAD_REGISTER_IMM(2) | MI_LRI_LRM_CS_MMIO;
	*cs++ = i915_mmio_reg_offset(GEN8_RING_CS_GPR(0, 0));
	*cs++ = lower_32_bits(size);
	*cs++ = i915_mmio_reg_offset(GEN8_RING_CS_GPR_UDW(0, 0));
	*cs++ = upper_32_bits(size);

	*cs++ = MI_ATOMIC | MI_ATOMIC_ADD64 | MI_ATOMIC64 | MI_USE_GGTT;
	*cs++ = global + INTEL_GT_CLEAR_BYTES * sizeof(u64);
	*cs++ = 0;

	intel_ring_advance(rq, cs);
	return 0;
}

static struct intel_context *
get_blitter_context(const struct intel_gt *gt, int idx)
{
	if (intel_gt_is_wedged(gt))
		return NULL;

	return gt->engine[idx] ? gt->engine[idx]->blitter_context : NULL;
}

static struct intel_context *get_clear_context(const struct intel_gt *gt)
{
	return get_blitter_context(gt, gt->rsvd_bcs);
}

void __iomem *
i915_gem_object_lmem_io_map_page_atomic(struct drm_i915_gem_object *obj,
					unsigned long n)
{
	resource_size_t offset;

	offset = i915_gem_object_get_dma_address(obj, n);
	offset -= obj->mm.region->region.start;

	return io_mapping_map_atomic_wc(&obj->mm.region->iomap, offset);
}

void __iomem *
i915_gem_object_lmem_io_map(struct drm_i915_gem_object *obj,
			    unsigned long n,
			    unsigned long size)
{
	resource_size_t offset;

	GEM_BUG_ON(!i915_gem_object_is_contiguous(obj));

	offset = i915_gem_object_get_dma_address(obj, n);
	offset -= obj->mm.region->region.start;

	return io_mapping_map_wc(&obj->mm.region->iomap, offset, size);
}

unsigned long i915_gem_object_lmem_offset(struct drm_i915_gem_object *obj)
{
	GEM_BUG_ON(!(obj->flags & I915_BO_ALLOC_CONTIGUOUS));
	return i915_gem_object_get_dma_address(obj, 0);
}

/**
 * i915_gem_object_validates_to_lmem - Whether the object is resident in
 * lmem when pages are present.
 * @obj: The object to check.
 *
 * Migratable objects residency may change from under us if the object is
 * not pinned or locked. This function is intended to be used to check whether
 * the object can only reside in lmem when pages are present.
 *
 * Return: Whether the object is always resident in lmem when pages are
 * present.
 */
bool i915_gem_object_validates_to_lmem(struct drm_i915_gem_object *obj)
{
	struct intel_memory_region *mr = READ_ONCE(obj->mm.region);

	return !i915_gem_object_migratable(obj) &&
		mr && (mr->type == INTEL_MEMORY_LOCAL ||
		       mr->type == INTEL_MEMORY_STOLEN_LOCAL);
}

/**
 * i915_gem_object_is_lmem - Whether the object is resident in
 * lmem
 * @obj: The object to check.
 *
 * Even if an object is allowed to migrate and change memory region,
 * this function checks whether it will always be present in lmem when
 * valid *or* if that's not the case, whether it's currently resident in lmem.
 * For migratable and evictable objects, the latter only makes sense when
 * the object is locked.
 *
 * Return: Whether the object migratable but resident in lmem, or not
 * migratable and will be present in lmem when valid.
 */
bool i915_gem_object_is_lmem(const struct drm_i915_gem_object *obj)
{
	struct intel_memory_region *mr = READ_ONCE(obj->mm.region);
#if 0
#ifdef CONFIG_LOCKDEP
	if (i915_gem_object_migratable(obj) &&
	    i915_gem_object_evictable(obj))
		assert_object_held(obj);
#endif
#endif
	return mr && (mr->type == INTEL_MEMORY_LOCAL ||
		      mr->type == INTEL_MEMORY_STOLEN_LOCAL);
}

struct drm_i915_gem_object *
i915_gem_object_create_lmem_from_data(struct intel_memory_region *region,
				      const void *data, size_t size)
{
	struct drm_i915_gem_object *obj;
	void *map;

	obj = i915_gem_object_create_region(region,
					    round_up(size, PAGE_SIZE),
					    I915_BO_ALLOC_CONTIGUOUS);
	if (IS_ERR(obj))
		return obj;

	map = i915_gem_object_pin_map_unlocked(obj, I915_MAP_WC);
	if (IS_ERR(map)) {
		i915_gem_object_put(obj);
		return map;
	}

	memcpy(map, data, size);

	i915_gem_object_unpin_map(obj);

	return obj;
}

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

static int emit_flush(struct i915_request *rq, unsigned int flags)
{
	u32 *cs;

	cs = intel_ring_begin(rq, 4);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	*cs++ = (MI_FLUSH_DW + 1) | flags;
	*cs++ = 0;
	*cs++ = 0;
	*cs++ = 0;

	intel_ring_advance(rq, cs);
	return 0;
}

static int num_ccs_blocks(unsigned int size)
{
	return DIV_ROUND_UP(size,
			    NUM_BYTES_PER_CCS_BYTE * NUM_CCS_BYTES_PER_BLOCK);
}

static int
emit_ccs_clear(struct i915_request *rq, u64 offset, u32 length)
{
	u32 mocs = REG_FIELD_PREP(XY_CSC_BLT_MOCS_INDEX_MASK_XEHP,
				  rq->engine->gt->mocs.uc_index);
	u64 zero = offset;
	int err;

	err = emit_flush(rq, 0);
	if (err)
		return err;

	do {
		u32 blocks = min_t(u32, length, SZ_64M);
		u32 *cs;

		cs = intel_ring_begin(rq, 6);
		if (IS_ERR(cs))
			return PTR_ERR(cs);

		*cs++ = XY_CTRL_SURF_COPY_BLT |
			DIRECT_ACCESS << SRC_ACCESS_TYPE_SHIFT |
			INDIRECT_ACCESS << DST_ACCESS_TYPE_SHIFT |
			REG_FIELD_PREP(CCS_SIZE_MASK_XEHP,
				       num_ccs_blocks(blocks) - 1);

		*cs++ = lower_32_bits(zero);
		*cs++ = upper_32_bits(zero) | mocs;
		*cs++ = lower_32_bits(offset);
		*cs++ = upper_32_bits(offset) | mocs;
		*cs++ = MI_NOOP;

		intel_ring_advance(rq, cs);

		offset += SZ_64M;
		length -= blocks;
	} while (length);

	return emit_flush(rq, MI_FLUSH_DW_LLC | MI_FLUSH_DW_CCS);
}

static int
lmem_swapout(struct drm_i915_gem_object *obj,
	     struct sg_table *pages, unsigned int sizes)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct i915_mm_swap_stat *stat = NULL;
	struct drm_i915_gem_object *dst, *src;
	ktime_t start = ktime_get();
	int err = -EINVAL;
	u64 size;

	GEM_BUG_ON(obj->swapto);
	assert_object_held(obj);

	/* create a shadow object on smem region */
	size = obj->base.size;
	if (HAS_FLAT_CCS(i915))
		size += size >> 8;
	dst = i915_gem_object_create_shmem(i915, size);
	if (IS_ERR(dst))
		return PTR_ERR(dst);

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
	if (i915->params.enable_eviction >= 2 &&
	    !intel_gt_is_wedged(obj->mm.region->gt)) {
		err = i915_window_blt_copy(dst, src, HAS_FLAT_CCS(i915));
		if (!err)
			stat = &i915->mm.blt_swap_stats.out;
	}

	if (err &&
	    err != -ERESTARTSYS && err != -EINTR &&
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
			i915_log_driver_error(i915, I915_DRIVER_ERROR_OBJECT_MIGRATION,
					      "Failed to swap-out object (%d)\n", err);
		i915_gem_object_put(dst);
	}

	__update_stat(stat, obj->base.size >> PAGE_SHIFT, start);

	return err;
}

static int
lmem_swapin(struct drm_i915_gem_object *obj,
	    struct sg_table *pages, unsigned int sizes)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct drm_i915_gem_object *dst, *src = obj->swapto;
	struct i915_mm_swap_stat *stat = NULL;
	ktime_t start = ktime_get();
	int err = -EINVAL;

	assert_object_held(obj);

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
	if (i915->params.enable_eviction >= 2 &&
	    !intel_gt_is_wedged(obj->mm.region->gt)) {
		err = i915_window_blt_copy(dst, src, HAS_FLAT_CCS(i915));
		if (!err)
			stat = &i915->mm.blt_swap_stats.in;
	}

	if (err &&
	    err != -ERESTARTSYS && err != -EINTR &&
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
	} else {
		if (err != -EINTR && err != -ERESTARTSYS)
			i915_log_driver_error(i915, I915_DRIVER_ERROR_OBJECT_MIGRATION,
					      "Failed to swap-in object (%d)\n", err);
	}

	__update_stat(stat, obj->base.size >> PAGE_SHIFT, start);

	return err;
}

static int
pvc_emit_clear(struct i915_request *rq, u64 offset, u32 size, u32 page_shift)
{
	u32 mocs;
	u32 *cs;

	cs = intel_ring_begin(rq, 8);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	*cs++ = PVC_MEM_SET_CMD | MS_MATRIX | (7 - 2);

	*cs++ = BIT(page_shift) - 1;
	*cs++ = (size >> page_shift) - 1;
	*cs++ = BIT(page_shift) - 1;

	*cs++ = lower_32_bits(offset);
	*cs++ = upper_32_bits(offset);

	mocs = rq->engine->gt->mocs.uc_index;
	mocs = FIELD_PREP(MS_MOCS_INDEX_MASK, mocs);
	*cs++ = mocs;

	*cs++ = MI_NOOP;
	intel_ring_advance(rq, cs);

	return 0;
}

static int
xy_emit_clear(struct i915_request *rq, u64 offset, u32 size, u32 page_shift)
{
	u32 mocs = 0;
	int len;
	u32 *cs;

	GEM_BUG_ON(page_shift > 18); /* max stride */
	GEM_BUG_ON(BIT(page_shift) / 4 > S16_MAX); /* max width */
	GEM_BUG_ON(size >> page_shift > S16_MAX); /* max height */

	len = 11;
	if (GRAPHICS_VER_FULL(rq->engine->i915) >= IP_VER(12, 50)) {
		mocs = rq->engine->gt->mocs.uc_index << 1;
		mocs = FIELD_PREP(XY_FAST_COLOR_BLT_MOCS_MASK, mocs);
		len = 16;
	}

	cs = intel_ring_begin(rq, 16);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	*cs++ = GEN9_XY_FAST_COLOR_BLT_CMD |
		XY_FAST_COLOR_BLT_DEPTH_32 |
		(len - 2);
	*cs++ = mocs | (BIT(page_shift) - 1);
	*cs++ = 0;
	*cs++ = size >> page_shift << 16 | BIT(page_shift) / 4;
	*cs++ = lower_32_bits(offset);
	*cs++ = upper_32_bits(offset);
	*cs++ = 0;
	*cs++ = 0;
	*cs++ = 0;
	*cs++ = 0;
	*cs++ = 0;
	*cs++ = 0;
	*cs++ = 0;
	*cs++ = 0;
	*cs++ = 0;
	*cs++ = 0;

	intel_ring_advance(rq, cs);

	return 0;
}

static struct i915_request *
chain_request(struct i915_request *rq, struct i915_request *chain)
{
	struct intel_timeline *tl = rq->context->timeline;
	struct i915_sched_attr attr = {};

	/*
	 * Hold the request until the next is chained. We need
	 * a complete chain in order to propagate any error to the
	 * final fence, and into the obj->mm.migrate. If we drop
	 * the error at any point (due to a completed request),
	 * then we may continue to use the uninitialised contents.
	 */

	lockdep_assert_held(&tl->mutex);
	lockdep_unpin_lock(&tl->mutex, rq->cookie);

	i915_sw_fence_await(&rq->submit);
	i915_request_get(rq);

	trace_i915_request_add(rq);
	__i915_request_commit(rq);
	__i915_request_queue(rq, &attr);

	if (chain) {
		i915_sw_fence_complete(&chain->submit);
		i915_request_put(chain);
	}

	return rq;
}

static int
clear_blt(struct intel_context *ce,
	  struct sg_table *sgt,
	  unsigned int page_sizes,
	  unsigned int flags,
	  struct i915_request **out)
{
	const int page_shift = min(__ffs(page_sizes), 16ul);
	const u32 step = BIT(page_shift) << 14; /* 128-1024MiB */
	bool use_pvc_memset = HAS_LINK_COPY_ENGINES(ce->engine->i915);
	const bool use_ccs_clear =
		!use_pvc_memset &&
		flags & I915_BO_ALLOC_USER &&
		HAS_FLAT_CCS(ce->engine->i915);
	struct sgt_iter it;
	u64 offset;
	int err;

	GEM_BUG_ON(ce->ring->size < SZ_64K);
	GEM_BUG_ON(ce->vm != ce->engine->gt->vm);
	GEM_BUG_ON(!drm_mm_node_allocated(&ce->engine->gt->flat));

	*out = NULL;

	err = intel_context_throttle(ce);
	if (err)
		return err;

	mutex_lock(&ce->timeline->mutex);
	intel_context_enter(ce);
	__for_each_sgt_daddr(offset, it, sgt, step) {
		struct i915_request *rq;
		u32 length;

		length = min_t(u32, it.max - it.curr, step);
		if (!length)
			continue;

		GEM_BUG_ON(offset < ce->engine->gt->flat.start);
		GEM_BUG_ON(offset + length > ce->engine->gt->flat.start + ce->engine->gt->flat.size);

		rq = i915_request_create_locked(ce, I915_GFP_ALLOW_FAIL);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			break;
		}

		err = emit_start_timestamp(rq);
		if (err)
			goto skip;

		if (use_pvc_memset)
			err = pvc_emit_clear(rq, offset, length, page_shift);
		else
			err = xy_emit_clear(rq, offset, length, page_shift);
		if (err == 0 && use_ccs_clear)
			err = emit_ccs_clear(rq, offset, length);
		if (err)
			goto skip;

		err = emit_update_counters(rq, length);
		if (err)
			goto skip;

skip:
		*out = chain_request(rq, *out);
		if (err)
			break;
	}
	intel_context_exit(ce);
	mutex_unlock(&ce->timeline->mutex);

	return err;
}

static void clear_cpu(struct intel_memory_region *mem, struct sg_table *sgt, u64 value)
{
        struct scatterlist *sg;

        for (sg = sgt->sgl; sg; sg = __sg_next(sg)) {
                unsigned int length;
                void __iomem *vaddr;
                dma_addr_t daddr;

                length = sg_dma_len(sg);
                if (!length)
                        continue;

                daddr = sg_dma_address(sg);
                daddr -= mem->region.start;

                vaddr = io_mapping_map_wc(&mem->iomap, daddr, length);
                memset64((void __force *)vaddr, value, length / sizeof(u64));
                io_mapping_unmap(vaddr);
        }

        wmb();
}

static inline bool
small_sync_clear(const struct drm_i915_gem_object *obj, unsigned int flags)
{
	if (!(flags & I915_BO_ALLOC_SYNC_HINT))
		return false;

	/* Assume exec + sync latency ~2ms and WC bw of ~4GiB/s */
	return obj->base.size <= SZ_32M;
}

static inline bool
use_flat_ccs(struct intel_gt *gt)
{
	/* If the device is wedged, [stale] indirect CCS is inaccessible */
	return HAS_FLAT_CCS(gt->i915) && !intel_gt_is_wedged(gt);
}

static inline bool
use_cpu_clear(struct intel_gt *gt, unsigned int flags)
{
	if (!(flags & I915_BO_ALLOC_CPU_CLEAR))
		return false;

	if (!(flags & I915_BO_ALLOC_USER))
		return true;

	return !use_flat_ccs(gt);
}

static int lmem_clear(struct drm_i915_gem_object *obj,
		      struct sg_table *pages,
		      unsigned int page_sizes,
		      struct i915_request **out)
{
	struct intel_memory_region *mem = obj->mm.region;
	unsigned int flags = obj->flags;
	struct intel_gt *gt = mem->gt;
	struct intel_context *ce;
	intel_wakeref_t wf = 0;
	int err = 0;

	ce = NULL;
	if (flags & (I915_BO_ALLOC_USER | I915_BO_ALLOC_CPU_CLEAR)) {
		ce = get_clear_context(gt);
		if (!ce || small_sync_clear(obj, flags))
			flags |= I915_BO_ALLOC_CPU_CLEAR;
	}

	/* Avoid misspending PCI credits between the GT; must use BLT clears */
	if (ce && gt->info.id > 0 && intel_gt_pm_is_awake(gt))
		flags &= ~I915_BO_ALLOC_CPU_CLEAR;

	/* Clear is too small to benefit from waking up the GPU */
	if (ce && page_sizes < SZ_2M && !(wf = intel_gt_pm_get_if_awake(gt)))
		flags |= I915_BO_ALLOC_CPU_CLEAR;

	/* Intended for kernel internal use only */
	if (use_cpu_clear(gt, flags))
		clear_cpu(mem, pages, 0);
	else if (ce)
		err = clear_blt(ce, pages, page_sizes, flags, out);
	else if (flags & I915_BO_ALLOC_CPU_CLEAR)
		err = -EIO;

	if (wf)
		intel_gt_pm_put(gt, wf);

	return err;
}

static int lmem_get_pages(struct drm_i915_gem_object *obj)
{
	struct i915_request *rq = NULL;
	unsigned int page_sizes;
	struct sg_table *pages;
	int err;

	pages = i915_gem_object_get_pages_buddy(obj, &page_sizes);
	if (IS_ERR(pages))
		return PTR_ERR(pages);

	if (obj->swapto)
		err = lmem_swapin(obj, pages, page_sizes);
	else
		err = lmem_clear(obj, pages, page_sizes, &rq);
	if (rq) {
		i915_gem_object_migrate_prepare(obj, rq);
		i915_sw_fence_complete(&rq->submit);
		i915_request_put(rq);
	}
	if (err)
		goto err;

	__i915_gem_object_set_pages(obj, pages, page_sizes);
	return 0;

err:
	i915_gem_object_migrate_finish(obj);
	i915_gem_object_put_pages_buddy(obj, pages);
	return err;
}

static int
lmem_put_pages(struct drm_i915_gem_object *obj, struct sg_table *pages)
{
	/* if need to save the page contents, swap them out */
	if (obj->do_swapping && !i915_gem_object_migrate_has_error(obj)) {
		unsigned int sizes = obj->mm.page_sizes.phys;
		int err;

		GEM_BUG_ON(obj->mm.madv != I915_MADV_WILLNEED);
		GEM_BUG_ON(i915_gem_object_is_volatile(obj));

		err = lmem_swapout(obj, pages, sizes);
		if (err)
			return err;
	}

	i915_gem_object_migrate_finish(obj);
	obj->flags &= ~I915_BO_ALLOC_SYNC_HINT;

	return i915_gem_object_put_pages_buddy(obj, pages);
}

static int
i915_ww_pin_lock_interruptible(struct drm_i915_gem_object *obj)
{
	struct i915_gem_ww_ctx ww;
	int ret;

	for_i915_gem_ww(&ww, ret, true) {
		ret = i915_gem_object_lock(obj, &ww);
		if (ret)
			continue;

		ret = i915_gem_object_pin_pages(obj);
		if (ret)
			continue;

		ret = i915_gem_object_set_to_wc_domain(obj, false);
		if (ret)
			goto out_unpin;

		ret = i915_gem_object_wait(obj,
					   I915_WAIT_INTERRUPTIBLE,
					   MAX_SCHEDULE_TIMEOUT);
		if (!ret)
			continue;

out_unpin:
		i915_gem_object_unpin_pages(obj);

		/* Unlocking is done implicitly */
	}

	return ret;
}

static int lmem_pread(struct drm_i915_gem_object *obj,
		      const struct drm_i915_gem_pread *arg)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct intel_runtime_pm *rpm = &i915->runtime_pm;
	intel_wakeref_t wakeref;
	char __user *user_data;
	unsigned int offset;
	unsigned long idx;
	u64 remain;
	int ret;

	ret = i915_gem_object_wait(obj,
				   I915_WAIT_INTERRUPTIBLE,
				   MAX_SCHEDULE_TIMEOUT);
	if (ret)
		return ret;

	ret = i915_ww_pin_lock_interruptible(obj);
	if (ret)
		return ret;

	wakeref = intel_runtime_pm_get(rpm);

	remain = arg->size;
	user_data = u64_to_user_ptr(arg->data_ptr);
	offset = offset_in_page(arg->offset);
	for (idx = arg->offset >> PAGE_SHIFT; remain; idx++) {
		unsigned long unwritten;
		void __iomem *vaddr;
		int length;

		length = remain;
		if (offset + length > PAGE_SIZE)
			length = PAGE_SIZE - offset;

		vaddr = i915_gem_object_lmem_io_map_page_atomic(obj, idx);
		if (!vaddr) {
			ret = -ENOMEM;
			goto out_put;
		}
		unwritten = __copy_to_user_inatomic(user_data,
						    (void __force *)vaddr + offset,
						    length);
		io_mapping_unmap_atomic(vaddr);
		if (unwritten) {
			vaddr = i915_gem_object_lmem_io_map_page(obj, idx);
			if (!IS_ERR_OR_NULL(vaddr)) {
				unwritten = copy_to_user(user_data,
							 (void __force *)vaddr + offset,
							 length);
				io_mapping_unmap(vaddr);
			}
		}
		if (unwritten) {
			ret = -EFAULT;
			goto out_put;
		}

		remain -= length;
		user_data += length;
		offset = 0;
	}

out_put:
	intel_runtime_pm_put(rpm, wakeref);
	i915_gem_object_unpin_pages(obj);

	return ret;
}

static int lmem_pwrite(struct drm_i915_gem_object *obj,
		       const struct drm_i915_gem_pwrite *arg)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct intel_runtime_pm *rpm = &i915->runtime_pm;
	intel_wakeref_t wakeref;
	char __user *user_data;
	unsigned int offset;
	unsigned long idx;
	u64 remain;
	int ret;

	ret = i915_gem_object_wait(obj,
				   I915_WAIT_INTERRUPTIBLE,
				   MAX_SCHEDULE_TIMEOUT);
	if (ret)
		return ret;

	ret = i915_ww_pin_lock_interruptible(obj);
	if (ret)
		return ret;

	wakeref = intel_runtime_pm_get(rpm);

	remain = arg->size;
	user_data = u64_to_user_ptr(arg->data_ptr);
	offset = offset_in_page(arg->offset);
	for (idx = arg->offset >> PAGE_SHIFT; remain; idx++) {
		unsigned long unwritten;
		void __iomem *vaddr;
		int length;

		length = remain;
		if (offset + length > PAGE_SIZE)
			length = PAGE_SIZE - offset;

		vaddr = i915_gem_object_lmem_io_map_page_atomic(obj, idx);
		if (!vaddr) {
			ret = -ENOMEM;
			goto out_put;
		}

		unwritten = __copy_from_user_inatomic_nocache((void __force *)vaddr + offset,
							      user_data, length);
		io_mapping_unmap_atomic(vaddr);
		if (unwritten) {
			vaddr = i915_gem_object_lmem_io_map_page(obj, idx);
			if (!IS_ERR_OR_NULL(vaddr)) {
				unwritten = copy_from_user((void __force *)vaddr + offset,
							   user_data, length);
				io_mapping_unmap(vaddr);
			}
		}
		if (unwritten) {
			ret = -EFAULT;
			goto out_put;
		}

		remain -= length;
		user_data += length;
		offset = 0;
	}

out_put:
	intel_runtime_pm_put(rpm, wakeref);
	i915_gem_object_unpin_pages(obj);

	return ret;
}

const struct drm_i915_gem_object_ops i915_gem_lmem_obj_ops = {
	.name = "i915_gem_object_lmem",
	.flags = I915_GEM_OBJECT_HAS_IOMEM,

	.get_pages = lmem_get_pages,
	.put_pages = lmem_put_pages,
	.release = i915_gem_object_release_memory_region,

	.pread = lmem_pread,
	.pwrite = lmem_pwrite,
};

void __iomem *
i915_gem_object_lmem_io_map_page(struct drm_i915_gem_object *obj,
				 unsigned long n)
{
	resource_size_t offset;
	int err;

	err = i915_gem_object_migrate_sync(obj);
	if (err)
		return IO_ERR_PTR(err);

	offset = i915_gem_object_get_dma_address(obj, n);
	offset -= obj->mm.region->region.start;

	return io_mapping_map_wc(&obj->mm.region->iomap, offset, PAGE_SIZE);
}

struct drm_i915_gem_object *
i915_gem_object_create_lmem(struct drm_i915_private *i915,
			    resource_size_t size,
			    unsigned int flags)
{
	return i915_gem_object_create_region(to_gt(i915)->lmem, size, flags);
}

int __i915_gem_lmem_object_init(struct intel_memory_region *mem,
				struct drm_i915_gem_object *obj,
				resource_size_t size,
				unsigned int flags)
{
	static struct lock_class_key lock_class;
	struct drm_i915_private *i915 = mem->i915;

	drm_gem_private_object_init(&i915->drm, &obj->base, size);
	i915_gem_object_init(obj, &i915_gem_lmem_obj_ops, &lock_class, flags);

	obj->read_domains = I915_GEM_DOMAIN_WC | I915_GEM_DOMAIN_GTT;

	i915_gem_object_set_cache_coherency(obj, I915_CACHE_NONE);

	i915_gem_object_init_memory_region(obj, mem);

	return 0;
}

int i915_gem_clear_all_lmem(struct intel_gt *gt, struct drm_printer *p)
{
	struct i915_request *last = NULL;
	struct intel_memory_region *mr;
	struct intel_context *ce;
	struct sg_table *pages;
	intel_wakeref_t wf;
	u64 cycles, bytes;
	int err = 0;
	int i;

	mr = gt->lmem;
	if (!mr)
		return 0;

	pages = kmalloc(sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	if (sg_alloc_table(pages, 1, GFP_KERNEL)) {
		err = -ENOMEM;
		goto err_pages;
	}

	wf = intel_gt_pm_get(gt);
	intel_rps_boost(&gt->rps);

	ce = get_blitter_context(gt, BCS0);
	if (!ce) {
		err = -EIO;
		goto err_wf;
	}

	cycles = -READ_ONCE(gt->counters.map[INTEL_GT_CLEAR_CYCLES]);
	bytes = -READ_ONCE(gt->counters.map[INTEL_GT_CLEAR_BYTES]);

	mutex_lock(&mr->mm_lock);
	for (i = mr->mm.max_order; i >= 0; i--) {
		struct i915_buddy_block *block;

		list_for_each_entry(block, &mr->mm.free_list[i], link) {
			u64 sz = i915_buddy_block_size(&mr->mm, block);
			u64 offset = i915_buddy_block_offset(block);
			struct i915_request *rq;

			while (sz) { /* sg_dma_len() is unsigned int */
				u64 len = min_t(u64, sz, SZ_2G);

				sg_dma_address(pages->sgl) = offset;
				sg_dma_len(pages->sgl) = len;
				GEM_BUG_ON(!sg_is_last(pages->sgl));

				err = clear_blt(ce, pages, len, 0, &rq);
				if (rq) {
					i915_sw_fence_complete(&rq->submit);
					if (last)
						i915_request_put(last);
					last = rq;
				}
				if (err)
					goto unlock;

				sz -= len;
				offset += len;
			}
		}
	}
unlock:
	if (last) {
		i915_request_wait(last, 0, MAX_SCHEDULE_TIMEOUT);
		if (err == 0)
			err = last->fence.error;
		i915_request_put(last);
	}
	mutex_unlock(&mr->mm_lock);

	bytes += READ_ONCE(gt->counters.map[INTEL_GT_CLEAR_BYTES]);
	cycles += READ_ONCE(gt->counters.map[INTEL_GT_CLEAR_CYCLES]);
	cycles = intel_gt_clock_interval_to_ns(gt, cycles);
	if (err == 0 && cycles && p)
		drm_printf(p, "%s%d, cleared %lluMiB in %lldms, %lldMiB/s\n",
			   gt->name, gt->info.id,
			   bytes >> 20,
			   div_u64(cycles, NSEC_PER_MSEC),
			   div64_u64(mul_u64_u32_shr(bytes, NSEC_PER_SEC, 20), cycles));

err_wf:
	intel_rps_cancel_boost(&gt->rps);
	intel_gt_pm_put(gt, wf);
	sg_free_table(pages);
err_pages:
	kfree(pages);
	return err;
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftests/i915_gem_lmem.c"
#endif
