// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include "i915_drv.h"
#include "gt/intel_context.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_flat_ppgtt_pool.h"
#include "gt/intel_gpu_commands.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_buffer_pool.h"
#include "gt/intel_gt_compression_formats.h"
#include "gt/intel_ring.h"
#include "i915_gem_clflush.h"
#include "i915_gem_object_blt.h"

static size_t calc_ccs_size(struct drm_i915_private *i915,
			    struct drm_i915_gem_object *obj)
{
	/*
	 * We only calculate space for CCS for objects resident in Local
	 * Memory only
	 *
	 * The CCS size is (1/256)th the size of the object
	 */
	if (HAS_FLAT_CCS(i915))
		return obj->base.size >> 8;

	return 0;
}

static phys_addr_t calc_ctrl_surf_instr_size(struct drm_i915_private *i915,
					     struct i915_vma *src)
{
	phys_addr_t total_size, ccs_size;
	int num_cmds, num_blks;

	/* CCS size is (1/256)th the object size */
	ccs_size = calc_ccs_size(i915, src->obj);
	if (!ccs_size)
		return 0;

	/*
	 * XY_CTRL_SURF_COPY_BLT transfers CCS in 256 byte
	 * blocks.
	 */
	num_blks = (ccs_size + (NUM_CCS_BYTES_PER_BLOCK - 1)) >> 8;

	/*
	 * 1 XY_CTRL_SURF_COPY_BLT command can trnasfer upto
	 * 1024 such 256 byte blocks.
	 */
	num_cmds = (num_blks + (NUM_CCS_BLKS_PER_XFER - 1)) >> 10;

	/*
	 * Each XY_CTRL_SURF_COPY_BLT command is 20 bytes in
	 * size.
	 */
	total_size = (XY_CTRL_SURF_INSTR_SIZE * sizeof(u32)) * num_cmds;

	/*
	 * We will also need to add a MI_FLUSH_DW before all the
	 * XY_CTRL_SURF_COPY_BLT commands for compatibility with
	 * legacy commands
	 */
	total_size += 2 * (MI_FLUSH_DW_SIZE * sizeof(u32));

	/*
	 * Wa_1409498409: xehpsdv
	 * Account for the extra flush in intel_emit_vma_fill_blt()
	 */
	if (IS_XEHPSDV(i915))
		total_size += MI_FLUSH_DW_SIZE;

	return total_size;
}

static u32 *_i915_ctrl_surf_copy_blt(u32 *cmd, u64 src_addr, u64 dst_addr,
				     u8 src_mem_access, u8 dst_mem_access,
				     int src_mocs, int dst_mocs,
				     u16 num_ccs_blocks)
{
	u32 src_mocs_field = FIELD_PREP(XY_CSC_BLT_MOCS_INDEX_MASK, src_mocs);
	u32 dst_mocs_field = FIELD_PREP(XY_CSC_BLT_MOCS_INDEX_MASK, dst_mocs);
	int i = num_ccs_blocks;

	/*
	 * The XY_CTRL_SURF_COPY_BLT instruction is used to copy the CCS
	 * data in and out of the CCS region.
	 *
	 * We can copy at most 1024 blocks of 256 bytes using one
	 * XY_CTRL_SURF_COPY_BLT instruction.
	 *
	 * In case we need to copy more than 1024 blocks, we need to add
	 * another instruction to the same batch buffer. This is done in
	 * a loop here.
	 *
	 * 1024 blocks of 256 bytes of CCS represent total 256KB of CCS.
	 *
	 * 256 KB of CCS represents 256 * 256 KB = 64 MB of LMEM.
	 *
	 * So, after every iteration, we advance the src and dst
	 * addresses by 64 MB
	 */
	do {
		/*
		 * We use logical AND with 1023 since the size field
		 * takes values which is in the range of 0 - 1023
		 */
		*cmd++ = ((XY_CTRL_SURF_COPY_BLT) |
			  (src_mem_access << SRC_ACCESS_TYPE_SHIFT) |
			  (dst_mem_access << DST_ACCESS_TYPE_SHIFT) |
			  (((i - 1) & 1023) << CCS_SIZE_SHIFT));
		*cmd++ = lower_32_bits(src_addr);
		*cmd++ = ((upper_32_bits(src_addr) & 0xFFFF) | src_mocs_field);
		*cmd++ = lower_32_bits(dst_addr);
		*cmd++ = ((upper_32_bits(dst_addr) & 0xFFFF) | dst_mocs_field);

		src_addr += SZ_64M;
		dst_addr += SZ_64M;
		i -= NUM_CCS_BLKS_PER_XFER;
	} while (i > 0);

	return cmd;
}

