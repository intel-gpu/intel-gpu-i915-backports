// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "i915_drv.h"
#include "i915_perf_oa_regs.h"
#include "i915_trace.h"
#include "intel_engine_pm.h"
#include "intel_gt.h"
#include "intel_gt_mcr.h"
#include "intel_gt_pm.h"
#include "intel_gt_print.h"
#include "intel_gt_regs.h"
#include "intel_tlb.h"
#include "uc/intel_guc.h"

struct reg_and_bit {
	union {
		i915_reg_t reg;
		i915_mcr_reg_t mcr_reg;
	};
	u32 bit;
};

static struct reg_and_bit
get_reg_and_bit(const struct intel_engine_cs *engine, const bool gen8,
		const i915_reg_t *regs, const unsigned int num)
{
	const unsigned int class = engine->class;
	struct reg_and_bit rb = {};

	if (drm_WARN_ON_ONCE(&engine->i915->drm,
			     class >= num || !regs[class].reg))
		return rb;

	rb.reg = regs[class];
	if (gen8 && class == VIDEO_DECODE_CLASS)
		rb.reg.reg += 4 * engine->instance; /* GEN8_M2TCR */
	else
		rb.bit = engine->instance;

	rb.bit = BIT(rb.bit);

	return rb;
}

static struct reg_and_bit
get_mcr_reg_and_bit(const struct intel_engine_cs *engine,
		    const i915_mcr_reg_t *regs, const unsigned int num)
{
	const unsigned int class = engine->class;
	struct reg_and_bit rb = {};

	if (drm_WARN_ON_ONCE(&engine->i915->drm,
			     class >= num || !regs[class].reg))
		return rb;

	rb.mcr_reg = regs[class];
	rb.bit = BIT(engine->instance);

	return rb;
}

/*
 * HW architecture suggest typical invalidation time at 40us,
 * with pessimistic cases up to 100us and a recommendation to
 * cap at 1ms. We go a bit higher just in case.
 */
#define TLB_INVAL_TIMEOUT_US 100
#define TLB_INVAL_TIMEOUT_MS 4

/*
 * On Xe_HP the TLB invalidation registers are located at the same MMIO offsets
 * but are now considered MCR registers.  Since they exist within a GAM range,
 * the primary instance of the register rolls up the status from each unit.
 */
static int wait_for_invalidate(struct intel_gt *gt, struct reg_and_bit rb)
{
	if (GRAPHICS_VER_FULL(gt->i915) >= IP_VER(12, 50))
		return intel_gt_mcr_wait_for_reg(gt, rb.mcr_reg, rb.bit, 0,
						 TLB_INVAL_TIMEOUT_US,
						 TLB_INVAL_TIMEOUT_MS);
	else
		return __intel_wait_for_register_fw(gt->uncore, rb.reg, rb.bit, 0,
						    TLB_INVAL_TIMEOUT_US,
						    TLB_INVAL_TIMEOUT_MS,
						    NULL);
}

static bool tlb_seqno_passed(const struct intel_gt *gt, u32 seqno)
{
	u32 cur = intel_gt_tlb_seqno(gt);

	/* Only skip if a *full* TLB invalidate barrier has passed */
	return (s32)(cur - ALIGN(seqno, 2)) > 0;
}

