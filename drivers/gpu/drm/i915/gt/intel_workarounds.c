// SPDX-License-Identifier: MIT
/*
 * Copyright © 2014-2018 Intel Corporation
 */

#include "i915_drv.h"
#include "intel_context.h"
#include "intel_engine_pm.h"
#include "intel_engine_regs.h"
#include "intel_gpu_commands.h"
#include "intel_gt.h"
#include "intel_gt_mcr.h"
#include "intel_gt_regs.h"
#include "intel_ring.h"
#include "intel_workarounds.h"

/**
 * DOC: Hardware workarounds
 *
 * This file is intended as a central place to implement most [1]_ of the
 * required workarounds for hardware to work as originally intended. They fall
 * in five basic categories depending on how/when they are applied:
 *
 * - Workarounds that touch registers that are saved/restored to/from the HW
 *   context image. The list is emitted (via Load Register Immediate commands)
 *   everytime a new context is created.
 * - GT workarounds. The list of these WAs is applied whenever these registers
 *   revert to default values (on GPU reset, suspend/resume [2]_, etc..).
 * - Display workarounds. The list is applied during display clock-gating
 *   initialization.
 * - Workarounds that whitelist a privileged register, so that UMDs can manage
 *   them directly. This is just a special case of a MMMIO workaround (as we
 *   write the list of these to/be-whitelisted registers to some special HW
 *   registers).
 * - Workaround batchbuffers, that get executed automatically by the hardware
 *   on every HW context restore.
 *
 * .. [1] Please notice that there are other WAs that, due to their nature,
 *    cannot be applied from a central place. Those are peppered around the rest
 *    of the code, as needed.
 *
 * .. [2] Technically, some registers are powercontext saved & restored, so they
 *    survive a suspend/resume. In practice, writing them again is not too
 *    costly and simplifies things. We can revisit this in the future.
 *
 * Layout
 * ~~~~~~
 *
 * Keep things in this file ordered by WA type, as per the above (context, GT,
 * display, register whitelist, batchbuffer). Then, inside each type, keep the
 * following order:
 *
 * - Infrastructure functions and macros
 * - WAs per platform in standard gen/chrono order
 * - Public functions to init or apply the given workaround type.
 */

static void wa_init(struct i915_wa_list *wal, const char *name, const char *engine_name)
{
	wal->name = name;
	wal->engine_name = engine_name;
}

#define WA_LIST_CHUNK (1 << 4)

static int _wa_index(struct i915_wa_list *wal, i915_reg_t reg)
{
	unsigned int addr = i915_mmio_reg_offset(reg);
	int start = 0, end = wal->count;

	/* addr and wal->list[].reg, both include the R/W flags */
	while (start < end) {
		int mid = start + (end - start) / 2;

		if (i915_mmio_reg_offset(wal->list[mid].reg) < addr)
			start = mid + 1;
		else if (i915_mmio_reg_offset(wal->list[mid].reg) > addr)
			end = mid;
		else
			return mid;
	}

	return -1;
}

static void _wa_remove(struct i915_wa_list *wal, i915_reg_t reg, u32 flags)
{
	int index;
	struct i915_wa *wa = wal->list;

	reg.reg |= flags;

	index = _wa_index(wal, reg);
	if (index < 0)
		return;

	memset(wa + index, 0, sizeof(*wa));

	while (index < wal->count - 1) {
		swap(wa[index], wa[index + 1]);
		index++;
	}

	wal->count--;
}

static void _wa_mcr_remove(struct i915_wa_list *wal, i915_mcr_reg_t mreg, u32 flags)
{
	i915_reg_t r = _MMIO(mreg.reg);

	_wa_remove(wal, r, flags);
}

static void _wa_add(struct i915_wa_list *wal, const struct i915_wa *wa)
{
	int index;
	const unsigned int grow = WA_LIST_CHUNK;
	struct i915_wa *wa_;

	BUILD_BUG_ON(!is_power_of_2(grow));

	if (IS_ALIGNED(wal->count, grow)) { /* Either uninitialized or full. */
		struct i915_wa *list;

		list = kmalloc_array(ALIGN(wal->count + 1, grow), sizeof(*wa),
				     GFP_KERNEL);
		if (!list) {
			DRM_ERROR("No space for workaround init!\n");
			return;
		}

		if (wal->list) {
			memcpy(list, wal->list, sizeof(*wa) * wal->count);
			kfree(wal->list);
		}

		wal->list = list;
	}

	index = _wa_index(wal, wa->reg);
	if (index >= 0) {
		wa_ = &wal->list[index];

		if ((wa->clr | wa_->clr) && !(wa->clr & ~wa_->clr)) {
			DRM_ERROR("Discarding overwritten w/a for reg %04x (clear: %08x, set: %08x)\n",
				  i915_mmio_reg_offset(wa_->reg),
				  wa_->clr, wa_->set);

			wa_->set &= ~wa->clr;
		}

		GEM_WARN_ON(wa->masked_reg != wa_->masked_reg);

		if (wa->masked_reg) {
			GEM_WARN_ON(wa->clr);
			GEM_WARN_ON(wa_->clr);

			/* Keep the enable mask, reset the actual target bits */
			wa_->set &= ~(wa->set >> 16);
		}

		wa_->set |= wa->set;
		wa_->clr |= wa->clr;
		wa_->read |= wa->read;
		return;
	}

	wa_ = &wal->list[wal->count++];
	*wa_ = *wa;

	while (wa_-- > wal->list) {
		GEM_BUG_ON(i915_mmio_reg_offset(wa_[0].reg) ==
			   i915_mmio_reg_offset(wa_[1].reg));
		if (i915_mmio_reg_offset(wa_[1].reg) >
		    i915_mmio_reg_offset(wa_[0].reg))
			break;

		swap(wa_[1], wa_[0]);
	}
}

static void wa_add(struct i915_wa_list *wal, i915_reg_t reg,
		   u32 clear, u32 set, u32 read_mask, bool masked_reg)
{
	struct i915_wa wa = {
		.reg  = reg,
		.clr  = clear,
		.set  = set,
		.read = read_mask,
		.masked_reg = masked_reg,
	};

	_wa_add(wal, &wa);
}

static void wa_mcr_add(struct i915_wa_list *wal, i915_mcr_reg_t reg,
		       u32 clear, u32 set, u32 read_mask, bool masked_reg)
{
	struct i915_wa wa = {
		.mcr_reg = reg,
		.clr  = clear,
		.set  = set,
		.read = read_mask,
		.masked_reg = masked_reg,
		.is_mcr = 1,
	};

	_wa_add(wal, &wa);
}

static void
wa_write_clr_set(struct i915_wa_list *wal, i915_reg_t reg, u32 clear, u32 set)
{
	wa_add(wal, reg, clear, set, clear, false);
}

static void
wa_mcr_write_clr_set(struct i915_wa_list *wal, i915_mcr_reg_t reg, u32 clear, u32 set)
{
	wa_mcr_add(wal, reg, clear, set, clear, false);
}

static void
wa_write(struct i915_wa_list *wal, i915_reg_t reg, u32 set)
{
	wa_write_clr_set(wal, reg, ~0, set);
}

static void
wa_mcr_write(struct i915_wa_list *wal, i915_mcr_reg_t reg, u32 set)
{
	wa_mcr_write_clr_set(wal, reg, ~0, set);
}

static void
wa_write_or(struct i915_wa_list *wal, i915_reg_t reg, u32 set)
{
	wa_write_clr_set(wal, reg, set, set);
}

static void
wa_mcr_write_or(struct i915_wa_list *wal, i915_mcr_reg_t reg, u32 set)
{
	wa_mcr_write_clr_set(wal, reg, set, set);
}

static void
wa_write_clr(struct i915_wa_list *wal, i915_reg_t reg, u32 clr)
{
	wa_write_clr_set(wal, reg, clr, 0);
}

static void
wa_mcr_write_clr(struct i915_wa_list *wal, i915_mcr_reg_t reg, u32 clr)
{
	wa_mcr_write_clr_set(wal, reg, clr, 0);
}

/*
 * WA operations on "masked register". A masked register has the upper 16 bits
 * documented as "masked" in b-spec. Its purpose is to allow writing to just a
 * portion of the register without a rmw: you simply write in the upper 16 bits
 * the mask of bits you are going to modify.
 *
 * The wa_masked_* family of functions already does the necessary operations to
 * calculate the mask based on the parameters passed, so user only has to
 * provide the lower 16 bits of that register.
 */

static void
wa_masked_en(struct i915_wa_list *wal, i915_reg_t reg, u32 val)
{
	wa_add(wal, reg, 0, _MASKED_BIT_ENABLE(val), val, true);
}

static void
wa_mcr_masked_en(struct i915_wa_list *wal, i915_mcr_reg_t reg, u32 val)
{
	wa_mcr_add(wal, reg, 0, _MASKED_BIT_ENABLE(val), val, true);
}

static void
wa_masked_dis(struct i915_wa_list *wal, i915_reg_t reg, u32 val)
{
	wa_add(wal, reg, 0, _MASKED_BIT_DISABLE(val), val, true);
}

static void
wa_mcr_masked_dis(struct i915_wa_list *wal, i915_mcr_reg_t reg, u32 val)
{
	wa_mcr_add(wal, reg, 0, _MASKED_BIT_DISABLE(val), val, true);
}

static void
wa_masked_field_set(struct i915_wa_list *wal, i915_reg_t reg,
		    u32 mask, u32 val)
{
	wa_add(wal, reg, 0, _MASKED_FIELD(mask, val), mask, true);
}

static void
wa_mcr_masked_field_set(struct i915_wa_list *wal, i915_mcr_reg_t reg,
			u32 mask, u32 val)
{
	wa_mcr_add(wal, reg, 0, _MASKED_FIELD(mask, val), mask, true);
}

/* Returns true if global was enabled and nothing more needs to be done */
static void gen9_debug_td_ctl_init(struct intel_engine_cs *engine,
				   struct i915_wa_list *wal)
{
	u32 ctl_mask;

	GEM_BUG_ON(GRAPHICS_VER(engine->i915) < 9);

	ctl_mask = TD_CTL_BREAKPOINT_ENABLE |
		TD_CTL_FORCE_THREAD_BREAKPOINT_ENABLE |
		TD_CTL_FEH_AND_FEE_ENABLE;

	if (GRAPHICS_VER_FULL(engine->i915) >= IP_VER(12, 50))
		ctl_mask |= TD_CTL_GLOBAL_DEBUG_ENABLE;

	wa_mcr_add(wal, TD_CTL, 0, ctl_mask, ctl_mask, false);
}

/*
 * These settings aren't actually workarounds, but general tuning settings that
 * need to be programmed on dg2 platform.
 */
static void dg2_ctx_gt_tuning_init(struct intel_engine_cs *engine,
				   struct i915_wa_list *wal)
{
	wa_mcr_masked_en(wal, CHICKEN_RASTER_2, TBIMR_FAST_CLIP);
	wa_mcr_write_clr_set(wal, XEHP_L3SQCREG5, L3_PWM_TIMER_INIT_VAL_MASK,
			     REG_FIELD_PREP(L3_PWM_TIMER_INIT_VAL_MASK, 0x7f));
	wa_mcr_add(wal,
		   XEHP_FF_MODE2,
		   FF_MODE2_TDS_TIMER_MASK,
		   FF_MODE2_TDS_TIMER_128,
		   0, false);
}

/*
 * These settings aren't actually workarounds, but general tuning settings that
 * need to be programmed on several platforms.
 */
static void gen12_ctx_gt_tuning_init(struct intel_engine_cs *engine,
				     struct i915_wa_list *wal)
{
	/*
	 * Although some platforms refer to it as Wa_1604555607, we need to
	 * program it even on those that don't explicitly list that
	 * workaround.
	 *
	 * Note that the programming of this register is further modified
	 * according to the FF_MODE2 guidance given by Wa_1608008084:gen12.
	 * Wa_1608008084 tells us the FF_MODE2 register will return the wrong
	 * value when read. The default value for this register is zero for all
	 * fields and there are no bit masks. So instead of doing a RMW we
	 * should just write TDS timer value. For the same reason read
	 * verification is ignored.
	 */
	wa_add(wal,
	       GEN12_FF_MODE2,
	       FF_MODE2_TDS_TIMER_MASK,
	       FF_MODE2_TDS_TIMER_128,
	       0, false);
}

/*
 * These settings aren't actually workarounds, but general tuning settings that
 * need to be programmed on PVC 0x0BD6.
 */
static void pvc_ctx_gt_tuning_init(struct intel_engine_cs *engine,
				   struct i915_wa_list *wal)
{
	/*
	 * For tuning power.
	 * FPU residue disable will lower the power consumption.
	 */
	if (INTEL_DEVID(engine->i915) == 0x0BD6 &&
	    engine->flags & I915_ENGINE_FIRST_RENDER_COMPUTE) {
		struct intel_gt *gt;
		u16 eu_count = 0;
		int id;

		for_each_gt(gt, engine->i915, id)
			eu_count += gt->info.sseu.eu_total;

		if (eu_count == 1024)
			wa_mcr_write_or(wal, GEN8_ROW_CHICKEN, FPU_RESIDUAL_DISABLE);
	}
}