struct i915_vma *intel_emit_vma_fill_blt(struct intel_context *ce,
					 struct i915_vma *vma,
					 struct i915_gem_ww_ctx *ww,
					 u32 value)
{
	struct drm_i915_private *i915 = ce->vm->i915;
	struct intel_gt *gt = ce->engine->gt;
	u32 block_size, stateless_comp = 0;
	struct intel_gt_buffer_pool_node *pool;
	struct i915_vma *batch;
	size_t ccs_size = calc_ccs_size(i915, vma->obj);
	u64 offset;
	u64 count;
	u64 rem;
	u64 size;
	u32 *cmd;
	u16 num_ccs_blks = (ccs_size + NUM_CCS_BYTES_PER_BLOCK - 1) >> 8;
	int err;

	GEM_BUG_ON(HAS_LINK_COPY_ENGINES(i915) && value > 255);
	GEM_BUG_ON(intel_engine_is_virtual(ce->engine));
	intel_engine_pm_get(ce->engine);

	if (HAS_LINK_COPY_ENGINES(i915))
		block_size = SZ_256K; /* PVC_MEM_SET has 18 bits for size */
	else
		block_size = SZ_8M; /* ~1ms at 8GiB/s preemption delay */

	count = div_u64(round_up(vma->size, block_size), block_size);
	if (HAS_LINK_COPY_ENGINES(i915))
		size = (1 + 8 * count) * sizeof(u32);
	else if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 50))
		size = (1 + 17 * count) * sizeof(u32);
	else if (GRAPHICS_VER(i915) >= 12)
		size = (1 + 12 * count) * sizeof(u32);
	else
		size = (1 + 8 * count) * sizeof(u32);

	/*
	 * For stateless compression we set compressible if LMEM, and note,
	 * hardware will handle the clearing of CCS.
	 */
	if (HAS_STATELESS_MC(i915) && i915_gem_object_is_lmem(vma->obj))
		stateless_comp =
			PVC_MEM_SET_DST_COMPRESSIBLE |
			PVC_MEM_SET_DST_COMPRESS_EN |
			FIELD_PREP(PVC_MEM_SET_COMPRESSION_FMT, XEHPC_LINEAR_16);

	/*
	 * Whenever the intel_emit_vma_fill_blt() function is used with
	 * the value to be filled in the BO as zero, we check if the BO
	 * is located in LMEM only and if it is, we zero out the
	 * contents of the CCS associated with the BO.
	 *
	 * We always pass in the source vma as the second argument since
	 * we want to calculate the size of the CCS of the source
	 * object.
	 */
	if (!value && !stateless_comp)
		size += calc_ctrl_surf_instr_size(i915, vma);

	size = round_up(size, PAGE_SIZE);

	pool = intel_gt_get_buffer_pool(gt, size, I915_MAP_WC);
	if (IS_ERR(pool)) {
		err = PTR_ERR(pool);
		goto out_pm;
	}

	err = i915_gem_object_lock(pool->obj, ww);
	if (err)
		goto out_put;

	batch = i915_vma_instance(pool->obj, ce->vm, NULL);
	if (IS_ERR(batch)) {
		err = PTR_ERR(batch);
		goto out_put;
	}

	err = i915_vma_pin_ww(batch, ww, 0, 0, PIN_USER | PIN_ZONE_48);
	if (unlikely(err))
		goto out_put;

	/* we pinned the pool, mark it as such */
	intel_gt_buffer_pool_mark_used(pool);

	cmd = i915_gem_object_pin_map(pool->obj, pool->type);
	if (IS_ERR(cmd)) {
		err = PTR_ERR(cmd);
		goto out_unpin;
	}

	rem = vma->size;
	offset = i915_vma_offset(vma);

	do {
		u32 size = min_t(u64, rem, block_size);

		GEM_BUG_ON(size >> PAGE_SHIFT > S16_MAX);

		if (HAS_LINK_COPY_ENGINES(i915)) {
			u32 mocs = FIELD_PREP(MS_MOCS_INDEX_MASK,
					      gt->mocs.uc_index);

			*cmd++ = PVC_MEM_SET_CMD | stateless_comp | (7 - 2);
			*cmd++ = size - 1;
			*cmd++ = 0;
			*cmd++ = 0;
			*cmd++ = lower_32_bits(offset);
			*cmd++ = upper_32_bits(offset);
			/* Value is Bit 31:24 */
			*cmd++ = value << 24 | mocs;
		} else if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 50)) {
			u32 mocs = FIELD_PREP(XY_FCB_MOCS_INDEX_MASK,
					      gt->mocs.uc_index);
			u8 mem_type;

			/* Wa to set the target memory region as system */
			if (IS_XEHPSDV_GRAPHICS_STEP(i915, STEP_A0, STEP_B0))
				mem_type = MEM_TYPE_SYS;
			else
				mem_type = i915_gem_object_is_lmem(vma->obj)?
					MEM_TYPE_LOCAL : MEM_TYPE_SYS;

			*cmd++ = XY_FAST_COLOR_BLT | BLT_COLOR_DEPTH_32 | (16 - 2);
			*cmd++ = mocs | (PAGE_SIZE - 1);
			*cmd++ = 0;
			*cmd++ = size >> PAGE_SHIFT << 16 | PAGE_SIZE / 4;
			*cmd++ = lower_32_bits(offset);
			*cmd++ = upper_32_bits(offset);
			*cmd++ = mem_type << 31;
			/* BG7 */
			*cmd++ = value;
			*cmd++ = 0;
			*cmd++ = 0;
			*cmd++ = 0;
			/* BG11 */
			*cmd++ = 0;
			*cmd++ = 0;
			/* BG13 */
			*cmd++ = 0;
			*cmd++ = 0;
			*cmd++ = 0;
		} else if (GRAPHICS_VER(i915) >= 12) {
			*cmd++ = XY_FAST_COLOR_BLT | BLT_COLOR_DEPTH_32 | (11 - 2);
			*cmd++ = PAGE_SIZE - 1;
			*cmd++ = 0;
			*cmd++ = size >> PAGE_SHIFT << 16 | PAGE_SIZE / 4;
			*cmd++ = lower_32_bits(offset);
			*cmd++ = upper_32_bits(offset);
			*cmd++ = 0;
			*cmd++ = value;
			*cmd++ = 0;
			*cmd++ = 0;
			*cmd++ = 0;
		} else if (GRAPHICS_VER(i915) >= 8) {
			*cmd++ = XY_COLOR_BLT_CMD | BLT_WRITE_RGBA | (7 - 2);
			*cmd++ = BLT_DEPTH_32 | BLT_ROP_COLOR_COPY | PAGE_SIZE;
			*cmd++ = 0;
			*cmd++ = size >> PAGE_SHIFT << 16 | PAGE_SIZE / 4;
			*cmd++ = lower_32_bits(offset);
			*cmd++ = upper_32_bits(offset);
			*cmd++ = value;
		} else {
			*cmd++ = XY_COLOR_BLT_CMD | BLT_WRITE_RGBA | (6 - 2);
			*cmd++ = BLT_DEPTH_32 | BLT_ROP_COLOR_COPY | PAGE_SIZE;
			*cmd++ = 0;
			*cmd++ = size >> PAGE_SHIFT << 16 | PAGE_SIZE / 4;
			*cmd++ = offset;
			*cmd++ = value;
		}

		/* Allow ourselves to be preempted in between blocks. */
		*cmd++ = MI_ARB_CHECK;

		offset += size;
		rem -= size;
	} while (rem);

	/*
	 * We only update the CCS if the BO is located in LMEM only and
	 * the value to be filled in the BO is all zeroes
	 */
	if (HAS_FLAT_CCS(i915) && !value && !stateless_comp) {
		/* Wa_1409498409: xehpsdv */
		if (IS_XEHPSDV(i915)) {
			cmd = i915_flush_dw(cmd, vma, MI_FLUSH_LLC);
			cmd = i915_flush_dw(cmd, vma, MI_FLUSH_CCS);
		} else {
			cmd = i915_flush_dw(cmd, vma,
					     MI_FLUSH_LLC | MI_FLUSH_CCS);
		}

		/*
		 * The src and dst addresses are same here since we will
		 * copy the zeroed out pages to the CCS of themselves in
		 * order to clear the CCS associated with the pages
		 */
		cmd = _i915_ctrl_surf_copy_blt(cmd,
					       i915_vma_offset(vma),
					       i915_vma_offset(vma),
					       DIRECT_ACCESS, INDIRECT_ACCESS,
					       gt->mocs.uc_index,
					       gt->mocs.uc_index,
					       num_ccs_blks);
		cmd = i915_flush_dw(cmd, vma, MI_FLUSH_LLC | MI_FLUSH_CCS);
	}

	*cmd = MI_BATCH_BUFFER_END;

	i915_gem_object_flush_map(pool->obj);
	i915_gem_object_unpin_map(pool->obj);

	intel_gt_chipset_flush(gt);

	batch->private = pool;
	return batch;

