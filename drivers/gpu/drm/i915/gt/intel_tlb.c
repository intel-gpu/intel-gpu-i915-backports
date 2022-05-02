// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

#include "i915_drv.h"
#include "i915_perf_oa_regs.h"
#include "intel_gt_regs.h"
#include "intel_tlb.h"
#include "i915_trace.h"

#define GEN12_GFX_TLB_INV_CR			_MMIO(0xced8)
#define   GEN12_GFX_TLB_INV_CR_INVALIDATE	(1 << 0)
#define GEN12_BLT_TLB_INV_CR			_MMIO(0xcee4)
#define   GEN12_BLT_TLB_INV_CR_INVALIDATE       (1 << 0)
#define GEN12_VCS_TLB_INV_CR			_MMIO(0xcedc)
#define   GEN12_VCS_TLB_INV_CR_INVALIDATE(n)    (1 << (n))
#define GEN12_VECS_TLB_INV_CR			_MMIO(0xcee0)
#define   GEN12_VECS_TLB_INV_CR_INVALIDATE(n)    (1 << (n))
#define GEN12_GUC_TLB_INV_CR			_MMIO(0xcee8)
#define   GEN12_GUC_TLB_INV_CR_INVALIDATE	(1 << 0)
#define GEN12_OA_TLB_INV_CR			_MMIO(0xceec)
#define   GEN12_OA_TLB_INV_CR_INVALIDATE	(1 << 0)


struct tlb_inv_cr {
	i915_reg_t      reg;
	u32		val;
};

static int get_tlb_inv_cr(struct intel_engine_cs *engine, struct tlb_inv_cr *cr)
{
	switch (engine->class) {
	case RENDER_CLASS:
	case COMPUTE_CLASS:
		cr->reg = GEN12_GFX_TLB_INV_CR;
		cr->val = GEN12_GFX_TLB_INV_CR_INVALIDATE;
		break;
	case VIDEO_DECODE_CLASS:
		cr->reg = GEN12_VCS_TLB_INV_CR;
		cr->val = GEN12_VCS_TLB_INV_CR_INVALIDATE(engine->instance);
		break;
	case VIDEO_ENHANCEMENT_CLASS:
		cr->reg = GEN12_VECS_TLB_INV_CR;
		cr->val = GEN12_VECS_TLB_INV_CR_INVALIDATE(engine->instance);
		break;
	case COPY_ENGINE_CLASS:
		cr->reg = GEN12_BLT_TLB_INV_CR;
		cr->val = GEN12_BLT_TLB_INV_CR_INVALIDATE;
		break;
	case OTHER_CLASS:
		cr->reg = GEN12_GUC_TLB_INV_CR;
		cr->val = GEN12_GUC_TLB_INV_CR_INVALIDATE;
		break;
	default:
		drm_WARN_ON(&engine->i915->drm, 1);
		return -EINVAL;
	}

	return 0;
}

static void invalidate_tlb_engine_flush(struct intel_engine_cs *engine)
{
	struct intel_uncore *uncore = engine->uncore;
	struct tlb_inv_cr tlb_inv_cr;
	int err = 0;
	struct intel_gt *gt = engine->gt;
	u64 rstcnt = atomic_read(&gt->reset.engines_reset_count);

	err = get_tlb_inv_cr(engine, &tlb_inv_cr);
	if (unlikely(err))
		return;

	/*
	 * HW architecture suggest typical invalidation time at 40us,
	 * with pessimistic cases up to 100us and a recommendation to
	 * cap at 1ms. We go a bit higher just in case.
	 */
	err = __intel_wait_for_register(uncore, tlb_inv_cr.reg,
					tlb_inv_cr.val, 0,
					100, 4,
					NULL);
	drm_WARN_ONCE(&engine->i915->drm,
		      ((err != 0) && (gt->reset.flags == 0) &&
		       (rstcnt == atomic_read(&gt->reset.engines_reset_count))),
		      "Tlb invalidation timed out on %s", engine->name);
}

static void invalidate_tlb_engine(struct intel_engine_cs *engine)
{
	struct intel_uncore *uncore = engine->uncore;
	struct tlb_inv_cr tlb_inv_cr;
	int err = 0;

	if (engine->instance > 0)
		return;

	err = get_tlb_inv_cr(engine, &tlb_inv_cr);
	if (unlikely(err))
		return;

	intel_uncore_write(uncore, tlb_inv_cr.reg, ~0u);

	/* Wa_2207587034:tgl,dg1,rkl,adl-s,adl-p */
	if (IS_TIGERLAKE(engine->i915) ||
	    IS_DG1(engine->i915) ||
	    IS_ROCKETLAKE(engine->i915) ||
	    IS_ALDERLAKE_S(engine->i915) ||
	    IS_ALDERLAKE_P(engine->i915))
		intel_uncore_write(uncore, GEN12_OA_TLB_INV_CR, 1);
}