static void gen12_ctx_workarounds_init(struct intel_engine_cs *engine,
				       struct i915_wa_list *wal)
{
	struct drm_i915_private *i915 = engine->i915;

	gen12_ctx_gt_tuning_init(engine, wal);

	/*
	 * Wa_1409142259:tgl,dg1,adl-p
	 * Wa_1409347922:tgl,dg1,adl-p
	 * Wa_1409252684:tgl,dg1,adl-p
	 * Wa_1409217633:tgl,dg1,adl-p
	 * Wa_1409207793:tgl,dg1,adl-p
	 * Wa_1409178076:tgl,dg1,adl-p
	 * Wa_1408979724:tgl,dg1,adl-p
	 * Wa_14010443199:tgl,rkl,dg1,adl-p
	 * Wa_14010698770:tgl,rkl,dg1,adl-s,adl-p
	 * Wa_1409342910:tgl,rkl,dg1,adl-s,adl-p
	 */
	wa_masked_en(wal, GEN11_COMMON_SLICE_CHICKEN3,
		     GEN12_DISABLE_CPS_AWARE_COLOR_PIPE);

	/* WaDisableGPGPUMidThreadPreemption:gen12 */
	wa_masked_field_set(wal, GEN8_CS_CHICKEN1,
			    GEN9_PREEMPT_GPGPU_LEVEL_MASK,
			    GEN9_PREEMPT_GPGPU_THREAD_GROUP_LEVEL);

	/*
	 * Wa_16011163337
	 *
	 * Like in gen12_ctx_gt_tuning_init(), read verification is ignored due
	 * to Wa_1608008084.
	 */
	wa_add(wal,
	       GEN12_FF_MODE2,
	       FF_MODE2_GS_TIMER_MASK,
	       FF_MODE2_GS_TIMER_224,
	       0, false);

	if (!IS_DG1(i915)) {
		/* Wa_1806527549 */
		wa_masked_en(wal, HIZ_CHICKEN, HZ_DEPTH_TEST_LE_GE_OPT_DISABLE);

		/* Wa_1606376872 */
		wa_masked_en(wal, COMMON_SLICE_CHICKEN4, DISABLE_TDC_LOAD_BALANCING_CALC);
	}
}

static void dg1_ctx_workarounds_init(struct intel_engine_cs *engine,
				     struct i915_wa_list *wal)
{
	gen12_ctx_workarounds_init(engine, wal);

	/* Wa_1409044764 */
	wa_masked_dis(wal, GEN11_COMMON_SLICE_CHICKEN3,
		      DG1_FLOAT_POINT_BLEND_OPT_STRICT_MODE_EN);

	/* Wa_22010493298 */
	wa_masked_en(wal, HIZ_CHICKEN,
		     DG1_HZ_READ_SUPPRESSION_OPTIMIZATION_DISABLE);
}

static void dg2_ctx_workarounds_init(struct intel_engine_cs *engine,
				     struct i915_wa_list *wal)
{
	dg2_ctx_gt_tuning_init(engine, wal);

	/* Wa_16011186671:dg2_g11 */
	if (IS_DG2_GRAPHICS_STEP(engine->i915, G11, STEP_A0, STEP_B0)) {
		wa_mcr_masked_dis(wal, VFLSKPD, DIS_MULT_MISS_RD_SQUASH);
		wa_mcr_masked_en(wal, VFLSKPD, DIS_OVER_FETCH_CACHE);
	}

	if (IS_DG2_GRAPHICS_STEP(engine->i915, G10, STEP_A0, STEP_B0)) {
		/* Wa_14010469329:dg2_g10 */
		wa_mcr_masked_en(wal, XEHP_COMMON_SLICE_CHICKEN3,
				 XEHP_DUAL_SIMD8_SEQ_MERGE_DISABLE);

		/*
		 * Wa_22010465075:dg2_g10
		 * Wa_22010613112:dg2_g10
		 * Wa_14010698770:dg2_g10
		 */
		wa_mcr_masked_en(wal, XEHP_COMMON_SLICE_CHICKEN3,
				 GEN12_DISABLE_CPS_AWARE_COLOR_PIPE);
	}

	/* Wa_16013271637:dg2 */
	wa_mcr_masked_en(wal, XEHP_SLICE_COMMON_ECO_CHICKEN1,
			 MSC_MSAA_REODER_BUF_BYPASS_DISABLE);

	/* Wa_14014947963:dg2 */
	if (IS_DG2_GRAPHICS_STEP(engine->i915, G10, STEP_B0, STEP_FOREVER) ||
	    IS_DG2_G11(engine->i915) || IS_DG2_G12(engine->i915))
		wa_masked_field_set(wal, VF_PREEMPTION, PREEMPTION_VERTEX_COUNT, 0x4000);

	/* Wa_18018764978:dg2 */
	if (IS_DG2_GRAPHICS_STEP(engine->i915, G10, STEP_C0, STEP_FOREVER) ||
	    IS_DG2_G11(engine->i915) || IS_DG2_G12(engine->i915))
		wa_mcr_masked_en(wal, XEHP_PSS_MODE2, SCOREBOARD_STALL_FLUSH_CONTROL);

	/* Wa_15010599737:dg2 */
	wa_mcr_masked_en(wal, CHICKEN_RASTER_1, DIS_SF_ROUND_NEAREST_EVEN);

	/* Wa_18019271663:dg2 */
	wa_mcr_masked_en(wal, XEHP_CACHE_MODE_1, MSAA_OPTIMIZATION_REDUC_DISABLE);
}

static void mtl_ctx_workarounds_init(struct intel_engine_cs *engine,
				     struct i915_wa_list *wal)
{
	struct drm_i915_private *i915 = engine->i915;

	if (IS_MTL_GRAPHICS_STEP(i915, M, STEP_A0, STEP_B0) ||
	    IS_MTL_GRAPHICS_STEP(i915, P, STEP_A0, STEP_B0)) {
		/* Wa_14014947963 */
		wa_masked_field_set(wal, VF_PREEMPTION,
				    PREEMPTION_VERTEX_COUNT, 0x4000);

		/* Wa_16013271637 */
		wa_mcr_masked_en(wal, XEHP_SLICE_COMMON_ECO_CHICKEN1,
				 MSC_MSAA_REODER_BUF_BYPASS_DISABLE);

		/* Wa_18019627453 */
		wa_mcr_masked_en(wal, VFLSKPD, VF_PREFETCH_TLB_DIS);

		/* Wa_18018764978 */
		wa_mcr_masked_en(wal, XEHP_PSS_MODE2, SCOREBOARD_STALL_FLUSH_CONTROL);
	}

	/* Wa_18019271663 */
	wa_mcr_masked_en(wal, XEHP_CACHE_MODE_1, MSAA_OPTIMIZATION_REDUC_DISABLE);
}

static void pvc_ctx_workarounds_init(struct intel_engine_cs *engine,
				     struct i915_wa_list *wal)
{
	struct drm_i915_private *i915 = engine->i915;

	pvc_ctx_gt_tuning_init(engine, wal);

	if ((!i915->params.enable_256B ||
	     IS_PVC_BD_STEP(i915, STEP_A0, STEP_B0)) &&
	    engine->class == COPY_ENGINE_CLASS) {
		/*
		 * Wa_16011062782:pvc - WA applies only to Link Copy
		 * engines, but setting it also on Main Copy engine doesn't have
		 * any side-effect
		 */
		wa_masked_en(wal, BCS_ENGINE_SWCTL(engine->mmio_base),
			     BCS_ENGINE_SWCTL_DISABLE_256B);
	}
}

static void fakewa_disable_nestedbb_mode(struct intel_engine_cs *engine,
					 struct i915_wa_list *wal)
{
	/*
	 * This is a "fake" workaround defined by software to ensure we
	 * maintain reliable, backward-compatible behavior for userspace with
	 * regards to how nested MI_BATCH_BUFFER_START commands are handled.
	 *
	 * The per-context setting of MI_MODE[12] determines whether the bits
	 * of a nested MI_BATCH_BUFFER_START instruction should be interpreted
	 * in the traditional manner or whether they should instead use a new
	 * tgl+ meaning that breaks backward compatibility, but allows nesting
	 * into 3rd-level batchbuffers.  When this new capability was first
	 * added in TGL, it remained off by default unless a context
	 * intentionally opted in to the new behavior.  However Xe_HPG now
	 * flips this on by default and requires that we explicitly opt out if
	 * we don't want the new behavior.
	 *
	 * From a SW perspective, we want to maintain the backward-compatible
	 * behavior for userspace, so we'll apply a fake workaround to set it
	 * back to the legacy behavior on platforms where the hardware default
	 * is to break compatibility.  At the moment there is no Linux
	 * userspace that utilizes third-level batchbuffers, so this will avoid
	 * userspace from needing to make any changes.  using the legacy
	 * meaning is the correct thing to do.  If/when we have userspace
	 * consumers that want to utilize third-level batch nesting, we can
	 * provide a context parameter to allow them to opt-in.
	 */
	wa_masked_dis(wal, RING_MI_MODE(engine->mmio_base), TGL_NESTED_BB_EN);
}

static void gen12_ctx_gt_mocs_init(struct intel_engine_cs *engine,
				   struct i915_wa_list *wal)
{
	u8 mocs;

	/*
	 * Some blitter commands do not have a field for MOCS, those
	 * commands will use MOCS index pointed by BLIT_CCTL.
	 * BLIT_CCTL registers are needed to be programmed to un-cached.
	 */
	if (engine->class == COPY_ENGINE_CLASS) {
		mocs = engine->gt->mocs.uc_index;
		wa_write_clr_set(wal,
				 BLIT_CCTL(engine->mmio_base),
				 BLIT_CCTL_MASK,
				 BLIT_CCTL_MOCS(mocs, mocs));
	}
}

/*
 * gen12_ctx_gt_fake_wa_init() aren't programmingan official workaround
 * defined by the hardware team, but it programming general context registers.
 * Adding those context register programming in context workaround
 * allow us to use the wa framework for proper application and validation.
 */
static void
gen12_ctx_gt_fake_wa_init(struct intel_engine_cs *engine,
			  struct i915_wa_list *wal)
{
	if (GRAPHICS_VER_FULL(engine->i915) >= IP_VER(12, 55))
		fakewa_disable_nestedbb_mode(engine, wal);

	gen12_ctx_gt_mocs_init(engine, wal);

	if (engine->class == RENDER_CLASS)
		wa_mcr_masked_en(wal, XEHP_WM_CHICKEN2, DEPTH_STALL_DONE_DISABLE);
}

static void
__intel_engine_init_ctx_wa(struct intel_engine_cs *engine,
			   struct i915_wa_list *wal,
			   const char *name)
{
	struct drm_i915_private *i915 = engine->i915;
	bool render_only_ctx_wa = !IS_PONTEVECCHIO(i915);

	if (IS_SRIOV_VF(i915))
		return;

	wa_init(wal, name, engine->name);

	/* Applies to all engines */
	/*
	 * Fake workarounds are not the actual workaround but
	 * programming of context registers using workaround framework.
	 */
	if (GRAPHICS_VER(i915) >= 12)
		gen12_ctx_gt_fake_wa_init(engine, wal);

	if (engine->class != RENDER_CLASS && render_only_ctx_wa)
		return;

	if (IS_METEORLAKE(i915))
		mtl_ctx_workarounds_init(engine, wal);
	else if (IS_PONTEVECCHIO(i915))
		pvc_ctx_workarounds_init(engine, wal);
	else if (IS_DG2(i915))
		dg2_ctx_workarounds_init(engine, wal);
	else if (IS_DG1(i915))
		dg1_ctx_workarounds_init(engine, wal);
	else
		gen12_ctx_workarounds_init(engine, wal);

#if IS_ENABLED(CPTCFG_DRM_I915_DEBUGGER)
	if (i915->debuggers.enable_eu_debug && IS_GRAPHICS_VER(i915, 9, 11))
		gen9_debug_td_ctl_init(engine, wal);
#endif
}

void intel_engine_init_ctx_wa(struct intel_engine_cs *engine)
{
	__intel_engine_init_ctx_wa(engine, &engine->ctx_wa_list, "context");
}