out_unpin:
	i915_vma_unpin(batch);
out_put:
	intel_gt_buffer_pool_put(pool);
out_pm:
	intel_engine_pm_put(ce->engine);
	return ERR_PTR(err);
}

int intel_emit_vma_mark_active(struct i915_vma *vma, struct i915_request *rq)
{
	int err;

	err = i915_request_await_object(rq, vma->obj, false);
	if (err == 0)
		err = i915_vma_move_to_active(vma, rq, 0);
	if (unlikely(err))
		return err;

	return intel_gt_buffer_pool_mark_active(vma->private, rq);
}

void intel_emit_vma_release(struct intel_context *ce, struct i915_vma *vma)
{
	i915_vma_unpin(vma);
	intel_gt_buffer_pool_put(vma->private);
	intel_engine_pm_put(ce->engine);
}

static int
move_obj_to_gpu(struct drm_i915_gem_object *obj,
		struct i915_request *rq,
		bool write, bool nowait)
{
	if (obj->cache_dirty & ~obj->cache_coherent)
		i915_gem_clflush_object(obj, 0);

	return nowait ? 0 : i915_request_await_object(rq, obj, write);
}

int i915_gem_object_ww_fill_blt(struct drm_i915_gem_object *obj,
				struct i915_gem_ww_ctx *ww,
				struct intel_context *ce,
				u32 value)
{
	struct i915_request *rq;
	struct i915_vma *batch;
	struct i915_vma *vma;
	int err;

	vma = i915_vma_instance(obj, ce->vm, NULL);
	if (IS_ERR(vma))
		return PTR_ERR(vma);

	intel_engine_pm_get(ce->engine);
	err = intel_context_pin_ww(ce, ww);
	if (err)
		goto out;

	err = i915_vma_pin_ww(vma, ww, 0, 0, PIN_USER);
	if (err)
		goto out_ctx;

	batch = intel_emit_vma_fill_blt(ce, vma, ww, value);
	if (IS_ERR(batch)) {
		err = PTR_ERR(batch);
		goto out_vma;
	}

	rq = i915_request_create(ce);
	if (IS_ERR(rq)) {
		err = PTR_ERR(rq);
		goto out_batch;
	}

	err = intel_emit_vma_mark_active(batch, rq);
	if (unlikely(err))
		goto out_request;

	err = move_obj_to_gpu(vma->obj, rq, true, false);
	if (err == 0)
		err = i915_vma_move_to_active(vma, rq, EXEC_OBJECT_WRITE);
	if (unlikely(err))
		goto out_request;

	if (ce->engine->emit_init_breadcrumb)
		err = ce->engine->emit_init_breadcrumb(rq);

	if (likely(!err))
		err = ce->engine->emit_bb_start(rq,
						i915_vma_offset(batch),
						i915_vma_size(batch),
						0);
out_request:
	if (unlikely(err))
		i915_request_set_error_once(rq, err);

	i915_request_add(rq);
out_batch:
	i915_gem_ww_unlock_single(batch->obj);
	intel_emit_vma_release(ce, batch);
out_vma:
	/* Rollback on error */
	intel_flat_ppgtt_request_pool_clean(vma);
	i915_vma_unpin(vma);
out_ctx:
	intel_context_unpin(ce);
out:
	intel_engine_pm_put(ce->engine);
	return err;
}