static void mmio_invalidate_full(struct intel_gt *gt)
{
	static const i915_reg_t gen8_regs[] = {
		[RENDER_CLASS]			= GEN8_RTCR,
		[VIDEO_DECODE_CLASS]		= GEN8_M1TCR, /* , GEN8_M2TCR */
		[VIDEO_ENHANCEMENT_CLASS]	= GEN8_VTCR,
		[COPY_ENGINE_CLASS]		= GEN8_BTCR,
	};
	static const i915_reg_t gen12_regs[] = {
		[RENDER_CLASS]			= GEN12_GFX_TLB_INV_CR,
		[VIDEO_DECODE_CLASS]		= GEN12_VD_TLB_INV_CR,
		[VIDEO_ENHANCEMENT_CLASS]	= GEN12_VE_TLB_INV_CR,
		[COPY_ENGINE_CLASS]		= GEN12_BLT_TLB_INV_CR,
		[COMPUTE_CLASS]			= GEN12_COMPCTX_TLB_INV_CR,
	};
	static const i915_mcr_reg_t xehp_regs[] = {
		[RENDER_CLASS]			= XEHP_GFX_TLB_INV_CR,
		[VIDEO_DECODE_CLASS]		= XEHP_VD_TLB_INV_CR,
		[VIDEO_ENHANCEMENT_CLASS]	= XEHP_VE_TLB_INV_CR,
		[COPY_ENGINE_CLASS]		= XEHP_BLT_TLB_INV_CR,
		[COMPUTE_CLASS]			= XEHP_COMPCTX_TLB_INV_CR,
	};
	struct drm_i915_private *i915 = gt->i915;
	struct intel_uncore *uncore = gt->uncore;
	struct intel_engine_cs *engine;
	intel_engine_mask_t awake, tmp;
	enum intel_engine_id id;
	const i915_reg_t *regs;
	unsigned int num = 0;
	unsigned long flags;

	if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 50)) {
		regs = NULL;
		num = ARRAY_SIZE(xehp_regs);
	} else if (GRAPHICS_VER(i915) == 12) {
		regs = gen12_regs;
		num = ARRAY_SIZE(gen12_regs);
	} else if (GRAPHICS_VER(i915) >= 8 && GRAPHICS_VER(i915) <= 11) {
		regs = gen8_regs;
		num = ARRAY_SIZE(gen8_regs);
	} else if (GRAPHICS_VER(i915) < 8) {
		return;
	}

	if (gt_WARN_ONCE(gt, !num, "Platform does not implement TLB invalidation!"))
		return;

	intel_uncore_forcewake_get(uncore, FORCEWAKE_ALL);

	intel_gt_mcr_lock(gt, &flags);
	spin_lock(&uncore->lock); /* serialise invalidate with GT reset */

	awake = 0;
	for_each_engine(engine, gt, id) {
		struct reg_and_bit rb;

		if (!intel_engine_pm_is_awake(engine))
			continue;

		if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 50)) {
			rb = get_mcr_reg_and_bit(engine, xehp_regs, num);
			if (!i915_mmio_reg_offset(rb.reg))
				continue;

			if (engine->class == VIDEO_DECODE_CLASS ||
			    engine->class == VIDEO_ENHANCEMENT_CLASS ||
			    engine->class == COMPUTE_CLASS)
				rb.bit = _MASKED_BIT_ENABLE(rb.bit);

			intel_gt_mcr_multicast_write_fw(gt, rb.mcr_reg, rb.bit);
		} else {
			rb = get_reg_and_bit(engine, regs == gen8_regs, regs, num);

			if (!i915_mmio_reg_offset(rb.reg))
				continue;

			if (GRAPHICS_VER(i915) == 12 &&
			    (engine->class == VIDEO_DECODE_CLASS ||
			     engine->class == VIDEO_ENHANCEMENT_CLASS ||
			     engine->class == COMPUTE_CLASS))
				rb.bit = _MASKED_BIT_ENABLE(rb.bit);

			intel_uncore_write_fw(uncore, rb.reg, rb.bit);
		}
		awake |= engine->mask;
	}

	GT_TRACE(gt, "invalidated engines %08x\n", awake);

	/* Wa_2207587034:tgl,dg1,rkl,adl-s,adl-p */
	if (awake &&
	    (IS_TIGERLAKE(i915) ||
	     IS_DG1(i915) ||
	     IS_ROCKETLAKE(i915) ||
	     IS_ALDERLAKE_S(i915) ||
	     IS_ALDERLAKE_P(i915)))
		intel_uncore_write_fw(uncore, GEN12_OA_TLB_INV_CR, 1);

	spin_unlock(&uncore->lock);
	intel_gt_mcr_unlock(gt, flags);

	for_each_engine_masked(engine, gt, awake, tmp) {
		struct reg_and_bit rb;

		if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 50))
			rb = get_mcr_reg_and_bit(engine, xehp_regs, num);
		else
			rb = get_reg_and_bit(engine, regs == gen8_regs, regs, num);
		GEM_BUG_ON(!i915_mmio_reg_offset(rb.reg));

		if (wait_for_invalidate(gt, rb))
			gt_err_ratelimited(gt, "%s TLB invalidation did not complete in %ums!\n",
					   engine->name, TLB_INVAL_TIMEOUT_MS);
	}

	/*
	 * Use delayed put since a) we mostly expect a flurry of TLB
	 * invalidations so it is good to avoid paying the forcewake cost and
	 * b) it works around a bug in Icelake which cannot cope with too rapid
	 * transitions.
	 */
	intel_uncore_forcewake_put_delayed(uncore, FORCEWAKE_ALL);
}

void intel_gt_invalidate_tlb_full(struct intel_gt *gt, u32 seqno)
{
	intel_wakeref_t wakeref;

	if (I915_SELFTEST_ONLY(gt->awake == -ENODEV))
		return;

	if (intel_gt_is_wedged(gt))
		return;

	if (tlb_seqno_passed(gt, seqno))
		return;

	trace_intel_tlb_invalidate(gt, 0, 0);
	with_intel_gt_pm_if_awake(gt, wakeref) {
		struct intel_guc *guc = &gt->uc.guc;

		mutex_lock(&gt->tlb.invalidate_lock);
		if (tlb_seqno_passed(gt, seqno))
			goto unlock;

		if (intel_guc_invalidate_tlb_full(guc, INTEL_GUC_TLB_INVAL_MODE_HEAVY) < 0)
			mmio_invalidate_full(gt);

		write_seqcount_invalidate(&gt->tlb.seqno);
unlock:
		mutex_unlock(&gt->tlb.invalidate_lock);
	}
}