void intel_invalidate_tlb_full_flush(struct intel_gt *gt)
{
	struct intel_guc *guc = &gt->uc.guc;
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	lockdep_assert_held(&gt->mutex);

	if (GRAPHICS_VER(gt->i915) < 12)
		return;

	/* Tlb invalidation via GuC blocks until invaliation is complete */
	if (INTEL_GUC_SUPPORTS_TLB_INVALIDATION(guc))
		return;

	for_each_engine(engine, gt, id)
		invalidate_tlb_engine_flush(engine);
}

void intel_invalidate_tlb_full(struct intel_gt *gt)
{
	struct intel_guc *guc = &gt->uc.guc;
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	lockdep_assert_held(&gt->mutex);

	if (GRAPHICS_VER(gt->i915) < 12)
		return;

	trace_intel_tlb_invalidate(gt, 0, 0);

	if (INTEL_GUC_SUPPORTS_TLB_INVALIDATION(guc)) {
		intel_guc_invalidate_tlb_full(guc, INTEL_GUC_TLB_INVAL_MODE_HEAVY);
	} else {
		intel_uncore_forcewake_get(gt->uncore, FORCEWAKE_ALL);
		for_each_engine(engine, gt, id)
			invalidate_tlb_engine(engine);
		intel_uncore_forcewake_put(gt->uncore, FORCEWAKE_ALL);
	}
}

void intel_invalidate_tlb_full_sync(struct intel_gt *gt)
{
	lockdep_assert_held(&gt->mutex);

	if (GRAPHICS_VER(gt->i915) < 12)
		return;

	/* Invalidate tlb's */
	intel_invalidate_tlb_full(gt);

	/* Wait fot invalidations to finish */
	intel_invalidate_tlb_full_flush(gt);
}

static void invalidate_tlb_page_selective_mmio(struct intel_gt *gt, u64 start, u64 length)
{
	u32 address_mask = (ilog2(length) - ilog2(I915_GTT_PAGE_SIZE_4K));
	u64 vm_total = BIT_ULL(INTEL_INFO(gt->i915)->ppgtt_size);
	struct intel_uncore *uncore = gt->uncore;
	u32 dw0, dw1;
	int err;

	GEM_BUG_ON(!IS_ALIGNED(start, I915_GTT_PAGE_SIZE_4K));
	GEM_BUG_ON(!IS_ALIGNED(length, I915_GTT_PAGE_SIZE_4K));
	GEM_BUG_ON(range_overflows(start, length, vm_total));

	dw0 = FIELD_PREP(XEHPSDV_TLB_INV_DESC0_ADDR_LO, (lower_32_bits(start) >> 12)) |
		FIELD_PREP(XEHPSDV_TLB_INV_DESC0_ADDR_MASK, address_mask) |
		FIELD_PREP(XEHPSDV_TLB_INV_DESC0_G, 0x3) |
		FIELD_PREP(XEHPSDV_TLB_INV_DESC0_VALID, 0x1);
	dw1 = upper_32_bits(start);
	mutex_lock(&gt->mutex);
	intel_uncore_write(uncore, XEHPSDV_TLB_INV_DESC1, dw1);
	intel_uncore_write(uncore, XEHPSDV_TLB_INV_DESC0, dw0);
	err = __intel_wait_for_register(uncore,
					XEHPSDV_TLB_INV_DESC0,
					XEHPSDV_TLB_INV_DESC0_VALID,
					0, 100, 10, NULL);
	if (err)
		drm_err(&gt->i915->drm, "mmio: tlb invalidation response timed out\n");
	mutex_unlock(&gt->mutex);
}

void intel_invalidate_tlb_range(struct intel_gt *gt,
				struct i915_address_space *vm,
				u64 start, u64 length)
{
	struct intel_guc *guc = &gt->uc.guc;

	trace_intel_tlb_invalidate(gt, start, length);
	/*XXX: We are seeing timeouts on guc based tlb invalidations on XEHPSDV.
	 * Until we have a fix, use mmio
	 */
	if (IS_XEHPSDV(gt->i915))
		return invalidate_tlb_page_selective_mmio(gt, start, length);

	if (drm_WARN_ON_ONCE(&gt->i915->drm, !INTEL_GUC_SUPPORTS_TLB_INVALIDATION_SELECTIVE(guc)))
		return;

	intel_guc_invalidate_tlb_page_selective(guc,
						INTEL_GUC_TLB_INVAL_MODE_HEAVY,
						start, length, vm->asid);
}