int i915_gem_object_fill_blt(struct drm_i915_gem_object *obj,
				struct intel_context *ce,
				u32 value)
{
	struct i915_gem_ww_ctx ww;
	int err;

	i915_gem_ww_ctx_init(&ww, true);
retry:
	err = i915_gem_object_lock(obj, &ww);
	if (err)
		goto out_err;

	err = i915_gem_object_ww_fill_blt(obj, &ww, ce, value);
out_err:
	if (err == -EDEADLK) {
		err = i915_gem_ww_ctx_backoff(&ww);
		if (!err)
			goto retry;
	}
	i915_gem_ww_ctx_fini(&ww);

	return err;
}


/* Wa_1209644611:icl,ehl */
static bool wa_1209644611_applies(struct drm_i915_private *i915, u32 size)
{
	u32 height = size >> PAGE_SHIFT;

	if (GRAPHICS_VER(i915) != 11)
		return false;

	return height % 4 == 3 && height <= 8;
}

struct i915_vma *intel_emit_vma_copy_blt(struct intel_context *ce,
					 struct i915_gem_ww_ctx *ww,
					 struct i915_vma *src,
					 struct i915_vma *dst)
{
	struct drm_i915_private *i915 = ce->vm->i915;
	struct intel_gt *gt = ce->engine->gt;
	struct intel_gt_buffer_pool_node *pool;
	struct i915_vma *batch;
	u64 src_offset, dst_offset;
	u64 count, rem;
	u32 block_size;
	u32 size, *cmd;
	int err;

	GEM_BUG_ON(src->size > dst->size);