static bool mmio_invalidate_range(struct intel_gt *gt, u64 start, u64 length)
{
	u64 vm_total = BIT_ULL(INTEL_INFO(gt->i915)->ppgtt_size);
	/*
	 * For page selective invalidations, this specifies the number of contiguous
	 * PPGTT pages that needs to be invalidated. The Address Mask values are 0 for
	 * 4KB page, 4 for 64KB page, 12 for 2MB page.
	 */
	u32 address_mask = (ilog2(length) - ilog2(SZ_4K));
	intel_wakeref_t wakeref;
	u32 dw0, dw1;
	int err;

	GEM_BUG_ON(length < SZ_4K);
	GEM_BUG_ON(!is_power_of_2(length));
	GEM_BUG_ON(length & GENMASK(ilog2(SZ_16M) - 1, ilog2(SZ_2M) + 1));
	GEM_BUG_ON(!IS_ALIGNED(start, length));
	GEM_BUG_ON(range_overflows(start, length, vm_total));

	dw0 = FIELD_PREP(XEHPSDV_TLB_INV_DESC0_ADDR_LO, (lower_32_bits(start) >> 12)) |
		FIELD_PREP(XEHPSDV_TLB_INV_DESC0_ADDR_MASK, address_mask) |
		FIELD_PREP(XEHPSDV_TLB_INV_DESC0_G, 0x3) |
		FIELD_PREP(XEHPSDV_TLB_INV_DESC0_VALID, 0x1);
	dw1 = upper_32_bits(start);

	err = 0;
	with_intel_gt_pm_if_awake(gt, wakeref) {
		struct intel_uncore *uncore = gt->uncore;

		intel_uncore_forcewake_get(uncore, FORCEWAKE_ALL);

		mutex_lock(&gt->tlb.invalidate_lock);
		intel_uncore_write_fw(uncore, XEHPSDV_TLB_INV_DESC1, dw1);
		intel_uncore_write_fw(uncore, XEHPSDV_TLB_INV_DESC0, dw0);
		err = __intel_wait_for_register_fw(uncore,
						   XEHPSDV_TLB_INV_DESC0,
						   XEHPSDV_TLB_INV_DESC0_VALID,
						   0, 100, 10, NULL);
		mutex_unlock(&gt->tlb.invalidate_lock);

		intel_uncore_forcewake_put_delayed(uncore, FORCEWAKE_ALL);
	}

	if (err)
		drm_err_ratelimited(&gt->i915->drm,
				    "TLB invalidation response timed out\n");

	return err == 0;
}

static u64 tlb_page_selective_size(u64 *addr, u64 length)
{
	const u64 end = *addr + length;
	u64 start;

	/*
	 * Minimum invalidation size for a 2MB page that the hardware expects is
	 * 16MB
	 */
	length = max_t(u64, roundup_pow_of_two(length), SZ_4K);
	if (length >= SZ_2M)
		length = max_t(u64, SZ_16M, length);

	/*
	 * We need to invalidate a higher granularity if start address is not
	 * aligned to length. When start is not aligned with length we need to
	 * find the length large enough to create an address mask covering the
	 * required range.
	 */
	start = round_down(*addr, length);
	while (start + length < end) {
		length <<= 1;
		start = round_down(*addr, length);
	}

	*addr = start;
	return length;
}

bool intel_gt_invalidate_tlb_range(struct intel_gt *gt,
				   struct i915_address_space *vm,
				   u64 start, u64 length)
{
	struct intel_guc *guc = &gt->uc.guc;
	intel_wakeref_t wakeref;
	u64 size, vm_total;
	bool ret = true;

	if (intel_gt_is_wedged(gt))
		return true;

	trace_intel_tlb_invalidate(gt, start, length);

	vm_total = BIT_ULL(INTEL_INFO(gt->i915)->ppgtt_size);
	/* Align start and length */
	size =  min_t(u64, vm_total, tlb_page_selective_size(&start, length));

	/*XXX: We are seeing timeouts on guc based tlb invalidations on XEHPSDV.
	 * Until we have a fix, use mmio
	 */
	if (IS_XEHPSDV(gt->i915))
		return mmio_invalidate_range(gt, start, size);

	with_intel_gt_pm_if_awake(gt, wakeref)
		ret = intel_guc_invalidate_tlb_page_selective(guc,
							      INTEL_GUC_TLB_INVAL_MODE_HEAVY,
							      start, size, vm->asid) == 0;

	return ret;
}

void intel_gt_init_tlb(struct intel_gt *gt)
{
	mutex_init(&gt->tlb.invalidate_lock);
	seqcount_mutex_init(&gt->tlb.seqno, &gt->tlb.invalidate_lock);
}

void intel_gt_fini_tlb(struct intel_gt *gt)
{
	mutex_destroy(&gt->tlb.invalidate_lock);
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftest_tlb.c"
#endif