int intel_engine_emit_ctx_wa(struct i915_request *rq)
{
	struct i915_wa_list *wal = &rq->engine->ctx_wa_list;
	struct i915_wa *wa;
	unsigned int i;
	u32 *cs;
	int ret;

	if (wal->count == 0)
		return 0;

	ret = rq->engine->emit_flush(rq, EMIT_BARRIER);
	if (ret)
		return ret;

	cs = intel_ring_begin(rq, (wal->count * 2 + 2));
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	*cs++ = MI_LOAD_REGISTER_IMM(wal->count);
	for (i = 0, wa = wal->list; i < wal->count; i++, wa++) {
		*cs++ = i915_mmio_reg_offset(wa->reg);
		*cs++ = wa->set;
	}
	*cs++ = MI_NOOP;

	intel_ring_advance(rq, cs);

	ret = rq->engine->emit_flush(rq, EMIT_BARRIER);
	if (ret)
		return ret;

	return 0;
}

static void __set_mcr_steering(struct i915_wa_list *wal,
			       i915_reg_t steering_reg,
			       unsigned int slice, unsigned int subslice)
{
	u32 mcr, mcr_mask;

	mcr = GEN11_MCR_SLICE(slice) | GEN11_MCR_SUBSLICE(subslice);
	mcr_mask = GEN11_MCR_SLICE_MASK | GEN11_MCR_SUBSLICE_MASK;

	wa_write_clr_set(wal, steering_reg, mcr_mask, mcr);
}

static void debug_dump_steering(struct intel_gt *gt)
{
	struct drm_printer p = drm_debug_printer("MCR Steering:");

	if (drm_debug_enabled(DRM_UT_DRIVER))
		intel_gt_mcr_report_steering(&p, gt, false);
}

static void __add_mcr_wa(struct intel_gt *gt, struct i915_wa_list *wal,
			 unsigned int slice, unsigned int subslice)
{
	__set_mcr_steering(wal, GEN8_MCR_SELECTOR, slice, subslice);

	gt->default_steering.groupid = slice;
	gt->default_steering.instanceid = subslice;

	debug_dump_steering(gt);
}

static void
icl_wa_init_mcr(struct intel_gt *gt, struct i915_wa_list *wal)
{
	const struct sseu_dev_info *sseu = &gt->info.sseu;
	unsigned int subslice;

	GEM_BUG_ON(GRAPHICS_VER(gt->i915) < 11);
	GEM_BUG_ON(hweight8(sseu->slice_mask) > 1);

	/*
	 * Although a platform may have subslices, we need to always steer
	 * reads to the lowest instance that isn't fused off.  When Render
	 * Power Gating is enabled, grabbing forcewake will only power up a
	 * single subslice (the "minconfig") if there isn't a real workload
	 * that needs to be run; this means that if we steer register reads to
	 * one of the higher subslices, we run the risk of reading back 0's or
	 * random garbage.
	 */
	subslice = __ffs(intel_sseu_get_hsw_subslices(sseu, 0));

	/*
	 * If the subslice we picked above also steers us to a valid L3 bank,
	 * then we can just rely on the default steering and won't need to
	 * worry about explicitly re-steering L3BANK reads later.
	 */
	if (gt->info.l3bank_mask & BIT(subslice))
		gt->steering_table[L3BANK] = NULL;

	__add_mcr_wa(gt, wal, 0, subslice);
}

static void
xehp_init_mcr(struct intel_gt *gt, struct i915_wa_list *wal)
{
	const struct sseu_dev_info *sseu = &gt->info.sseu;
	unsigned long slice, subslice = 0, slice_mask = 0;
	u32 lncf_mask = 0;
	int i;

	/*
	 * On Xe_HP the steering increases in complexity. There are now several
	 * more units that require steering and we're not guaranteed to be able
	 * to find a common setting for all of them. These are:
	 * - GSLICE (fusable)
	 * - DSS (sub-unit within gslice; fusable)
	 * - L3 Bank (fusable)
	 * - MSLICE (fusable)
	 * - LNCF (sub-unit within mslice; always present if mslice is present)
	 *
	 * We'll do our default/implicit steering based on GSLICE (in the
	 * sliceid field) and DSS (in the subsliceid field).  If we can
	 * find overlap between the valid MSLICE and/or LNCF values with
	 * a suitable GSLICE, then we can just re-use the default value and
	 * skip and explicit steering at runtime.
	 *
	 * We only need to look for overlap between GSLICE/MSLICE/LNCF to find
	 * a valid sliceid value.  DSS steering is the only type of steering
	 * that utilizes the 'subsliceid' bits.
	 *
	 * Also note that, even though the steering domain is called "GSlice"
	 * and it is encoded in the register using the gslice format, the spec
	 * says that the combined (geometry | compute) fuse should be used to
	 * select the steering.
	 */

	/* Find the potential gslice candidates */
	slice_mask = intel_slicemask_from_xehp_dssmask(sseu->subslice_mask,
						       GEN_DSS_PER_GSLICE);

	/*
	 * Find the potential LNCF candidates.  Either LNCF within a valid
	 * mslice is fine.
	 */
	for_each_set_bit(i, &gt->info.mslice_mask, GEN12_MAX_MSLICES)
		lncf_mask |= (0x3 << (i * 2));

	/*
	 * Are there any sliceid values that work for both GSLICE and LNCF
	 * steering?
	 */
	if (slice_mask & lncf_mask) {
		slice_mask &= lncf_mask;
		gt->steering_table[LNCF] = NULL;
	}

	/* How about sliceid values that also work for MSLICE steering? */
	if (slice_mask & gt->info.mslice_mask) {
		slice_mask &= gt->info.mslice_mask;
		gt->steering_table[MSLICE] = NULL;
	}

	slice = __ffs(slice_mask);
	subslice = intel_sseu_find_first_xehp_dss(sseu, GEN_DSS_PER_GSLICE, slice) %
		GEN_DSS_PER_GSLICE;

	__add_mcr_wa(gt, wal, slice, subslice);

	/*
	 * SQIDI ranges are special because they use different steering
	 * registers than everything else we work with.  On XeHP SDV and
	 * DG2-G10, any value in the steering registers will work fine since
	 * all instances are present, but DG2-G11 only has SQIDI instances at
	 * ID's 2 and 3, so we need to steer to one of those.  For simplicity
	 * we'll just steer to a hardcoded "2" since that value will work
	 * everywhere.
	 */
	__set_mcr_steering(wal, MCFG_MCR_SELECTOR, 0, 2);
	__set_mcr_steering(wal, SF_MCR_SELECTOR, 0, 2);

	/*
	 * On DG2, GAM registers have a dedicated steering control register
	 * and must always be programmed to a hardcoded groupid of "1."
	 */
	if (IS_DG2(gt->i915))
		__set_mcr_steering(wal, GAM_MCR_SELECTOR, 1, 0);
}

static void
pvc_init_mcr(struct intel_gt *gt, struct i915_wa_list *wal)
{
	unsigned int dss;

	/*
	 * Setup implicit steering for COMPUTE and DSS ranges to the first
	 * non-fused-off DSS.  All other types of MCR registers will be
	 * explicitly steered.
	 */
	dss = intel_sseu_find_first_xehp_dss(&gt->info.sseu, 0, 0);
	__add_mcr_wa(gt, wal, dss / GEN_DSS_PER_CSLICE, dss % GEN_DSS_PER_CSLICE);
}

/*
 * Though there are per-engine instances of these registers,
 * they retain their value through engine resets and should
 * only be provided on the GT workaround list rather than
 * the engine-specific workaround list.
 */
static void
wa_14011060649(struct intel_gt *gt, struct i915_wa_list *wal)
{
	struct intel_engine_cs *engine;
	int id;

	for_each_engine(engine, gt, id) {
		if (engine->class != VIDEO_DECODE_CLASS ||
		    (engine->instance % 2))
			continue;

		wa_write_or(wal, VDBOX_CGCTL3F10(engine->mmio_base),
			    IECPUNIT_CLKGATE_DIS);
	}
}

static void
gen12_gt_workarounds_init(struct intel_gt *gt, struct i915_wa_list *wal)
{
	icl_wa_init_mcr(gt, wal);

	/* Wa_14011060649:tgl,rkl,dg1,adl-s,adl-p */
	wa_14011060649(gt, wal);

	/* Wa_14011059788:tgl,rkl,adl-s,dg1,adl-p */
	wa_mcr_write_or(wal, GEN10_DFR_RATIO_EN_AND_CHICKEN, DFR_DISABLE);
}

static void
dg1_gt_workarounds_init(struct intel_gt *gt, struct i915_wa_list *wal)
{
	struct drm_i915_private *i915 = gt->i915;

	gen12_gt_workarounds_init(gt, wal);

	/* Wa_1409420604:dg1 */
	if (IS_DG1(i915))
		wa_mcr_write_or(wal,
				SUBSLICE_UNIT_LEVEL_CLKGATE2,
				CPSSUNIT_CLKGATE_DIS);

	/* Wa_1408615072:dg1 */
	/* Empirical testing shows this register is unaffected by engine reset. */
	if (IS_DG1(i915))
		wa_write_or(wal, UNSLICE_UNIT_LEVEL_CLKGATE2,
			    VSUNIT_CLKGATE_DIS_TGL);
}

static void
dg2_gt_workarounds_init(struct intel_gt *gt, struct i915_wa_list *wal)
{
	struct intel_engine_cs *engine;
	int id;

	xehp_init_mcr(gt, wal);

	/* Wa_14011060649:dg2 */
	wa_14011060649(gt, wal);

	/*
	 * Although there are per-engine instances of these registers,
	 * they technically exist outside the engine itself and are not
	 * impacted by engine resets.  Furthermore, they're part of the
	 * GuC blacklist so trying to treat them as engine workarounds
	 * will result in GuC initialization failure and a wedged GPU.
	 */
	for_each_engine(engine, gt, id) {
		if (engine->class != VIDEO_DECODE_CLASS)
			continue;

		/* Wa_16010515920:dg2_g10 */
		if (IS_DG2_GRAPHICS_STEP(gt->i915, G10, STEP_A0, STEP_B0))
			wa_write_or(wal, VDBOX_CGCTL3F18(engine->mmio_base),
				    ALNUNIT_CLKGATE_DIS);
	}

	if (IS_DG2_G10(gt->i915)) {
		/* Wa_22010523718:dg2 */
		wa_write_or(wal, UNSLICE_UNIT_LEVEL_CLKGATE,
			    CG3DDISCFEG_CLKGATE_DIS);

		/* Wa_14011006942:dg2 */
		wa_mcr_write_or(wal, GEN11_SUBSLICE_UNIT_LEVEL_CLKGATE,
				DSS_ROUTER_CLKGATE_DIS);
	}

	if (IS_DG2_GRAPHICS_STEP(gt->i915, G10, STEP_A0, STEP_B0) ||
	    IS_DG2_GRAPHICS_STEP(gt->i915, G11, STEP_A0, STEP_B0)) {
		/* Wa_14012362059:dg2 */
		wa_mcr_write_or(wal, XEHP_MERT_MOD_CTRL, FORCE_MISS_FTLB);
	}

	if (IS_DG2_GRAPHICS_STEP(gt->i915, G10, STEP_A0, STEP_B0)) {
		/* Wa_14010948348:dg2_g10 */
		wa_write_or(wal, UNSLCGCTL9430, MSQDUNIT_CLKGATE_DIS);

		/* Wa_14011037102:dg2_g10 */
		wa_write_or(wal, UNSLCGCTL9444, LTCDD_CLKGATE_DIS);

		/* Wa_14011371254:dg2_g10 */
		wa_mcr_write_or(wal, XEHP_SLICE_UNIT_LEVEL_CLKGATE, NODEDSS_CLKGATE_DIS);

		/* Wa_14011431319:dg2_g10 */
		wa_write_or(wal, UNSLCGCTL9440, GAMTLBOACS_CLKGATE_DIS |
			    GAMTLBVDBOX7_CLKGATE_DIS |
			    GAMTLBVDBOX6_CLKGATE_DIS |
			    GAMTLBVDBOX5_CLKGATE_DIS |
			    GAMTLBVDBOX4_CLKGATE_DIS |
			    GAMTLBVDBOX3_CLKGATE_DIS |
			    GAMTLBVDBOX2_CLKGATE_DIS |
			    GAMTLBVDBOX1_CLKGATE_DIS |
			    GAMTLBVDBOX0_CLKGATE_DIS |
			    GAMTLBKCR_CLKGATE_DIS |
			    GAMTLBGUC_CLKGATE_DIS |
			    GAMTLBBLT_CLKGATE_DIS);
		wa_write_or(wal, UNSLCGCTL9444, GAMTLBGFXA0_CLKGATE_DIS |
			    GAMTLBGFXA1_CLKGATE_DIS |
			    GAMTLBCOMPA0_CLKGATE_DIS |
			    GAMTLBCOMPA1_CLKGATE_DIS |
			    GAMTLBCOMPB0_CLKGATE_DIS |
			    GAMTLBCOMPB1_CLKGATE_DIS |
			    GAMTLBCOMPC0_CLKGATE_DIS |
			    GAMTLBCOMPC1_CLKGATE_DIS |
			    GAMTLBCOMPD0_CLKGATE_DIS |
			    GAMTLBCOMPD1_CLKGATE_DIS |
			    GAMTLBMERT_CLKGATE_DIS   |
			    GAMTLBVEBOX3_CLKGATE_DIS |
			    GAMTLBVEBOX2_CLKGATE_DIS |
			    GAMTLBVEBOX1_CLKGATE_DIS |
			    GAMTLBVEBOX0_CLKGATE_DIS);

		/* Wa_14010569222:dg2_g10 */
		wa_write_or(wal, UNSLICE_UNIT_LEVEL_CLKGATE,
			    GAMEDIA_CLKGATE_DIS);

		/* Wa_14011028019:dg2_g10 */
		wa_mcr_write_or(wal, SSMCGCTL9530, RTFUNIT_CLKGATE_DIS);

		/* Wa_14010680813:dg2_g10 */
		wa_mcr_write_or(wal, XEHP_GAMSTLB_CTRL,
				CONTROL_BLOCK_CLKGATE_DIS |
				EGRESS_BLOCK_CLKGATE_DIS |
				TAG_BLOCK_CLKGATE_DIS);
	}

	/* Wa_14014830051:dg2 */
	wa_mcr_write_clr(wal, SARB_CHICKEN1, COMP_CKN_IN);

	/*
	 * The following are not actually "workarounds" but rather
	 * recommended tuning settings documented in the bspec's
	 * performance guide section.
	 */
	wa_mcr_write_or(wal, XEHP_SQCM, EN_32B_ACCESS);

	/* Wa_18018781329 */
	wa_mcr_write_or(wal, RENDER_MOD_CTRL, FORCE_MISS_FTLB);
	wa_mcr_write_or(wal, COMP_MOD_CTRL, FORCE_MISS_FTLB);
	wa_mcr_write_or(wal, XEHP_VDBX_MOD_CTRL, FORCE_MISS_FTLB);
	wa_mcr_write_or(wal, XEHP_VEBX_MOD_CTRL, FORCE_MISS_FTLB);
	wa_mcr_write_or(wal, BLT_MOD_CTRL, FORCE_MISS_FTLB);

	/* Wa_1509235366:dg2 */
	wa_mcr_write_or(wal, XEHP_GAMCNTRL_CTRL,
			INVALIDATION_BROADCAST_MODE_DIS | GLOBAL_INVALIDATION_MODE);

	/* Wa_14010648519:dg2 */
	wa_mcr_write_or(wal, XEHP_L3NODEARBCFG, XEHP_LNESPARE);
}