	GEM_BUG_ON(intel_engine_is_virtual(ce->engine));

	if (IS_PONTEVECCHIO(i915)) /* PVC_MEM_COPY has 18 bits for size */
		block_size = SZ_256K;
	else if (IS_XEHPSDV(i915))
		block_size = SZ_16K;
	else /* ~1ms at 8GiB/s preemption delay */
		block_size = SZ_8M;

	intel_engine_pm_get(ce->engine);
	count = div_u64(round_up(dst->size, block_size), block_size);

	/*
	 * BLOCK_COPY_CMD in linear mode supports max size of 16k
	 */
	if (IS_XEHPSDV(i915))
		size = (1 + 23 * count) * sizeof(u32);
	else
		size = (1 + 11 * count) * sizeof(u32);

	size = round_up(size, PAGE_SIZE);
	pool = intel_gt_get_buffer_pool(gt, size, I915_MAP_WC);
	if (IS_ERR(pool)) {
		err = PTR_ERR(pool);
		goto out_pm;
	}

	err = i915_gem_object_lock(pool->obj, ww);
	if (err)
		goto out_put;

	batch = i915_vma_instance(pool->obj, ce->vm, NULL);
	if (IS_ERR(batch)) {
		err = PTR_ERR(batch);
		goto out_put;
	}

	err = i915_vma_pin_ww(batch, ww, 0, 0, PIN_USER | PIN_ZONE_48);
	if (unlikely(err))
		goto out_put;

	/* we pinned the pool, mark it as such */
	intel_gt_buffer_pool_mark_used(pool);

	cmd = i915_gem_object_pin_map(pool->obj, pool->type);
	if (IS_ERR(cmd)) {
		err = PTR_ERR(cmd);
		goto out_unpin;
	}

	rem = src->size;
	src_offset = i915_vma_offset(src);
	dst_offset = i915_vma_offset(dst);