static void
pvc_gt_workarounds_init(struct intel_gt *gt, struct i915_wa_list *wal)
{
	pvc_init_mcr(gt, wal);

	if (i915_modparams.enable_force_miss_ftlb) {
		/* Wa_18018781329 */
		wa_mcr_write_or(wal, RENDER_MOD_CTRL, FORCE_MISS_FTLB);
		wa_mcr_write_or(wal, COMP_MOD_CTRL, FORCE_MISS_FTLB);
		wa_mcr_write_or(wal, BLT_MOD_CTRL, FORCE_MISS_FTLB);

		if (VDBOX_MASK(gt))
			wa_mcr_write_or(wal, XEHP_VDBX_MOD_CTRL, FORCE_MISS_FTLB);
		if (VEBOX_MASK(gt))
			wa_mcr_write_or(wal, XEHP_VEBX_MOD_CTRL, FORCE_MISS_FTLB);
	}

	/* Wa_16016694945 */
	wa_mcr_masked_en(wal, XEHPC_LNCFMISCCFGREG0, XEHPC_OVRLSCCC);

	/*
	 * Wa_14015795083
	 * Apply to all PVC but don't verify it on PVC A0 steps, as this Wa is
	 * dependent on clearing GEN12_DOP_CLOCK_GATE_LOCK Lock bit by
	 * respective firmware. PVC A0 steps may not have that firmware fix.
	 */
	if (IS_PVC_BD_STEP(gt->i915, STEP_A0, STEP_B0))
		wa_mcr_add(wal, GEN8_MISCCPCTL, GEN12_DOP_CLOCK_GATE_RENDER_ENABLE, 0, 0, false);
	else
		wa_mcr_write_clr(wal, GEN8_MISCCPCTL, GEN12_DOP_CLOCK_GATE_RENDER_ENABLE);

	if (IS_PVC_BD_STEP(gt->i915, STEP_A0, STEP_B0)) {
		/* Wa_14011780169:pvc */
		wa_write_or(wal, UNSLCGCTL9440, GAMTLBOACS_CLKGATE_DIS |
			    GAMTLBVDBOX7_CLKGATE_DIS |
			    GAMTLBVDBOX6_CLKGATE_DIS |
			    GAMTLBVDBOX5_CLKGATE_DIS |
			    GAMTLBVDBOX4_CLKGATE_DIS |
			    GAMTLBVDBOX3_CLKGATE_DIS |
			    GAMTLBVDBOX2_CLKGATE_DIS |
			    GAMTLBVDBOX1_CLKGATE_DIS |
			    GAMTLBVDBOX0_CLKGATE_DIS |
			    GAMTLBKCR_CLKGATE_DIS |
			    GAMTLBGUC_CLKGATE_DIS |
			    GAMTLBBLT_CLKGATE_DIS);
		wa_write_or(wal, UNSLCGCTL9444, GAMTLBGFXA0_CLKGATE_DIS |
			    GAMTLBGFXA1_CLKGATE_DIS |
			    GAMTLBCOMPA0_CLKGATE_DIS |
			    GAMTLBCOMPA1_CLKGATE_DIS |
			    GAMTLBCOMPB0_CLKGATE_DIS |
			    GAMTLBCOMPB1_CLKGATE_DIS |
			    GAMTLBCOMPC0_CLKGATE_DIS |
			    GAMTLBCOMPC1_CLKGATE_DIS |
			    GAMTLBCOMPD0_CLKGATE_DIS |
			    GAMTLBCOMPD1_CLKGATE_DIS |
			    GAMTLBMERT_CLKGATE_DIS   |
			    GAMTLBVEBOX3_CLKGATE_DIS |
			    GAMTLBVEBOX2_CLKGATE_DIS |
			    GAMTLBVEBOX1_CLKGATE_DIS |
			    GAMTLBVEBOX0_CLKGATE_DIS);

		/*
		 * Wa_1508060568:pvc
		 * Wa_16011235395:pvc
		 * Wa_14011776591:pvc Note: Additional registers required for this Wa
		 */
		wa_write_or(wal, UNSLCGCTL9430, UNSLCG_FTLUNIT_CLKGATE_DIS);

		/* Wa_14011776591:pvc Note: Additional register required for this Wa */
		wa_write_or(wal, UNSLICE_UNIT_LEVEL_CLKGATE2, FTLUNIT_CLKGATE_DIS);
		wa_mcr_write_or(wal, SUBSLICE_UNIT_LEVEL_CLKGATE2, SSMCG_FTLUNIT_CLKGATE_DIS);

		/* Wa_16011254478:pvc */
		wa_mcr_write_or(wal, INF_UNIT_LEVEL_CLKGATE, MCR_CLKGATE_DIS);
		wa_mcr_write_or(wal, SCCGCTL94D0, SCCG_SMCR_CLKGATE_DIS);
		wa_mcr_write_or(wal, SSMCGCTL9520, SSMCG_SMCR_CLKGATE_DIS);
		wa_write_or(wal, UNSLCGCTL9430, UNSLCG_MCRUNIT_CLKGATE_DIS);
		wa_write_or(wal, UNSLCGCTL9444, SMCR_CLKGATE_DIS);

		/* Wa_16011062782:pvc */
		wa_mcr_masked_en(wal, XEHPC_LNCFMISCCFGREG0,
				 XEHPC_DIS256BREQGLB | XEHPC_DIS128BREQ);

		/* Wa_14010847520:pvc */
		wa_mcr_write_or(wal, GEN12_LTCDREG, SLPDIS);
	}

	/*
	 * Wa:16023902795
	 * Promote all TLB invalidations across all engines, regardless of activity.
	 */
	wa_mcr_write_or(wal, XEHP_GAMSTLB_CTRL, INVALIDATE_ENTIRE_STLB);
}

static void
xelpg_gt_workarounds_init(struct intel_gt *gt, struct i915_wa_list *wal)
{
	if (IS_MTL_GRAPHICS_STEP(gt->i915, M, STEP_A0, STEP_B0) ||
	    IS_MTL_GRAPHICS_STEP(gt->i915, P, STEP_A0, STEP_B0)) {
		/* Wa_14014830051 */
		wa_mcr_write_clr(wal, SARB_CHICKEN1, COMP_CKN_IN);

		/* Wa_18018781329 */
		wa_mcr_write_or(wal, RENDER_MOD_CTRL, FORCE_MISS_FTLB);
		wa_mcr_write_or(wal, COMP_MOD_CTRL, FORCE_MISS_FTLB);
	}

	/*
	 * Unlike older platforms, we no longer setup implicit steering here;
	 * all MCR accesses are explicitly steered.
	 */
	debug_dump_steering(gt);
}

static void
xelpmp_gt_workarounds_init(struct intel_gt *gt, struct i915_wa_list *wal)
{
	if (IS_MTL_MEDIA_STEP(gt->i915, STEP_A0, STEP_B0)) {
		/*
		 * Wa_18018781329
		 *
		 * Note that although these registers are MCR on the primary
		 * GT, the media GT's versions are regular singleton registers.
		 */
		wa_write_or(wal, XELPMP_GSC_MOD_CTRL, FORCE_MISS_FTLB);
		wa_write_or(wal, XELPMP_VDBX_MOD_CTRL, FORCE_MISS_FTLB);
		wa_write_or(wal, XELPMP_VEBX_MOD_CTRL, FORCE_MISS_FTLB);
	}

	debug_dump_steering(gt);
}

/*
 * The bspec performance guide has recommended MMIO tuning settings.  These
 * aren't truly "workarounds" but we want to program them through the
 * workaround infrastructure to make sure they're (re)applied at the proper
 * times.
 *
 * The programming in this function is for settings that persist through
 * engine resets and also are not part of any engine's register state context.
 * I.e., settings that only need to be re-applied in the event of a full GT
 * reset.
 */
static void gt_tuning_settings(struct intel_gt *gt, struct i915_wa_list *wal)
{
	if (IS_PONTEVECCHIO(gt->i915)) {
		wa_mcr_write(wal, XEHPC_L3SCRUB,
			     SCRUB_CL_DWNGRADE_SHARED | SCRUB_RATE_4B_PER_CLK);
		wa_mcr_masked_en(wal, XEHPC_LNCFMISCCFGREG0, XEHPC_HOSTCACHEEN);
	}

	if (IS_DG2(gt->i915))
		wa_mcr_write_or(wal, XEHP_L3SCQREG7, BLEND_FILL_CACHING_OPT_DIS);
}

static void
gt_init_workarounds(struct intel_gt *gt, struct i915_wa_list *wal)
{
	struct drm_i915_private *i915 = gt->i915;

	gt_tuning_settings(gt, wal);

	if (gt->type == GT_MEDIA) {
		if (MEDIA_VER(i915) >= 13)
			xelpmp_gt_workarounds_init(gt, wal);
		else
			MISSING_CASE(MEDIA_VER(i915));

		return;
	}

	if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 70))
		xelpg_gt_workarounds_init(gt, wal);
	else if (IS_PONTEVECCHIO(i915))
		pvc_gt_workarounds_init(gt, wal);
	else if (IS_DG2(i915))
		dg2_gt_workarounds_init(gt, wal);
	else if (IS_DG1(i915))
		dg1_gt_workarounds_init(gt, wal);
	else
		gen12_gt_workarounds_init(gt, wal);
}

void intel_gt_init_workarounds(struct intel_gt *gt)
{
	struct i915_wa_list *wal = &gt->wa_list;

	if (IS_SRIOV_VF(gt->i915))
		return;

	wa_init(wal, "GT", "global");
	gt_init_workarounds(gt, wal);
}

static enum forcewake_domains
wal_get_fw(struct intel_uncore *uncore, const struct i915_wa_list *wal,
	   unsigned int op)
{
	enum forcewake_domains fw = 0;
	struct i915_wa *wa;
	unsigned int i;

	for (i = 0, wa = wal->list; i < wal->count; i++, wa++)
		fw |= intel_uncore_forcewake_for_reg(uncore,
						     wa->reg,
						     op);

	return fw;
}

static const char *valid(bool state)
{
	return state ? "valid" : "invalid";
}

static bool wa_ok(const struct i915_wa *wa, bool mcr,
		  u32 cur, const char *name, void *data)
{
	return ((cur ^ wa->set) & wa->read) == 0;
}

static bool
wa_show(const struct i915_wa *wa, bool mcr,
	u32 cur, const char *name, void *data)
{
	struct drm_printer *p = data;
	bool ok = wa_ok(wa, mcr, cur, name, data);

	drm_printf(p,
		   "reg:%x%s { raw:%08x, mask:%08x, value:%08x, expected:%08x, %s }\n",
		   i915_mmio_reg_offset(wa->reg), mcr ? "*" : "",
		   cur, wa->read,
		   cur & wa->read, wa->set & wa->read,
		   valid(ok));

	return ok;
}

static bool
wa_verify(const struct i915_wa *wa, bool mcr,
	  u32 cur, const char *name, void *data)
{
	const char *from = data;

	if (!wa_ok(wa, mcr, cur, name, data)) {
		DRM_ERROR("%s workaround lost on %s! (reg[%x%s]=0x%x, relevant bits were 0x%x vs expected 0x%x)\n",
			  name, from, i915_mmio_reg_offset(wa->reg),
			  mcr ? "*" : "",
			  cur, cur & wa->read, wa->set & wa->read);

		return false;
	}

	return true;
}

static void
wa_list_apply(struct intel_gt *gt, const struct i915_wa_list *wal)
{
	struct intel_uncore *uncore = gt->uncore;
	enum forcewake_domains fw;
	unsigned long flags;
	struct i915_wa *wa;
	unsigned int i;

	if (!wal->count)
		return;

	fw = wal_get_fw(uncore, wal, FW_REG_READ | FW_REG_WRITE);

	intel_gt_mcr_lock(gt, &flags);
	spin_lock(&uncore->lock);
	intel_uncore_forcewake_get__locked(uncore, fw);

	for (i = 0, wa = wal->list; i < wal->count; i++, wa++) {
		u32 val, old = 0;

		/* open-coded rmw due to steering */
		if (wa->clr)
			old = wa->is_mcr ?
				intel_gt_mcr_read_any_fw(gt, wa->mcr_reg) :
				intel_uncore_read_fw(uncore, wa->reg);
		val = (old & ~wa->clr) | wa->set;
		if (val != old || !wa->clr) {
			if (wa->is_mcr)
				intel_gt_mcr_multicast_write_fw(gt, wa->mcr_reg, val);
			else
				intel_uncore_write_fw(uncore, wa->reg, val);
		}

		if (IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GEM)) {
			u32 val = wa->is_mcr ?
				intel_gt_mcr_read_any_fw(gt, wa->mcr_reg) :
				intel_uncore_read_fw(uncore, wa->reg);

			wa_verify(wa, true, val, wal->name, "application");
		}
	}

	intel_uncore_forcewake_put__locked(uncore, fw);
	spin_unlock(&uncore->lock);
	intel_gt_mcr_unlock(gt, flags);
}

void intel_gt_apply_workarounds(struct intel_gt *gt)
{
	wa_list_apply(gt, &gt->wa_list);
}

static int wa_list_verify(struct intel_gt *gt,
			  const struct i915_wa_list *wal,
			  bool (*verify)(const struct i915_wa *wa,
					 bool mcr,
					 u32 cur,
					 const char *name,
					 void *data),
			  void *data)
{
	struct intel_uncore *uncore = gt->uncore;
	struct i915_wa *wa;
	enum forcewake_domains fw;
	unsigned long flags;
	unsigned int i;
	int err = 0;

	if (!wal->count)
		return 0;

	fw = wal_get_fw(uncore, wal, FW_REG_READ);

	intel_gt_mcr_lock(gt, &flags);
	spin_lock(&uncore->lock);
	intel_uncore_forcewake_get__locked(uncore, fw);

	for (i = 0, wa = wal->list; i < wal->count; i++, wa++)
		if (!verify(wa, true, wa->is_mcr ?
			    intel_gt_mcr_read_any_fw(gt, wa->mcr_reg) :
			    intel_uncore_read_fw(uncore, wa->reg),
			    wal->name, data))
			err = -EINVAL;

	intel_uncore_forcewake_put__locked(uncore, fw);
	spin_unlock(&uncore->lock);
	intel_gt_mcr_unlock(gt, flags);

	return err;
}

bool intel_gt_verify_workarounds(struct intel_gt *gt, const char *from)
{
	return wa_list_verify(gt, &gt->wa_list, wa_verify, (void *)from) == 0;
}

int intel_gt_show_workarounds(struct drm_printer *p,
			      struct intel_gt *gt,
			      const struct i915_wa_list * const wal)
{
	return wa_list_verify(gt, wal, wa_show, p);
}

__maybe_unused
static bool is_nonpriv_flags_valid(u32 flags)
{
	/* Check only valid flag bits are set */
	if (flags & ~RING_FORCE_TO_NONPRIV_MASK_VALID)
		return false;

	/* NB: Only 3 out of 4 enum values are valid for access field */
	if ((flags & RING_FORCE_TO_NONPRIV_ACCESS_MASK) ==
	    RING_FORCE_TO_NONPRIV_ACCESS_INVALID)
		return false;

	return true;
}

static void
whitelist_reg_ext(struct i915_wa_list *wal, i915_reg_t reg, u32 flags)
{
	struct i915_wa wa = {
		.reg = reg
	};

	if (GEM_DEBUG_WARN_ON(wal->count >= RING_MAX_NONPRIV_SLOTS))
		return;

	if (GEM_DEBUG_WARN_ON(!is_nonpriv_flags_valid(flags)))
		return;

	wa.reg.reg |= flags;
	_wa_add(wal, &wa);
}

static void
whitelist_mcr_reg_ext(struct i915_wa_list *wal, i915_mcr_reg_t reg, u32 flags)
{
	struct i915_wa wa = {
		.mcr_reg = reg,
		.is_mcr = 1,
	};

	if (GEM_DEBUG_WARN_ON(wal->count >= RING_MAX_NONPRIV_SLOTS))
		return;

	if (GEM_DEBUG_WARN_ON(!is_nonpriv_flags_valid(flags)))
		return;

	wa.mcr_reg.reg |= flags;
	_wa_add(wal, &wa);
}

static void
whitelist_reg(struct i915_wa_list *wal, i915_reg_t reg)
{
	whitelist_reg_ext(wal, reg, RING_FORCE_TO_NONPRIV_ACCESS_RW);
}

static void
whitelist_mcr_reg(struct i915_wa_list *wal, i915_mcr_reg_t reg)
{
	whitelist_mcr_reg_ext(wal, reg, RING_FORCE_TO_NONPRIV_ACCESS_RW);
}

static void allow_read_ctx_timestamp(struct intel_engine_cs *engine)
{
	struct i915_wa_list *w = &engine->whitelist;

	if (engine->class != RENDER_CLASS)
		whitelist_reg_ext(w,
				  RING_CTX_TIMESTAMP(engine->mmio_base),
				  RING_FORCE_TO_NONPRIV_ACCESS_RD);
}

static void tgl_whitelist_build(struct intel_engine_cs *engine)
{
	struct i915_wa_list *w = &engine->whitelist;

	allow_read_ctx_timestamp(engine);

	switch (engine->class) {
	case RENDER_CLASS:
		/*
		 * WaAllowPMDepthAndInvocationCountAccessFromUMD:tgl
		 * Wa_1408556865:tgl
		 *
		 * This covers 4 registers which are next to one another :
		 *   - PS_INVOCATION_COUNT
		 *   - PS_INVOCATION_COUNT_UDW
		 *   - PS_DEPTH_COUNT
		 *   - PS_DEPTH_COUNT_UDW
		 */
		whitelist_reg_ext(w, PS_INVOCATION_COUNT,
				  RING_FORCE_TO_NONPRIV_ACCESS_RD |
				  RING_FORCE_TO_NONPRIV_RANGE_4);

		/*
		 * Wa_1808121037:tgl
		 * Wa_14012131227:dg1
		 * Wa_1508744258:tgl,rkl,dg1,adl-s,adl-p
		 */
		whitelist_reg(w, GEN7_COMMON_SLICE_CHICKEN1);

		/* Wa_1806527549:tgl */
		whitelist_reg(w, HIZ_CHICKEN);

		/* Required by recommended tuning setting (not a workaround) */
		whitelist_reg(w, GEN11_COMMON_SLICE_CHICKEN3);

		break;
	default:
		break;
	}
}

static void engine_debug_init_whitelist(struct intel_engine_cs *engine,
					struct i915_wa_list *wal)
{
}

static void engine_debug_fini_whitelist(struct intel_engine_cs *engine,
					struct i915_wa_list *wal)
{
}

static void dg2_whitelist_build(struct intel_engine_cs *engine)
{
	struct i915_wa_list *w = &engine->whitelist;

	switch (engine->class) {
	case RENDER_CLASS:
		/*
		 * Wa_1507100340:dg2_g10
		 *
		 * This covers 4 registers which are next to one another :
		 *   - PS_INVOCATION_COUNT
		 *   - PS_INVOCATION_COUNT_UDW
		 *   - PS_DEPTH_COUNT
		 *   - PS_DEPTH_COUNT_UDW
		 */
		if (IS_DG2_GRAPHICS_STEP(engine->i915, G10, STEP_A0, STEP_B0))
			whitelist_reg_ext(w, PS_INVOCATION_COUNT,
					  RING_FORCE_TO_NONPRIV_ACCESS_RD |
					  RING_FORCE_TO_NONPRIV_RANGE_4);

		/* Required by recommended tuning setting (not a workaround) */
		whitelist_mcr_reg(w, XEHP_COMMON_SLICE_CHICKEN3);

		break;
	case COMPUTE_CLASS:
		/* Wa_16011157294:dg2_g10 */
		if (IS_DG2_GRAPHICS_STEP(engine->i915, G10, STEP_A0, STEP_B0))
			whitelist_reg(w, GEN9_CTX_PREEMPT_REG);
		break;
	default:
		break;
	}
}

static void blacklist_trtt(struct intel_engine_cs *engine)
{
	struct i915_wa_list *w = &engine->whitelist;

	/*
	 * Prevent read/write access to [0x4400, 0x4600) which covers
	 * the TRTT range across all engines. Note that normally userspace
	 * cannot access the other engines' trtt control, but for simplicity
	 * we cover the entire range on each engine.
	 */
	whitelist_reg_ext(w, _MMIO(0x4400),
			  RING_FORCE_TO_NONPRIV_DENY |
			  RING_FORCE_TO_NONPRIV_RANGE_64);
	whitelist_reg_ext(w, _MMIO(0x4500),
			  RING_FORCE_TO_NONPRIV_DENY |
			  RING_FORCE_TO_NONPRIV_RANGE_64);
}

static void pvc_whitelist_build(struct intel_engine_cs *engine)
{
	/* Wa_16014440446:pvc */
	blacklist_trtt(engine);

	/* Wa_16017236439 - Blacklist BCS_SWCTRL */
	if (engine->class == COPY_ENGINE_CLASS)
		whitelist_reg_ext(&engine->whitelist,
				  BCS_ENGINE_SWCTL(engine->mmio_base),
				  RING_FORCE_TO_NONPRIV_DENY);
}

static void mtl_whitelist_build(struct intel_engine_cs *engine)
{
	struct i915_wa_list *w = &engine->whitelist;

	switch (engine->class) {
	case RENDER_CLASS:
		/* Required by recommended tuning setting (not a workaround) */
		whitelist_mcr_reg(w, XEHP_COMMON_SLICE_CHICKEN3);

		break;
	default:
		break;
	}
}

void intel_engine_init_whitelist(struct intel_engine_cs *engine)
{
	struct drm_i915_private *i915 = engine->i915;

	if (IS_SRIOV_VF(engine->i915))
		return;

	wa_init(&engine->whitelist, "whitelist", engine->name);

	if (IS_METEORLAKE(i915))
		mtl_whitelist_build(engine);
	else if (IS_PONTEVECCHIO(i915))
		pvc_whitelist_build(engine);
	else if (IS_DG2(i915))
		dg2_whitelist_build(engine);
	else
		tgl_whitelist_build(engine);

#if IS_ENABLED(CPTCFG_DRM_I915_DEBUGGER)
	if (i915->debuggers.enable_eu_debug)
		engine_debug_init_whitelist(engine, &engine->whitelist);
#endif
}

void intel_engine_apply_whitelist(struct intel_engine_cs *engine)
{
	enum forcewake_domains fw;
	const struct i915_wa_list *wal = &engine->whitelist;
	struct intel_uncore *uncore = engine->uncore;
	const u32 base = engine->mmio_base;
	struct i915_wa *wa;
	unsigned long flags;
	unsigned int i;

	fw = intel_uncore_forcewake_for_reg(uncore,
					    RING_FORCE_TO_NONPRIV(base, 0),
					    FW_REG_WRITE);

	spin_lock_irqsave(&uncore->lock, flags);
	intel_uncore_forcewake_get__locked(uncore, fw);

	for (i = 0, wa = wal->list; i < wal->count; i++, wa++)
		intel_uncore_write_fw(uncore,
				      RING_FORCE_TO_NONPRIV(base, i),
				      i915_mmio_reg_offset(wa->reg));

	/* And clear the rest just in case of garbage */
	for (; i < RING_MAX_NONPRIV_SLOTS; i++)
		intel_uncore_write_fw(uncore,
				      RING_FORCE_TO_NONPRIV(base, i),
				      i915_mmio_reg_offset(RING_NOPID(base)));

	intel_uncore_forcewake_put__locked(uncore, fw);
	spin_unlock_irqrestore(&uncore->lock, flags);
}