	do {
		size = min_t(u64, rem, block_size);
		GEM_BUG_ON(size >> PAGE_SHIFT > S16_MAX);

		if (IS_XEHPSDV(i915)) {
			u8 src_mem_type, dst_mem_type;
			u32 mocs = FIELD_PREP(XY_BCB_MOCS_INDEX_MASK, gt->mocs.uc_index);

			/* Wa_14010828422:xehpsdv set target memory region to smem */
			if (IS_XEHPSDV_GRAPHICS_STEP(i915, STEP_A0, STEP_B0)) {
				src_mem_type = MEM_TYPE_SYS;
				dst_mem_type = MEM_TYPE_SYS;
			} else {
				src_mem_type = i915_gem_object_is_lmem(src->obj) ?
					       MEM_TYPE_LOCAL : MEM_TYPE_SYS;
				dst_mem_type = i915_gem_object_is_lmem(dst->obj) ?
					       MEM_TYPE_LOCAL : MEM_TYPE_SYS;
			}

			*cmd++ = XY_BLOCK_COPY_BLT_CMD | (22 - 2);
			*cmd++ = mocs | (size - 1);
			*cmd++ = 0;
			/* BG3 */
			*cmd++ = (1 << 16) | (size);
			*cmd++ = lower_32_bits(dst_offset);
			*cmd++ = upper_32_bits(dst_offset);
			/* BG6 */
			*cmd++ = dst_mem_type << DEST_MEM_TYPE_SHIFT;
			*cmd++ = 0;
			/* BG8 */
			*cmd++ = mocs | (size - 1);
			*cmd++ = lower_32_bits(src_offset);
			*cmd++ = upper_32_bits(src_offset);
			 /* BG 11 */
			*cmd++ = src_mem_type << SRC_MEM_TYPE_SHIFT;
			cmd += 4;
			*cmd++ = 0;
			*cmd++ = 0;
			*cmd++ = 0;
			*cmd++ = 0;
			*cmd++ = 0;
		} else if (HAS_LINK_COPY_ENGINES(i915)) {
			u32 src_mocs = FIELD_PREP(MC_SRC_MOCS_INDEX_MASK,
						  gt->mocs.uc_index);
			u32 dst_mocs = FIELD_PREP(MC_DST_MOCS_INDEX_MASK,
						  gt->mocs.uc_index);
			u32 comp_bits = 0;

			/* for stateless compression we mark compressible if LMEM */
			if (HAS_STATELESS_MC(i915)) {
				comp_bits = FIELD_PREP(PVC_MEM_COPY_COMPRESSION_FMT,
						       XEHPC_LINEAR_16);

				if (i915_gem_object_is_lmem(dst->obj))
					comp_bits |=
						PVC_MEM_COPY_DST_COMPRESSIBLE |
						PVC_MEM_COPY_DST_COMPRESS_EN;

				if (i915_gem_object_is_lmem(src->obj))
					comp_bits |= PVC_MEM_COPY_SRC_COMPRESSIBLE;
			}

			*cmd++ = PVC_MEM_COPY_CMD | comp_bits | (10 - 2);
			*cmd++ = size - 1;
			*cmd++ = 0;
			*cmd++ = 0;
			*cmd++ = 0;
			*cmd++ = lower_32_bits(src_offset);
			*cmd++ = upper_32_bits(src_offset);
			*cmd++ = lower_32_bits(dst_offset);
			*cmd++ = upper_32_bits(dst_offset);
			*cmd++ = src_mocs | dst_mocs;
		} else if (GRAPHICS_VER(i915) >= 9 &&
			   !wa_1209644611_applies(i915, size)) {
			*cmd++ = GEN9_XY_FAST_COPY_BLT_CMD | (10 - 2);
			*cmd = BLT_DEPTH_32 | PAGE_SIZE;
			/* Wa_14010828422:xehpsdv */
			if (IS_XEHPSDV_GRAPHICS_STEP(i915, STEP_A0, STEP_B0))
				*cmd |= BLT_SRCMEM_SYS;
			cmd++;
			*cmd++ = 0;
			*cmd++ = size >> PAGE_SHIFT << 16 | PAGE_SIZE / 4;
			*cmd++ = lower_32_bits(dst_offset);
			*cmd++ = upper_32_bits(dst_offset);
			*cmd++ = 0;
			*cmd++ = PAGE_SIZE;
			*cmd++ = lower_32_bits(src_offset);
			*cmd++ = upper_32_bits(src_offset);
		} else if (GRAPHICS_VER(i915) >= 8) {
			*cmd++ = XY_SRC_COPY_BLT_CMD | BLT_WRITE_RGBA | (10 - 2);
			*cmd++ = BLT_DEPTH_32 | BLT_ROP_SRC_COPY | PAGE_SIZE;
			*cmd++ = 0;
			*cmd++ = size >> PAGE_SHIFT << 16 | PAGE_SIZE / 4;
			*cmd++ = lower_32_bits(dst_offset);
			*cmd++ = upper_32_bits(dst_offset);
			*cmd++ = 0;
			*cmd++ = PAGE_SIZE;
			*cmd++ = lower_32_bits(src_offset);
			*cmd++ = upper_32_bits(src_offset);
		} else {
			*cmd++ = SRC_COPY_BLT_CMD | BLT_WRITE_RGBA | (6 - 2);
			*cmd++ = BLT_DEPTH_32 | BLT_ROP_SRC_COPY | PAGE_SIZE;
			*cmd++ = size >> PAGE_SHIFT << 16 | PAGE_SIZE;
			*cmd++ = dst_offset;
			*cmd++ = PAGE_SIZE;
			*cmd++ = src_offset;
		}

		/* Allow ourselves to be preempted in between blocks. */
		*cmd++ = MI_ARB_CHECK;

		src_offset += size;
		dst_offset += size;
		rem -= size;
	} while (rem);

	*cmd = MI_BATCH_BUFFER_END;

	i915_gem_object_flush_map(pool->obj);
	i915_gem_object_unpin_map(pool->obj);

	intel_gt_chipset_flush(gt);
	batch->private = pool;
	return batch;

out_unpin:
	i915_vma_unpin(batch);
out_put:
	intel_gt_buffer_pool_put(pool);
out_pm:
	intel_engine_pm_put(ce->engine);
	return ERR_PTR(err);
}

static struct i915_vma *
prepare_compressed_copy_cmd_buf(struct intel_context *ce,
				struct i915_gem_ww_ctx *ww,
				struct i915_vma *src,
				struct i915_vma *dst)
{
	struct intel_gt_buffer_pool_node *pool;
	struct i915_vma *batch;
	struct intel_gt *gt = src->vm->gt;
	u64 src_offset, dst_offset;
	u64 count, rem, size;
	u32 *cmd;
	int err;
	u8 src_mem_type, dst_mem_type;
	u32 src_compression, dst_compression;
	u32 src_mocs = FIELD_PREP(XY_BCB_MOCS_INDEX_MASK, gt->mocs.uc_index);
	u32 dst_mocs = FIELD_PREP(XY_BCB_MOCS_INDEX_MASK, gt->mocs.uc_index);
	bool dst_is_lmem = i915_gem_object_is_lmem(dst->obj);

	count = (src->size + SZ_64K - 1)/SZ_64K;
	size = (1 + (4 * 2 + 23) * count) * sizeof(u32);
	size = round_up(size, PAGE_SIZE);

	intel_engine_pm_get(ce->engine);