/*
 * engine_fake_wa_init(), a place holder to program the registers
 * which are not part of an official workaround defined by the
 * hardware team.
 * Adding programming of those register inside workaround will
 * allow utilizing wa framework to proper application and verification.
 */
static void
engine_fake_wa_init(struct intel_engine_cs *engine, struct i915_wa_list *wal)
{
	u8 mocs_w, mocs_r;

	/*
	 * RING_CMD_CCTL specifies the default MOCS entry that will be used
	 * by the command streamer when executing commands that don't have
	 * a way to explicitly specify a MOCS setting.  The default should
	 * usually reference whichever MOCS entry corresponds to uncached
	 * behavior, although use of a WB cached entry is recommended by the
	 * spec in certain circumstances on specific platforms.
	 */
	if (GRAPHICS_VER(engine->i915) >= 12) {
		mocs_r = engine->gt->mocs.uc_index;
		mocs_w = engine->gt->mocs.uc_index;

		if (HAS_L3_CCS_READ(engine->i915) &&
		    engine->class == COMPUTE_CLASS) {
			mocs_r = engine->gt->mocs.wb_index;

			/*
			 * Even on the few platforms where MOCS 0 is a
			 * legitimate table entry, it's never the correct
			 * setting to use here; we can assume the MOCS init
			 * just forgot to initialize wb_index.
			 */
			drm_WARN_ON(&engine->i915->drm, mocs_r == 0);
		}

		wa_masked_field_set(wal,
				    RING_CMD_CCTL(engine->mmio_base),
				    CMD_CCTL_MOCS_MASK,
				    CMD_CCTL_MOCS_OVERRIDE(mocs_w, mocs_r));
	}
}

static bool needs_wa_1308578152(struct intel_engine_cs *engine)
{
	return intel_sseu_find_first_xehp_dss(&engine->gt->info.sseu, 0, 0) >=
		GEN_DSS_PER_GSLICE;
}

static void
rcs_engine_wa_init(struct intel_engine_cs *engine, struct i915_wa_list *wal)
{
	struct drm_i915_private *i915 = engine->i915;

	if (IS_MTL_GRAPHICS_STEP(i915, M, STEP_A0, STEP_B0) ||
	    IS_MTL_GRAPHICS_STEP(i915, P, STEP_A0, STEP_B0)) {
		/* Wa_22014600077 */
		wa_mcr_masked_en(wal, GEN10_CACHE_MODE_SS,
				 ENABLE_EU_COUNT_FOR_TDL_FLUSH);
	}

	if (IS_MTL_GRAPHICS_STEP(i915, M, STEP_A0, STEP_B0) ||
	    IS_MTL_GRAPHICS_STEP(i915, P, STEP_A0, STEP_B0) ||
	    IS_DG2_GRAPHICS_STEP(i915, G10, STEP_B0, STEP_FOREVER) ||
	    IS_DG2_G11(i915) || IS_DG2_G12(i915)) {
		/* Wa_1509727124 */
		wa_mcr_masked_en(wal, GEN10_SAMPLER_MODE,
				 SC_DISABLE_POWER_OPTIMIZATION_EBB);

		/* Wa_22013037850 */
		wa_mcr_write_or(wal, LSC_CHICKEN_BIT_0_UDW,
				DISABLE_128B_EVICTION_COMMAND_UDW);
	}

	if (IS_DG2_GRAPHICS_STEP(i915, G10, STEP_B0, STEP_FOREVER) ||
	    IS_DG2_G11(i915) || IS_DG2_G12(i915) ||
	    IS_MTL_GRAPHICS_STEP(i915, M, STEP_A0, STEP_B0)) {
		/* Wa_22012856258 */
		wa_mcr_masked_en(wal, GEN8_ROW_CHICKEN2,
				 GEN12_DISABLE_READ_SUPPRESSION);
	}

	if (IS_DG2_GRAPHICS_STEP(i915, G11, STEP_A0, STEP_B0)) {
		/* Wa_14013392000:dg2_g11 */
		wa_mcr_masked_en(wal, GEN8_ROW_CHICKEN2, GEN12_ENABLE_LARGE_GRF_MODE);
	}

	if (IS_DG2_GRAPHICS_STEP(i915, G10, STEP_A0, STEP_B0) ||
	    IS_DG2_GRAPHICS_STEP(i915, G11, STEP_A0, STEP_B0)) {
		/* Wa_14012419201:dg2 */
		wa_mcr_masked_en(wal, GEN9_ROW_CHICKEN4,
				 GEN12_DISABLE_HDR_PAST_PAYLOAD_HOLD_FIX);
	}

	if (IS_DG2_GRAPHICS_STEP(i915, G10, STEP_B0, STEP_C0) ||
	    IS_DG2_G11(i915)) {
		/*
		 * Wa_22012826095:dg2
		 * Wa_22013059131:dg2
		 */
		wa_mcr_write_clr_set(wal, LSC_CHICKEN_BIT_0_UDW,
				     MAXREQS_PER_BANK,
				     REG_FIELD_PREP(MAXREQS_PER_BANK, 2));

		/* Wa_22013059131:dg2 */
		wa_mcr_write_or(wal, LSC_CHICKEN_BIT_0,
				FORCE_1_SUB_MESSAGE_PER_FRAGMENT);
	}

	/* Wa_1308578152:dg2_g10 when first gslice is fused off */
	if (IS_DG2_GRAPHICS_STEP(i915, G10, STEP_B0, STEP_C0) &&
	    needs_wa_1308578152(engine)) {
		wa_masked_dis(wal, GEN12_CS_DEBUG_MODE1_CCCSUNIT_BE_COMMON,
			      GEN12_REPLAY_MODE_GRANULARITY);
	}

	if (IS_DG2_GRAPHICS_STEP(i915, G10, STEP_B0, STEP_FOREVER) ||
	    IS_DG2_G11(i915) || IS_DG2_G12(i915)) {
		/*
		 * Wa_22010960976:dg2
		 * Wa_14013347512:dg2
		 */
		wa_mcr_masked_dis(wal, XEHP_HDC_CHICKEN0,
				  LSC_L1_FLUSH_CTL_3D_DATAPORT_FLUSH_EVENTS_MASK);
	}

	if (IS_DG2_GRAPHICS_STEP(i915, G10, STEP_A0, STEP_B0)) {
		/*
		 * Wa_1608949956:dg2_g10
		 * Wa_14010198302:dg2_g10
		 */
		wa_mcr_masked_en(wal, GEN8_ROW_CHICKEN,
				 MDQ_ARBITRATION_MODE | UGM_BACKUP_MODE);

		/*
		 * Wa_14010918519:dg2_g10
		 *
		 * LSC_CHICKEN_BIT_0 always reads back as 0 is this stepping,
		 * so ignoring verification.
		 */
		wa_mcr_add(wal, LSC_CHICKEN_BIT_0_UDW, 0,
			   FORCE_SLM_FENCE_SCOPE_TO_TILE | FORCE_UGM_FENCE_SCOPE_TO_TILE,
			   0, false);
	}

	if (IS_DG2_GRAPHICS_STEP(i915, G10, STEP_A0, STEP_B0))
		/* Wa_22010430635:dg2 */
		wa_mcr_masked_en(wal,
				 GEN9_ROW_CHICKEN4,
				 GEN12_DISABLE_GRF_CLEAR);

	/* Wa_14013202645:dg2 */
	if (IS_DG2_GRAPHICS_STEP(i915, G10, STEP_B0, STEP_C0) ||
	    IS_DG2_GRAPHICS_STEP(i915, G11, STEP_A0, STEP_B0))
		wa_mcr_write_or(wal, RT_CTRL, DIS_NULL_QUERY);

	/* Wa_22012532006:dg2 */
	if (IS_DG2_GRAPHICS_STEP(engine->i915, G10, STEP_A0, STEP_C0) ||
	    IS_DG2_GRAPHICS_STEP(engine->i915, G11, STEP_A0, STEP_B0))
		wa_mcr_masked_en(wal, GEN9_HALF_SLICE_CHICKEN7,
				 DG2_DISABLE_ROUND_ENABLE_ALLOW_FOR_SSLA);

	if (IS_DG2(i915)) {
		/* Wa_14015150844 */
		wa_mcr_add(wal, XEHP_HDC_CHICKEN0, 0,
			   _MASKED_BIT_ENABLE(DIS_ATOMIC_CHAINING_TYPED_WRITES),
			   0, true);
	}

	if (IS_DG2_GRAPHICS_STEP(i915, G11, STEP_B0, STEP_FOREVER) ||
	    IS_DG2_G10(i915)) {
		/* Wa_22014600077:dg2 */
		wa_mcr_add(wal, GEN10_CACHE_MODE_SS, 0,
			   _MASKED_BIT_ENABLE(ENABLE_EU_COUNT_FOR_TDL_FLUSH),
			   0 /* Wa_14012342262 write-only reg, so skip verification */,
			   true);
	}

	if (IS_ALDERLAKE_P(i915) || IS_ALDERLAKE_S(i915) || IS_DG1(i915) ||
	    IS_ROCKETLAKE(i915) || IS_TIGERLAKE(i915)) {
		/* Wa_1606931601:tgl,rkl,dg1,adl-s,adl-p */
		wa_mcr_masked_en(wal, GEN8_ROW_CHICKEN2, GEN12_DISABLE_EARLY_READ);

		/*
		 * Wa_1407928979:tgl A*
		 * Wa_18011464164:tgl[B0+],dg1[B0+]
		 * Wa_22010931296:tgl[B0+],dg1[B0+]
		 * Wa_14010919138:rkl,dg1,adl-s,adl-p
		 */
		wa_write_or(wal, GEN7_FF_THREAD_MODE,
			    GEN12_FF_TESSELATION_DOP_GATE_DISABLE);
	}

	if (IS_ALDERLAKE_P(i915) || IS_DG2(i915) || IS_ALDERLAKE_S(i915) ||
	    IS_DG1(i915) || IS_ROCKETLAKE(i915) || IS_TIGERLAKE(i915)) {
		/*
		 * Wa_1606700617:tgl,dg1,adl-p
		 * Wa_22010271021:tgl,rkl,dg1,adl-s,adl-p
		 * Wa_14010826681:tgl,dg1,rkl,adl-p
		 * Wa_18019627453:dg2
		 */
		wa_masked_en(wal,
			     GEN9_CS_DEBUG_MODE1,
			     FF_DOP_CLOCK_GATE_DISABLE);
	}

	if (IS_ALDERLAKE_P(i915) || IS_ALDERLAKE_S(i915) ||
	    IS_ROCKETLAKE(i915) || IS_TIGERLAKE(i915)) {
		/* Wa_1409804808 */
		wa_mcr_masked_en(wal, GEN8_ROW_CHICKEN2,
				 GEN12_PUSH_CONST_DEREF_HOLD_DIS);

		/* Wa_14010229206 */
		wa_mcr_masked_en(wal, GEN9_ROW_CHICKEN4, GEN12_DISABLE_TDL_PUSH);
	}

	if (IS_ROCKETLAKE(i915) || IS_TIGERLAKE(i915) || IS_ALDERLAKE_P(i915)) {
		/*
		 * Wa_1607297627
		 *
		 * On TGL and RKL there are multiple entries for this WA in the
		 * BSpec; some indicate this is an A0-only WA, others indicate
		 * it applies to all steppings so we trust the "all steppings."
		 */
		wa_masked_en(wal,
			     RING_PSMI_CTL(RENDER_RING_BASE),
			     GEN12_WAIT_FOR_EVENT_POWER_DOWN_DISABLE |
			     GEN8_RC_SEMA_IDLE_MSG_DISABLE);
	}

	if (IS_DG1(i915) || IS_ROCKETLAKE(i915) || IS_TIGERLAKE(i915) ||
	    IS_ALDERLAKE_S(i915) || IS_ALDERLAKE_P(i915)) {
		/* Wa_1406941453:tgl,rkl,dg1,adl-s,adl-p */
		wa_mcr_masked_en(wal,
				 GEN10_SAMPLER_MODE,
				 ENABLE_SMALLPL);
	}

	/*
	 * Intel platforms that support fine-grained preemption (i.e., gen9 and
	 * beyond) allow the kernel-mode driver to choose between two different
	 * options for controlling preemption granularity and behavior.
	 *
	 * Option 1 (hardware default):
	 *   Preemption settings are controlled in a global manner via
	 *   kernel-only register CS_DEBUG_MODE1 (0x20EC).  Any granularity
	 *   and settings chosen by the kernel-mode driver will apply to all
	 *   userspace clients.
	 *
	 * Option 2:
	 *   Preemption settings are controlled on a per-context basis via
	 *   register CS_CHICKEN1 (0x2580).  CS_CHICKEN1 is saved/restored on
	 *   context switch and is writable by userspace (e.g., via
	 *   MI_LOAD_REGISTER_IMMEDIATE instructions placed in a batch buffer)
	 *   which allows different userspace drivers/clients to select
	 *   different settings, or to change those settings on the fly in
	 *   response to runtime needs.  This option was known by name
	 *   "FtrPerCtxtPreemptionGranularityControl" at one time, although
	 *   that name is somewhat misleading as other non-granularity
	 *   preemption settings are also impacted by this decision.
	 *
	 * On Linux, our policy has always been to let userspace drivers
	 * control preemption granularity/settings (Option 2).  This was
	 * originally mandatory on gen9 to prevent ABI breakage (old gen9
	 * userspace developed before object-level preemption was enabled would
	 * not behave well if i915 were to go with Option 1 and enable that
	 * preemption in a global manner).  On gen9 each context would have
	 * object-level preemption disabled by default (see
	 * WaDisable3DMidCmdPreemption in gen9_ctx_workarounds_init), but
	 * userspace drivers could opt-in to object-level preemption as they
	 * saw fit.  For post-gen9 platforms, we continue to utilize Option 2;
	 * even though it is no longer necessary for ABI compatibility when
	 * enabling a new platform, it does ensure that userspace will be able
	 * to implement any workarounds that show up requiring temporary
	 * adjustments to preemption behavior at runtime.
	 *
	 * Notes/Workarounds:
	 *  - Wa_14015141709:  On DG2 and early steppings of MTL,
	 *      CS_CHICKEN1[0] does not disable object-level preemption as
	 *      it is supposed to (nor does CS_DEBUG_MODE1[0] if we had been
	 *      using Option 1).  Effectively this means userspace is unable
	 *      to disable object-level preemption on these platforms/steppings
	 *      despite the setting here.
	 *
	 *  - Wa_16013994831:  May require that userspace program
	 *      CS_CHICKEN1[10] when certain runtime conditions are true.
	 *      Userspace requires Option 2 to be in effect for their update of
	 *      CS_CHICKEN1[10] to be effective.
	 *
	 * Other workarounds may appear in the future that will also require
	 * Option 2 behavior to allow proper userspace implementation.
	 */
	wa_masked_en(wal,
		     GEN7_FF_SLICE_CS_CHICKEN1,
		     GEN9_FFSC_PERCTX_PREEMPT_CTRL);
}