	pool = intel_gt_get_buffer_pool(ce->engine->gt, size, I915_MAP_WC);
	if (IS_ERR(pool)) {
		err = PTR_ERR(pool);
		goto out_pm;
	}
	err = i915_gem_object_lock(pool->obj, ww);
	if (err)
		goto out_put;

	batch = i915_vma_instance(pool->obj, ce->vm, NULL);
	if (IS_ERR(batch)) {
		err = PTR_ERR(batch);
		goto out_put;
	}

	err = i915_vma_pin_ww(batch, ww, 0, 0, PIN_USER | PIN_ZONE_48);
	if (unlikely(err))
		goto out_put;
		/* we pinned the pool, mark it as such */
	intel_gt_buffer_pool_mark_used(pool);

	cmd = i915_gem_object_pin_map(pool->obj, pool->type);
	if (IS_ERR(cmd)) {
		err = PTR_ERR(cmd);
		goto out_unpin;
	}
	if (dst_is_lmem) {
		src_compression = 0;
		dst_compression =  COMPRESSION_ENABLE | AUX_CCS_E;
		rem = dst->size;
	} else {
		src_compression = COMPRESSION_ENABLE | AUX_CCS_E;
		dst_compression = 0;
		rem = src->size;
	}

	src_mem_type = i915_gem_object_is_lmem(src->obj) ?
		       MEM_TYPE_LOCAL : MEM_TYPE_SYS;
	dst_mem_type = i915_gem_object_is_lmem(dst->obj) ?
		       MEM_TYPE_LOCAL : MEM_TYPE_SYS;
	src_offset = i915_vma_offset(src);
	dst_offset = i915_vma_offset(dst);
	do {
		int block_size = min_t(u64, rem, SZ_64K);
		*cmd++ = XY_BLOCK_COPY_BLT_CMD | 0x14;
		*cmd++ = dst_mocs | dst_compression | TILE_4_FORMAT
				| TILE_4_WIDTH_DWORD;
		*cmd++ = 0;
		/* BG3 */
		*cmd++ = TILE_4_WIDTH | (block_size >> 7) << 16;
		*cmd++ = lower_32_bits(dst_offset);
		*cmd++ = upper_32_bits(dst_offset);
		/* BG6 */
		*cmd++ = dst_mem_type << DEST_MEM_TYPE_SHIFT;
		*cmd++ = 0;
		/* BG8 */
		*cmd++ = src_mocs | src_compression | TILE_4_WIDTH_DWORD | TILE_4_FORMAT;
		*cmd++ = lower_32_bits(src_offset);
		*cmd++ = upper_32_bits(src_offset);
		/* BG 11 */
		*cmd++ = src_mem_type << SRC_MEM_TYPE_SHIFT;
		*cmd++ = 0;
		*cmd++ = 0;
		*cmd++ = 0;
		*cmd++ = 0;
		/* BG 16 */
		*cmd++ = SURFACE_TYPE_2D |
			((TILE_4_WIDTH - 1) << DEST_SURF_WIDTH_SHIFT) |
			(TILE_4_HEIGHT - 1);
		*cmd++ = 0;
		*cmd++ = 0;
		/* BG 19 */
		*cmd++ = SURFACE_TYPE_2D |
			((TILE_4_WIDTH - 1) << SRC_SURF_WIDTH_SHIFT) |
			(TILE_4_HEIGHT - 1);

		*cmd++ = 0;
		*cmd++ = 0;
		cmd = i915_flush_dw(cmd, dst, MI_FLUSH_LLC|MI_INVALIDATE_TLB);
		cmd = i915_flush_dw(cmd, dst, MI_FLUSH_CCS);
		*cmd++ = MI_ARB_CHECK;

		src_offset += block_size;
		dst_offset += block_size;
		rem -= block_size;

	} while (rem);

	*cmd = MI_BATCH_BUFFER_END;

	i915_gem_object_flush_map(pool->obj);
	i915_gem_object_unpin_map(pool->obj);
	intel_gt_chipset_flush(ce->vm->gt);
	batch->private = pool;
	return batch;
out_unpin:
	i915_vma_unpin(batch);
out_put:
	intel_gt_buffer_pool_put(pool);
out_pm:
	intel_engine_pm_put(ce->engine);
	return ERR_PTR(err);
}

static int __i915_gem_object_ww_copy_blt(struct drm_i915_gem_object *src,
				struct drm_i915_gem_object *dst,
				struct i915_gem_ww_ctx *ww,
				struct intel_context *ce,
				bool nowait,
				bool compression)
{
	struct drm_i915_private *i915 = ce->vm->i915;
	struct i915_address_space *vm = ce->vm;
	struct i915_vma *vma[2], *batch;
	struct i915_request *rq;
	int err, i;

	vma[0] = i915_vma_instance(src, vm, NULL);
	if (IS_ERR(vma[0]))
		return PTR_ERR(vma[0]);

	vma[1] = i915_vma_instance(dst, vm, NULL);
	if (IS_ERR(vma[1]))
		return PTR_ERR(vma[1]);

	intel_engine_pm_get(ce->engine);
	err = intel_context_pin_ww(ce, ww);
	if (err)
		goto out;

	err = i915_vma_pin_ww(vma[0], ww, 0, 0, PIN_USER);
	if (err)
		goto out_ctx;

	err = i915_vma_pin_ww(vma[1], ww, 0, 0, PIN_USER);
	if (unlikely(err))
		goto out_unpin_src;
	if (!compression) {
		batch = intel_emit_vma_copy_blt(ce, ww, vma[0], vma[1]);
		if (IS_ERR(batch)) {
			err = PTR_ERR(batch);
			goto out_unpin_dst;
		}
	} else if (HAS_FLAT_CCS(i915)) {
		batch = prepare_compressed_copy_cmd_buf(ce, ww, vma[0], vma[1]);
		if (IS_ERR(batch)) {
			err = PTR_ERR(batch);
			goto out_unpin_dst;
		}
	} else {
		err = -EINVAL;
		goto out_unpin_dst;
	}

	rq = i915_request_create(ce);
	if (IS_ERR(rq)) {
		err = PTR_ERR(rq);
		goto out_batch;
	}

	err = intel_emit_vma_mark_active(batch, rq);
	if (unlikely(err))
		goto out_request;

	for (i = 0; i < ARRAY_SIZE(vma); i++) {
		err = move_obj_to_gpu(vma[i]->obj, rq, i, nowait);
		if (unlikely(err))
			goto out_request;
	}

	for (i = 0; i < ARRAY_SIZE(vma); i++) {
		unsigned int flags = i ? EXEC_OBJECT_WRITE : 0;

		err = i915_vma_move_to_active(vma[i], rq, flags);
		if (unlikely(err))
			goto out_request;
	}

	if (rq->engine->emit_init_breadcrumb) {
		err = rq->engine->emit_init_breadcrumb(rq);
		if (unlikely(err))
			goto out_request;
	}

	err = rq->engine->emit_bb_start(rq,
					i915_vma_offset(batch),
					i915_vma_size(batch),
					0);

out_request:
	if (unlikely(err))
		i915_request_set_error_once(rq, err);

	i915_request_add(rq);
out_batch:
	i915_gem_ww_unlock_single(batch->obj);
	intel_emit_vma_release(ce, batch);
out_unpin_dst:
	/* Rollback on error */
	intel_flat_ppgtt_request_pool_clean(vma[1]);
	i915_vma_unpin(vma[1]);
out_unpin_src:
	intel_flat_ppgtt_request_pool_clean(vma[0]);
	i915_vma_unpin(vma[0]);
out_ctx:
	intel_context_unpin(ce);
out:
	intel_engine_pm_put(ce->engine);
	return err;
}

int i915_gem_object_ww_copy_blt(struct drm_i915_gem_object *src,
				struct drm_i915_gem_object *dst,
				struct i915_gem_ww_ctx *ww,
				struct intel_context *ce,
				bool nowait)
{
	return __i915_gem_object_ww_copy_blt(src, dst, ww, ce, nowait, false);
}

int i915_gem_object_ww_compressed_copy_blt(struct drm_i915_gem_object *src,
				struct drm_i915_gem_object *dst,
				struct i915_gem_ww_ctx *ww,
				struct intel_context *ce,
				bool nowait)
{
	return __i915_gem_object_ww_copy_blt(src, dst, ww, ce, nowait, true);
}

int i915_gem_object_copy_blt(struct drm_i915_gem_object *src,
			     struct drm_i915_gem_object *dst,
			     struct intel_context *ce,
			     bool nowait)
{
	struct i915_gem_ww_ctx ww;
	int err;

	i915_gem_ww_ctx_init(&ww, true);
retry:
	err = i915_gem_object_lock(src, &ww);
	if (err)
		goto out_err;

	err = i915_gem_object_lock(dst, &ww);
	if (err)
		goto out_err;

	err = i915_gem_object_ww_copy_blt(src, dst, &ww, ce, nowait);
out_err:
	if (err == -EDEADLK) {
		err = i915_gem_ww_ctx_backoff(&ww);
		if (!err)
			goto retry;
	}
	i915_gem_ww_ctx_fini(&ww);

	return err;
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftests/i915_gem_object_blt.c"
#endif