static void
xcs_engine_wa_init(struct intel_engine_cs *engine, struct i915_wa_list *wal)
{
	struct drm_i915_private *i915 = engine->i915;

	/* WaKBLVECSSemaphoreWaitPoll:kbl */
	if (IS_KBL_GRAPHICS_STEP(i915, STEP_A0, STEP_F0)) {
		wa_write(wal,
			 RING_SEMA_WAIT_POLL(engine->mmio_base),
			 1);
	}

	/* Wa_16018031267, Wa_16018063123, Wa_16010961369:pvc */
	if (NEEDS_FASTCOLOR_BLT_WABB(engine))
		wa_masked_field_set(wal, ECOSKPD(engine->mmio_base),
				    XEHP_BLITTER_SCHEDULING_MODE_MASK,
				    XEHP_BLITTER_ROUND_ROBIN_MODE);
}

static void
ccs_engine_wa_init(struct intel_engine_cs *engine, struct i915_wa_list *wal)
{
	if (IS_PVC_CT_STEP(engine->i915, STEP_A0, STEP_C0)) {
		/* Wa_14014999345:pvc */
		wa_mcr_masked_en(wal, GEN10_CACHE_MODE_SS, DISABLE_ECC);
	}

	if (IS_PVC_CT_STEP(engine->i915, STEP_A0, STEP_B0)) {
		/* Wa_18015335494:pvc */
		wa_mcr_write_or(wal, GEN8_ROW_CHICKEN, FPU_RESIDUAL_DISABLE);

		/* Wa_16011764597:pvc */
		wa_mcr_write_or(wal, LSC_CHICKEN_BIT_0_UDW,
				DISABLE_MF_READ_FIFO_DEPTH_DECREASE |
				ENABLE_CREDIT_UNIFICATION);

		/* Wa_16013172390:pvc */
		wa_mcr_masked_en(wal, GADSS_CHICKEN, GADSS_128B_COMPRESSION_DISABLE_XEHPC);

		/* Wa_16012607674:pvc */
		wa_mcr_masked_en(wal, GADSS_CHICKEN,
				 GADSS_LINK_LAYER_DUMMY_DISABLE);
	}

	if (IS_PVC_BD_STEP(engine->i915, STEP_A0, STEP_B0)) {
		/* Wa_16011062782:pvc */
		wa_mcr_masked_field_set(wal, GADSS_CHICKEN,
					GADSS_COMPRESSION_MASK,
					GADSS_READ_COMPRESSION_DISABLE |
					GADSS_128B_COMPRESSION_DISABLE_XEHPC);
	}
}

/*
 * The bspec performance guide has recommended MMIO tuning settings.  These
 * aren't truly "workarounds" but we want to program them with the same
 * workaround infrastructure to ensure that they're automatically added to
 * the GuC save/restore lists, re-applied at the right times, and checked for
 * any conflicting programming requested by real workarounds.
 *
 * Programming settings should be added here only if their registers are not
 * part of an engine's register state context.  If a register is part of a
 * context, then any tuning settings should be programmed in an appropriate
 * function invoked by __intel_engine_init_ctx_wa().
 */
static void
add_render_compute_tuning_settings(struct drm_i915_private *i915,
				   struct i915_wa_list *wal)
{
	if (IS_DG2(i915))
		wa_mcr_write_clr_set(wal, RT_CTRL, STACKID_CTRL, STACKID_CTRL_512);

	/*
	 * This tuning setting proves beneficial only on ATS-M designs; the
	 * default "age based" setting is optimal on regular DG2 and other
	 * platforms.
	 */
	if (INTEL_INFO(i915)->tuning_thread_rr_after_dep)
		wa_mcr_masked_field_set(wal, GEN9_ROW_CHICKEN4, THREAD_EX_ARB_MODE,
					THREAD_EX_ARB_MODE_RR_AFTER_DEP);

	if (GRAPHICS_VER(i915) == 12 && GRAPHICS_VER_FULL(i915) < IP_VER(12, 50))
		wa_write_clr(wal, GEN12_GARBCNTL, GEN12_BUS_HASH_CTL_BIT_EXC);
}

/*
 * The workarounds in this function apply to shared registers in
 * the general render reset domain that aren't tied to a
 * specific engine.  Since all render+compute engines get reset
 * together, and the contents of these registers are lost during
 * the shared render domain reset, we'll define such workarounds
 * here and then add them to just a single RCS or CCS engine's
 * workaround list (whichever engine has the XXXX flag).
 */
static void
general_render_compute_wa_init(struct intel_engine_cs *engine, struct i915_wa_list *wal)
{
	struct drm_i915_private *i915 = engine->i915;

	add_render_compute_tuning_settings(i915, wal);

	if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 50)) {
		/* This is not a Wa (although referred to as
		 * WaSetInidrectStateOverride in places), this allows
		 * applications that reference sampler states through
		 * the BindlessSamplerStateBaseAddress to have their
		 * border color relative to DynamicStateBaseAddress
		 * rather than BindlessSamplerStateBaseAddress.
		 *
		 * Otherwise SAMPLER_STATE border colors have to be
		 * copied in multiple heaps (DynamicStateBaseAddress &
		 * BindlessSamplerStateBaseAddress)
		 *
		 * BSpec: 46052
		 */
		if (!IS_PONTEVECCHIO(i915))
			wa_mcr_masked_en(wal,
					 GEN10_SAMPLER_MODE,
					 GEN11_INDIRECT_STATE_BASE_ADDR_OVERRIDE);
	}

	if (IS_MTL_GRAPHICS_STEP(i915, M, STEP_A0, STEP_B0) ||
	    IS_MTL_GRAPHICS_STEP(i915, P, STEP_A0, STEP_B0) ||
	    IS_PONTEVECCHIO(i915) ||
	    IS_DG2(i915)) {
		/* Wa_22014226127 */
		wa_mcr_write_or(wal, LSC_CHICKEN_BIT_0, DISABLE_D8_D16_COASLESCE);
	}

	if (IS_MTL_GRAPHICS_STEP(i915, M, STEP_A0, STEP_B0) ||
	    IS_MTL_GRAPHICS_STEP(i915, P, STEP_A0, STEP_B0) ||
	    IS_DG2(i915)) {
		/* Wa_18017747507 */
		wa_masked_en(wal, VFG_PREEMPTION_CHICKEN, POLYGON_TRIFAN_LINELOOP_DISABLE);
	}

	if (IS_PONTEVECCHIO(i915)) {
		/* Wa_16017028706 */
		wa_masked_en(wal, GEN12_RCU_MODE,
			     XEHP_RCU_MODE_FIXED_SLICE_CCS_MODE);
	}

	if (IS_DG2_G10(i915) || IS_DG2_G12(i915)) {
		/* Wa_18028616096 */
		wa_mcr_write_or(wal, LSC_CHICKEN_BIT_0_UDW, UGM_FRAGMENT_THRESHOLD_TO_3);
	}
	/* Wa_14015227452:dg2,pvc */
	if (IS_DG2(i915) || IS_PONTEVECCHIO(i915))
		wa_mcr_masked_en(wal, GEN9_ROW_CHICKEN4, XEHP_DIS_BBL_SYSPIPE);

	/* Wa_16015675438:dg2,pvc */
	if (IS_DG2_G10(i915) || IS_DG2_G12(i915))
		wa_masked_en(wal, FF_SLICE_CS_CHICKEN2, GEN12_PERF_FIX_BALANCING_CFE_DISABLE);

	if (IS_DG2(i915)) {
		/*
		 * Wa_16011620976:dg2_g11
		 * Wa_22015475538:dg2
		 */
		wa_mcr_write_or(wal, LSC_CHICKEN_BIT_0_UDW, DIS_CHAIN_2XSIMD8);
	}

	if (IS_DG2_GRAPHICS_STEP(i915, G10, STEP_A0, STEP_C0) || IS_DG2_G11(i915))
		/*
		 * Wa_22012654132
		 *
		 * Note that register 0xE420 is write-only and cannot be read
		 * back for verification on DG2 (due to Wa_14012342262), so
		 * we need to explicitly skip the readback.
		 */
		wa_mcr_add(wal, GEN10_CACHE_MODE_SS, 0,
			   _MASKED_BIT_ENABLE(ENABLE_PREFETCH_INTO_IC),
			   0 /* write-only, so skip validation */,
			   true);

	if (!RCS_MASK(engine->gt)) {
		/*
		 * EUs on compute engines can generate hardware status page
		 * updates to a fused off render engine. Avoid these by always
		 * keeping full mask for the fused off part as the default
		 * mask can let updates happen and that leads to write into
		 * ggtt that is not backed up by a real hardware status page.
		 *
		 *  Wa_18020744125
		 */
		wa_write(wal, RING_HWSTAM(RENDER_RING_BASE), ~0);
	}
}

static void
engine_init_workarounds(struct intel_engine_cs *engine, struct i915_wa_list *wal)
{
	if (I915_SELFTEST_ONLY(GRAPHICS_VER(engine->i915) < 4))
		return;

	engine_fake_wa_init(engine, wal);

	/*
	 * These are common workarounds that just need to applied
	 * to a single RCS/CCS engine's workaround list since
	 * they're reset as part of the general render domain reset.
	 */
	if (engine->flags & I915_ENGINE_FIRST_RENDER_COMPUTE)
		general_render_compute_wa_init(engine, wal);

	if (engine->class == COMPUTE_CLASS)
		ccs_engine_wa_init(engine, wal);
	else if (engine->class == RENDER_CLASS)
		rcs_engine_wa_init(engine, wal);
	else
		xcs_engine_wa_init(engine, wal);
}

static void engine_debug_init_workarounds(struct intel_engine_cs *engine,
					  struct i915_wa_list *wal)
{
	struct drm_i915_private *i915 = engine->i915;

	if (!(engine->flags & I915_ENGINE_FIRST_RENDER_COMPUTE) ||
	    GRAPHICS_VER(i915) < 9)
		return;

	gen9_debug_td_ctl_init(engine, wal);

	/* Wa_22015693276 */
	if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 50))
		wa_mcr_masked_en(wal, GEN8_ROW_CHICKEN,
				 STALL_DOP_GATING_DISABLE);

	/* Wa_14015527279:pvc */
	if (IS_PONTEVECCHIO(i915))
		wa_masked_en(wal, GEN7_ROW_CHICKEN2, XEHPC_DISABLE_BTB);

	if (engine->class == COMPUTE_CLASS)
		return;

	GEM_WARN_ON(engine->class != RENDER_CLASS);

	if (GRAPHICS_VER(i915) >= 11 && GRAPHICS_VER_FULL(i915) < IP_VER(12, 50))
		wa_masked_en(wal, GEN9_CS_DEBUG_MODE2, GEN11_GLOBAL_DEBUG_ENABLE);
	else if (GRAPHICS_VER(i915) == 9)
		wa_masked_en(wal, GEN9_CS_DEBUG_MODE1, GEN9_GLOBAL_DEBUG_ENABLE);
}

static void _wa_remove_bit(struct i915_wa_list *wal, i915_reg_t reg, u32 clr)
{
	struct i915_wa *wa;
	int index;

	index = _wa_index(wal, reg);
	if (index < 0) {
		DRM_ERROR("removing bits:%08x from unknown w/a for 0x%x\n",
			  clr, i915_mmio_reg_offset(reg));
		return;
	}

	wa = &wal->list[index];
	if ((wa->set & clr) != clr)
		DRM_ERROR("removing unknown bits:%08x from w/a for 0x%x\n",
			  clr, i915_mmio_reg_offset(reg));

	wa->set &= ~clr;
	if (!(wa->set | wa->clr))
		_wa_remove(wal, reg, 0);
}

static void engine_debug_fini_workarounds(struct intel_engine_cs *engine,
					  struct i915_wa_list *wal)
{
	struct drm_i915_private *i915 = engine->i915;

	if (!(engine->flags & I915_ENGINE_FIRST_RENDER_COMPUTE) ||
	    GRAPHICS_VER(i915) < 9)
		return;

	_wa_mcr_remove(wal, TD_CTL, 0);

	/* Wa_22015693276 */
	if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 50))
		wa_mcr_masked_dis(wal, GEN8_ROW_CHICKEN,
				  STALL_DOP_GATING_DISABLE);

	/* Wa_14015527279:pvc */
	if (IS_PONTEVECCHIO(i915))
		_wa_remove_bit(wal, GEN7_ROW_CHICKEN2, XEHPC_DISABLE_BTB);

	if (engine->class == COMPUTE_CLASS)
		return;

	GEM_WARN_ON(engine->class != RENDER_CLASS);

	if (GRAPHICS_VER(i915) >= 11 && GRAPHICS_VER_FULL(i915) < IP_VER(12, 50))
		_wa_remove_bit(wal, GEN9_CS_DEBUG_MODE2, GEN11_GLOBAL_DEBUG_ENABLE);
	else if (GRAPHICS_VER(i915) == 9)
		_wa_remove_bit(wal, GEN9_CS_DEBUG_MODE1, GEN9_GLOBAL_DEBUG_ENABLE);
}

void intel_engine_init_workarounds(struct intel_engine_cs *engine)
{
	struct i915_wa_list *wal = &engine->wa_list;

	if (IS_SRIOV_VF(engine->i915))
		return;

	if (GRAPHICS_VER(engine->i915) < 4)
		return;

	wa_init(wal, "engine", engine->name);
	engine_init_workarounds(engine, wal);

#if IS_ENABLED(CPTCFG_DRM_I915_DEBUGGER)
	if (engine->i915->debuggers.enable_eu_debug)
		engine_debug_init_workarounds(engine, wal);
#endif
}

void intel_engine_debug_enable(struct intel_engine_cs *engine)
{
	engine_debug_init_workarounds(engine, &engine->wa_list);
	engine_debug_init_whitelist(engine, &engine->whitelist);
}

void intel_engine_debug_disable(struct intel_engine_cs *engine)
{
	engine_debug_fini_workarounds(engine, &engine->wa_list);
	engine_debug_fini_whitelist(engine, &engine->whitelist);
}

void intel_engine_allow_user_register_access(struct intel_engine_cs *engine,
					     struct i915_whitelist_reg *reg,
					     u32 count)
{
	if (!engine || !reg)
		return;

	while (count--) {
		whitelist_reg_ext(&engine->whitelist, reg->reg, reg->flags);
		reg++;
	}

	intel_engine_apply_whitelist(engine);
}

void intel_engine_deny_user_register_access(struct intel_engine_cs *engine,
					    struct i915_whitelist_reg *reg,
					    u32 count)
{
	if (!engine || !reg)
		return;

	while (count--) {
		_wa_remove(&engine->whitelist, reg->reg, reg->flags);
		reg++;
	}

	intel_engine_apply_whitelist(engine);
}

void intel_engine_apply_workarounds(struct intel_engine_cs *engine)
{
	wa_list_apply(engine->gt, &engine->wa_list);
}

static const struct i915_range mcr_ranges_gen8[] = {
	{ .start = 0x5500, .end = 0x55ff },
	{ .start = 0x7000, .end = 0x7fff },
	{ .start = 0x9400, .end = 0x97ff },
	{ .start = 0xb000, .end = 0xb3ff },
	{ .start = 0xe000, .end = 0xe7ff },
	{},
};

static const struct i915_range mcr_ranges_gen12[] = {
	{ .start =  0x8150, .end =  0x815f },
	{ .start =  0x9520, .end =  0x955f },
	{ .start =  0xb100, .end =  0xb3ff },
	{ .start =  0xde80, .end =  0xe8ff },
	{ .start = 0x24a00, .end = 0x24a7f },
	{},
};

static const struct i915_range mcr_ranges_xehp[] = {
	{ .start =  0x4000, .end =  0x4aff },
	{ .start =  0x5200, .end =  0x52ff },
	{ .start =  0x5400, .end =  0x7fff },
	{ .start =  0x8140, .end =  0x815f },
	{ .start =  0x8c80, .end =  0x8dff },
	{ .start =  0x94d0, .end =  0x955f },
	{ .start =  0x9680, .end =  0x96ff },
	{ .start =  0xb000, .end =  0xb3ff },
	{ .start =  0xc800, .end =  0xcfff },
	{ .start =  0xd800, .end =  0xd8ff },
	{ .start =  0xdc00, .end =  0xffff },
	{ .start = 0x17000, .end = 0x17fff },
	{ .start = 0x24a00, .end = 0x24a7f },
	{},
};

static bool mcr_range(struct drm_i915_private *i915, u32 offset)
{
	const struct i915_range *mcr_ranges;
	int i;

	if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 50))
		mcr_ranges = mcr_ranges_xehp;
	else if (GRAPHICS_VER(i915) >= 12)
		mcr_ranges = mcr_ranges_gen12;
	else if (GRAPHICS_VER(i915) >= 8)
		mcr_ranges = mcr_ranges_gen8;
	else
		return false;

	/*
	 * Registers in these ranges are affected by the MCR selector
	 * which only controls CPU initiated MMIO. Routing does not
	 * work for CS access so we cannot verify them on this path.
	 */
	for (i = 0; mcr_ranges[i].start; i++)
		if (offset >= mcr_ranges[i].start &&
		    offset <= mcr_ranges[i].end)
			return true;

	return false;
}

static int
wa_list_srm(struct i915_request *rq,
	    const struct i915_wa_list *wal,
	    struct i915_vma *vma)
{
	struct drm_i915_private *i915 = rq->engine->i915;
	unsigned int i, count = 0;
	const struct i915_wa *wa;
	u32 srm, *cs;

	srm = MI_STORE_REGISTER_MEM | MI_SRM_LRM_GLOBAL_GTT;
	if (GRAPHICS_VER(i915) >= 8)
		srm++;

	for (i = 0, wa = wal->list; i < wal->count; i++, wa++) {
		if (!mcr_range(i915, i915_mmio_reg_offset(wa->reg)))
			count++;
	}

	cs = intel_ring_begin(rq, 4 * count + 4);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	for (i = 0, wa = wal->list; i < wal->count; i++, wa++) {
		u32 offset = i915_mmio_reg_offset(wa->reg);

		if (mcr_range(i915, offset))
			continue;

		*cs++ = srm;
		*cs++ = offset;
		*cs++ = i915_ggtt_offset(vma) + sizeof(u32) * i;
		*cs++ = 0;
	}

	*cs++ = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT;
	*cs++ = i915_ggtt_offset(vma) + sizeof(u32) * i;
	*cs++ = 0;
	*cs++ = 1;

	intel_ring_advance(rq, cs);

	if (GRAPHICS_VER(i915) >= 8) {
		cs = intel_ring_begin(rq, 4);
		if (IS_ERR(cs))
			return PTR_ERR(cs);

		*cs++ = MI_SEMAPHORE_WAIT |
			MI_SEMAPHORE_GLOBAL_GTT |
			MI_SEMAPHORE_POLL |
			MI_SEMAPHORE_SAD_EQ_SDD;
		*cs++ = 2;
		*cs++ = i915_ggtt_offset(vma) + sizeof(u32) * i;
		*cs++ = 0;
		intel_ring_advance(rq, cs);
	}

	return 0;
}

static int engine_wa_list_verify(struct intel_context *ce,
				 const struct i915_wa_list * const wal,
				 bool (*verify)(const struct i915_wa *wa,
					 bool mcr,
					 u32 cur,
					 const char *name,
					 void *data),
				 void *data)
{
	const struct i915_wa *wa;
	struct i915_request *rq;
	struct i915_vma *vma;
	struct i915_gem_ww_ctx ww;
	unsigned int i;
	u32 *results;
	int err;

	if (!wal->count)
		return 0;

	vma = __vm_create_scratch_for_read(&ce->engine->gt->ggtt->vm,
					   (wal->count + 1) * sizeof(u32));
	if (IS_ERR(vma))
		return PTR_ERR(vma);

	intel_engine_pm_get(ce->engine);
	i915_gem_ww_ctx_init(&ww, false);
retry:
	err = i915_gem_object_lock(vma->obj, &ww);
	if (err == 0)
		err = intel_context_pin_ww(ce, &ww);
	if (err)
		goto err_pm;

	err = i915_vma_pin_ww(vma, &ww, 0, 0,
			   i915_vma_is_ggtt(vma) ? PIN_GLOBAL : PIN_USER);
	if (err)
		goto err_unpin;

	results = i915_gem_object_pin_map(vma->obj, I915_MAP_WB);
	if (IS_ERR(results)) {
		err = PTR_ERR(results);
		goto err_vma;
	}
	memset(results, 0, (wal->count + 1) * sizeof(u32));

	rq = i915_request_create(ce);
	if (IS_ERR(rq)) {
		err = PTR_ERR(rq);
		goto err_unmap;
	}

	err = i915_request_await_object(rq, vma->obj, true);
	if (err == 0)
		err = i915_vma_move_to_active(vma, rq, EXEC_OBJECT_WRITE);
	if (err == 0)
		err = wa_list_srm(rq, wal, vma);

	i915_request_get(rq);
	if (err)
		i915_request_set_error_once(rq, err);
	i915_request_add(rq);
	if (err)
		goto err_rq;

	if (wait_for(READ_ONCE(results[wal->count]), 1000)) {
		err = -ETIME;
		goto err_rq;
	}

	for (i = 0, wa = wal->list; i < wal->count; i++, wa++) {
		u32 cur = results[i];
		bool mcr = false;

		/* Context is held to keep the engine powered for the mmio */
		if (mcr_range(rq->engine->i915, i915_mmio_reg_offset(wa->reg))) {
			cur = wa->set | ~wa->read;
			if (!i915_request_completed(rq))
				cur = intel_gt_mcr_read_any(ce->engine->gt, wa->mcr_reg);
			mcr = true;
			continue; /* XXX Some are being reset to default? */
		}

		if (!verify(wa, mcr, cur, wal->name, data))
			err = -EINVAL;
	}

err_rq:
	i915_request_put(rq);
err_unmap:
	WRITE_ONCE(results[wal->count], 2);
	i915_gem_object_unpin_map(vma->obj);
err_vma:
	i915_vma_unpin(vma);
err_unpin:
	intel_context_unpin(ce);
err_pm:
	if (err == -EDEADLK) {
		err = i915_gem_ww_ctx_backoff(&ww);
		if (!err)
			goto retry;
	}
	i915_gem_ww_ctx_fini(&ww);
	intel_engine_pm_put(ce->engine);
	i915_vma_put(vma);
	return err;
}

int intel_engine_show_workarounds(struct drm_printer *m,
				  struct intel_engine_cs *engine,
				  const struct i915_wa_list * const wal)
{
	struct intel_context *ce;
	int err;

	ce = intel_context_create(engine);
	if (IS_ERR(ce))
		return PTR_ERR(ce);

	err = engine_wa_list_verify(ce, wal, wa_show, m);
	intel_context_put(ce);

	return err;
}

int intel_engine_verify_workarounds(struct intel_engine_cs *engine,
				    const char *from)
{
	return engine_wa_list_verify(engine->kernel_context,
				     &engine->wa_list,
				     wa_verify,
				     (void *)from);
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftest_workarounds.c"
#endif
