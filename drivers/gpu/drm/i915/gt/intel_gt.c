// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <drm/intel-gtt.h>

#include "intel_gt_debugfs.h"

#include "gem/i915_gem_lmem.h"
#include "gem/i915_gem_region.h"

#include "gen8_ppgtt.h"
#include "i915_drv.h"
#include "intel_context.h"
#include "intel_engine_regs.h"
#include "intel_flat_ppgtt_pool.h"
#include "intel_gt.h"
#include "intel_gt_buffer_pool.h"
#include "intel_gt_clock_utils.h"
#include "intel_gt_pm.h"
#include "intel_gt_regs.h"
#include "intel_gt_requests.h"
#include "intel_mocs.h"
#include "intel_pm.h"
#include "intel_rc6.h"
#include "intel_renderstate.h"
#include "intel_rps.h"
#include "intel_uncore.h"
#include "intel_pagefault.h"
#include "intel_pm.h"
#include "iov/intel_iov.h"
#include "shmem_utils.h"
#include "intel_gt_sysfs.h"
#include "pxp/intel_pxp.h"
#include "gt/iov/intel_iov_sysfs.h"

static const char *intel_gt_driver_errors_to_str[] = {
	[INTEL_GT_DRIVER_ERROR_GGTT] = "GGTT",
	[INTEL_GT_DRIVER_ERROR_ENGINE_OTHER] = "ENGINE OTHER",
	[INTEL_GT_DRIVER_ERROR_GUC_COMMUNICATION] = "GUC COMMUNICATION",
	[INTEL_GT_DRIVER_ERROR_RPS] = "RPS",
	[INTEL_GT_DRIVER_ERROR_GT_OTHER] = "GT OTHER",
	[INTEL_GT_DRIVER_ERROR_INTERRUPT] = "INTERRUPT",
};

void intel_gt_log_driver_error(struct intel_gt *gt,
			       const enum intel_gt_driver_errors error,
			       const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;

	BUILD_BUG_ON(ARRAY_SIZE(intel_gt_driver_errors_to_str) !=
		     INTEL_GT_DRIVER_ERROR_COUNT);
	GEM_BUG_ON(error >= INTEL_GT_DRIVER_ERROR_COUNT);

	gt->errors.driver[error]++;

	drm_err_ratelimited(&gt->i915->drm, "GT%u [%s] %pV",
			    gt->info.id,
			    intel_gt_driver_errors_to_str[error],
			    &vaf);
	va_end(args);
}

static void
__intel_gt_init_early(struct intel_gt *gt,
		      struct intel_uncore *uncore,
		      struct intel_uncore_mmio_debug *mmio_debug,
		      struct drm_i915_private *i915)
{
	gt->i915 = i915;
	gt->uncore = uncore;
	gt->mmio_debug = mmio_debug;

	spin_lock_init(&gt->irq_lock);

	mutex_init(&gt->mutex);

	INIT_LIST_HEAD(&gt->closed_vma);
	spin_lock_init(&gt->closed_lock);

	init_llist_head(&gt->watchdog.list);
	INIT_WORK(&gt->watchdog.work, intel_gt_watchdog_work);

	xa_init(&gt->errors.soc);

	intel_gt_init_buffer_pool(gt);

	atomic_set(&gt->next_token, 0);

	intel_gt_init_reset(gt);
	intel_gt_init_requests(gt);
	intel_gt_init_timelines(gt);
	intel_gt_pm_init_early(gt);

	intel_flat_ppgtt_pool_init_early(&gt->fpp);
	intel_uc_init_early(&gt->uc);
	intel_rps_init_early(&gt->rps);
}

static unsigned int to_logical_instance(struct intel_gt *gt, unsigned int instance)
{
	struct drm_i915_private *i915 = gt->i915;

	if (IS_SRIOV_VF(i915) && HAS_REMOTE_TILES(i915))
		instance = hweight32(GENMASK(instance, 0) &
				     to_root_gt(i915)->iov.vf.config.tile_mask) - 1;
	return instance;
}

static int intel_gt_probe_lmem(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;
	unsigned int instance = gt->info.id;
	struct intel_memory_region *mem;
	int id;
	int err;

	mem = intel_gt_setup_lmem(gt);
	if (IS_ERR(mem)) {
		err = PTR_ERR(mem);
		if (err == -ENODEV)
			return 0;

		drm_err(&i915->drm,
			"Failed to setup region(%d) type=%d instance=%u\n",
			err, INTEL_MEMORY_LOCAL, instance);
		return err;
	}

	id = INTEL_REGION_LMEM + instance;

	mem->id = id;
	mem->type = INTEL_MEMORY_LOCAL;
	mem->instance = to_logical_instance(gt, instance);
	mem->gt = gt;

	intel_memory_region_set_name(mem, "local%u", mem->instance);

	GEM_BUG_ON(!HAS_REGION(i915, id));
	GEM_BUG_ON(i915->mm.regions[id]);
	i915->mm.regions[id] = mem;
	gt->lmem = mem;

	return 0;
}

void intel_gt_init_early(struct intel_gt *gt, struct drm_i915_private *i915)
{
	__intel_gt_init_early(gt, &i915->uncore, &i915->mmio_debug, i915);
}

void intel_gt_init_ggtt(struct intel_gt *gt, struct i915_ggtt *ggtt)
{
	gt->ggtt = ggtt;
}

static const char * const intel_steering_types[] = {
	"L3BANK",
	"MSLICE",
	"LNCF",
	"BSLICE",
};
static const struct intel_mmio_range icl_l3bank_steering_table[] = {
	{ 0x00B100, 0x00B3FF },
	{},
};

static const struct intel_mmio_range xehpsdv_mslice_steering_table[] = {
	{ 0x004000, 0x004AFF },
	{ 0x00C800, 0x00CFFF },
	{ 0x00DD00, 0x00DDFF },
	{ 0x00E900, 0x00FFFF }, /* 0xEA00 - OxEFFF is unused */
	{},
};

static const struct intel_mmio_range xehpsdv_lncf_steering_table[] = {
	{ 0x00B000, 0x00B0FF },
	{ 0x00D800, 0x00D8FF },
	{},
};

static const struct intel_mmio_range dg2_lncf_steering_table[] = {
	{ 0x00B000, 0x00B0FF },
	{ 0x00D880, 0x00D8FF },
	{},
};

static const struct intel_mmio_range pvc_bslice_steering_table[] = {
	{ 0x00DD00, 0x00DDFF },
	{},
};

static u16 slicemask(struct intel_gt *gt, int count)
{
	u64 dss_mask = intel_sseu_get_subslices(&gt->info.sseu, 0);

	return intel_slicemask_from_dssmask(dss_mask, count);
}

int intel_gt_init_mmio(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;

	intel_gt_init_clock_frequency(gt);
	intel_uc_init_mmio(&gt->uc);

	intel_sseu_info_init(gt);

	/*
	 * An mslice is unavailable only if both the meml3 for the slice is
	 * disabled *and* all of the DSS in the slice (quadrant) are disabled.
	 */
	if (HAS_MSLICES(i915))
		gt->info.mslice_mask =
			slicemask(gt, GEN_DSS_PER_MSLICE) |
			(intel_uncore_read(gt->uncore, GEN10_MIRROR_FUSE3) &
			 GEN12_MEML3_EN_MASK);

	/*
	 * There are 4 bslices which hold 16 DSS each.  Bslice 0 is
	 * always present.
	 */
	if (HAS_BSLICES(i915))
		gt->info.bslice_mask =
			slicemask(gt, GEN_DSS_PER_BSLICE) | BIT(0);

	if (IS_PONTEVECCHIO(i915)) {
		gt->steering_table[BSLICE] = pvc_bslice_steering_table;
	} else if (IS_DG2(i915)) {
		gt->steering_table[MSLICE] = xehpsdv_mslice_steering_table;
		gt->steering_table[LNCF] = dg2_lncf_steering_table;
	} else if (IS_XEHPSDV(i915)) {
		gt->steering_table[MSLICE] = xehpsdv_mslice_steering_table;
		gt->steering_table[LNCF] = xehpsdv_lncf_steering_table;
	} else if (GRAPHICS_VER(i915) >= 11 &&
		   GRAPHICS_VER_FULL(i915) < IP_VER(12, 50)) {
		gt->steering_table[L3BANK] = icl_l3bank_steering_table;
		gt->info.l3bank_mask =
			~intel_uncore_read(gt->uncore, GEN10_MIRROR_FUSE3) &
			GEN10_L3BANK_MASK;
	} else if (HAS_MSLICES(i915) || HAS_BSLICES(i915)) {
		MISSING_CASE(INTEL_INFO(i915)->platform);
	}

	return intel_engines_init_mmio(gt);
}

static void init_unused_ring(struct intel_gt *gt, u32 base)
{
	struct intel_uncore *uncore = gt->uncore;

	intel_uncore_write(uncore, RING_CTL(base), 0);
	intel_uncore_write(uncore, RING_HEAD(base), 0);
	intel_uncore_write(uncore, RING_TAIL(base), 0);
	intel_uncore_write(uncore, RING_START(base), 0);
}

static void init_unused_rings(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;

	if (IS_I830(i915)) {
		init_unused_ring(gt, PRB1_BASE);
		init_unused_ring(gt, SRB0_BASE);
		init_unused_ring(gt, SRB1_BASE);
		init_unused_ring(gt, SRB2_BASE);
		init_unused_ring(gt, SRB3_BASE);
	} else if (GRAPHICS_VER(i915) == 2) {
		init_unused_ring(gt, SRB0_BASE);
		init_unused_ring(gt, SRB1_BASE);
	} else if (GRAPHICS_VER(i915) == 3) {
		init_unused_ring(gt, PRB1_BASE);
		init_unused_ring(gt, PRB2_BASE);
	}
}

static void gen12_stateless_mc_set(struct intel_gt *gt, u32 val)
{
	struct drm_i915_private *i915 = gt->i915;
	struct intel_uncore *uncore = gt->uncore;

	if (intel_gt_has_eus(gt)) {
		u32 misccpctl;

		misccpctl = intel_uncore_read(uncore, GEN7_MISCCPCTL);

		if ((misccpctl & GEN12_DOP_CLOCK_GATE_LOCK) != 0) {
			drm_err(&i915->drm, "Clock gating control register locked for writing");
			mkwrite_device_info(i915)->has_stateless_mc = 0;
			return;
		}

		/* Wa_14015795083: Disable DOP clk gating for
		 * programming GEN12_DSS_UM_COMPRESSION */
		intel_uncore_write(uncore, GEN7_MISCCPCTL, misccpctl
					& ~GEN12_DOP_CLOCK_GATE_RENDER_ENABLE);
		intel_uncore_write(uncore, GEN12_DSS_UM_COMPRESSION, val);
		intel_uncore_write(uncore, GEN7_MISCCPCTL, misccpctl);
	}

	intel_uncore_write(uncore, GEN12_UM_COMPRESSION, val);
	intel_uncore_write(uncore, GEN12_LNI_UM_COMPRESSION, val);
}

/*
 * Unified memory allows access to any user virtual address from the
 * device.  Buffers allocated by system allocator are said to be without
 * any state.  To still support memory compression for these buffers,
 * the device has compression defaults for 'stateless access' and when
 * the buffer is backed by device memory.
 */
static void intel_stateless_mc_init(struct intel_gt *gt)
{
	if (!HAS_STATELESS_MC(gt->i915))
		return;

	gen12_stateless_mc_set(gt, GEN12_COMPRESSION_ENABLE);
}

int intel_gt_init_hw(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;
	struct intel_uncore *uncore = gt->uncore;
	int ret;

	gt->last_init_time = ktime_get();

	/* Double layer security blanket, see i915_gem_init() */
	intel_uncore_forcewake_get(uncore, FORCEWAKE_ALL);

	if (HAS_EDRAM(i915) && GRAPHICS_VER(i915) < 9)
		intel_uncore_rmw(uncore, HSW_IDICR, 0, IDIHASHMSK(0xf));

	if (IS_HASWELL(i915))
		intel_uncore_write(uncore,
				   HSW_MI_PREDICATE_RESULT_2,
				   IS_HSW_GT3(i915) ?
				   LOWER_SLICE_ENABLED : LOWER_SLICE_DISABLED);

	/* Apply the GT workarounds... */
	intel_gt_apply_workarounds(gt);
	/* ...and determine whether they are sticking. */
	intel_gt_verify_workarounds(gt, "init");

	intel_gt_init_swizzling(gt);

	/*
	 * At least 830 can leave some of the unused rings
	 * "active" (ie. head != tail) after resume which
	 * will prevent c3 entry. Makes sure all unused rings
	 * are totally idle.
	 */
	init_unused_rings(gt);

	ret = i915_ppgtt_init_hw(gt);
	if (ret) {
		DRM_ERROR("Enabling PPGTT failed (%d)\n", ret);
		goto out;
	}

	/*
	 * GuC DMA transfers are affected by MOCS programming on some
	 * platforms so make sure the MOCS table is initialised prior
	 * to loading the GuC firmware
	 */
	intel_mocs_init(gt);

	/* We can't enable contexts until all firmware is loaded */
	ret = intel_uc_init_hw(&gt->uc);
	if (ret) {
		i915_probe_error(i915, "Enabling uc failed (%d)\n", ret);
		goto out;
	}

	/* Initialize stateless compression settings */
	intel_stateless_mc_init(gt);

	ret = intel_iov_init_hw(&gt->iov);
	if (unlikely(ret)) {
		i915_probe_error(i915, "Enabling IOV failed (%pe)\n",
				 ERR_PTR(ret));
		goto out;
	}

out:
	intel_uncore_forcewake_put(uncore, FORCEWAKE_ALL);
	return ret;
}

static void rmw_set(struct intel_uncore *uncore, i915_reg_t reg, u32 set)
{
	intel_uncore_rmw(uncore, reg, 0, set);
}

static void rmw_clear(struct intel_uncore *uncore, i915_reg_t reg, u32 clr)
{
	intel_uncore_rmw(uncore, reg, clr, 0);
}

static void clear_register(struct intel_uncore *uncore, i915_reg_t reg)
{
	intel_uncore_rmw(uncore, reg, 0, 0);
}

static void gen6_clear_engine_error_register(struct intel_engine_cs *engine)
{
	GEN6_RING_FAULT_REG_RMW(engine, RING_FAULT_VALID, 0);
	GEN6_RING_FAULT_REG_POSTING_READ(engine);
}

bool intel_gt_has_eus(const struct intel_gt *gt)
{
	if (GRAPHICS_VER_FULL(gt->i915) < IP_VER(12, 50))
		return true;

	return intel_sseu_get_subslices(&gt->info.sseu, 0) > 0;
}

void
intel_gt_clear_error_registers(struct intel_gt *gt,
			       intel_engine_mask_t engine_mask)
{
	struct drm_i915_private *i915 = gt->i915;
	struct intel_uncore *uncore = gt->uncore;
	u32 eir;

	if (IS_GRAPHICS_VER(i915, 3, 5))
		clear_register(uncore, PGTBL_ER);

	if (GRAPHICS_VER(i915) < 4)
		clear_register(uncore, IPEIR(RENDER_RING_BASE));

	if (intel_gt_has_eus(gt)) {
		clear_register(uncore, IPEIR_I965);
		clear_register(uncore, EIR);
		eir = intel_uncore_read(uncore, EIR);
		if (eir) {
			/*
			 * some errors might have become stuck,
			 * mask them.
			 */
			DRM_DEBUG_DRIVER("EIR stuck: 0x%08x, masking\n", eir);
			rmw_set(uncore, EMR, eir);
			intel_uncore_write(uncore, GEN2_IIR,
					   I915_MASTER_ERROR_INTERRUPT);
		}
	}

	if (HAS_MSLICES(i915)) {
		enum forcewake_domains fw_domains;
		u32 old_mcr;
		u8 mslice;

		/* all the fault regs are in the same FW domain. MCR is not in FW */
		fw_domains = intel_uncore_forcewake_for_reg(uncore, GEN12_RING_FAULT_REG,
							    FW_REG_READ | FW_REG_WRITE);

		fw_domains |= intel_uncore_forcewake_for_reg(uncore, GEN8_MCR_SELECTOR,
							     FW_REG_READ | FW_REG_WRITE);

		spin_lock_irq(&uncore->lock);
		intel_uncore_forcewake_get__locked(uncore, fw_domains);

		old_mcr = intel_uncore_read_fw(uncore, GEN8_MCR_SELECTOR);

		for_each_set_bit(mslice, &gt->info.mslice_mask, GEN12_MAX_MSLICES) {
			/* unicast access to selected mslice */
			intel_uncore_write_fw(uncore, GEN8_MCR_SELECTOR, GEN8_MCR_SLICE(mslice));

			intel_uncore_rmw_fw(uncore, GEN12_RING_FAULT_REG,
					    RING_FAULT_VALID, 0);
		}

		intel_uncore_write_fw(uncore, GEN8_MCR_SELECTOR, old_mcr);

		/* multicast post */
		intel_uncore_posting_read_fw(uncore, GEN12_RING_FAULT_REG);

		intel_uncore_forcewake_put__locked(uncore, fw_domains);
		spin_unlock_irq(&uncore->lock);
	} else if (GRAPHICS_VER(i915) >= 12) {
		rmw_clear(uncore, GEN12_RING_FAULT_REG, RING_FAULT_VALID);
		intel_uncore_posting_read(uncore, GEN12_RING_FAULT_REG);
	} else if (GRAPHICS_VER(i915) >= 8) {
		rmw_clear(uncore, GEN8_RING_FAULT_REG, RING_FAULT_VALID);
		intel_uncore_posting_read(uncore, GEN8_RING_FAULT_REG);
	} else if (GRAPHICS_VER(i915) >= 6) {
		struct intel_engine_cs *engine;
		enum intel_engine_id id;

		for_each_engine_masked(engine, gt, engine_mask, id)
			gen6_clear_engine_error_register(engine);
	}
}

static void gen6_check_faults(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	u32 fault;

	for_each_engine(engine, gt, id) {
		fault = GEN6_RING_FAULT_REG_READ(engine);
		if (fault & RING_FAULT_VALID) {
			drm_dbg(&engine->i915->drm, "Unexpected fault\n"
				"\tAddr: 0x%08lx\n"
				"\tAddress space: %s\n"
				"\tSource ID: %d\n"
				"\tLevel: %d\n",
				fault & PAGE_MASK,
				fault & RING_FAULT_GTTSEL_MASK ?
				"GGTT" : "PPGTT",
				RING_FAULT_SRCID(fault),
				RING_FAULT_LEVEL(fault));
		}
	}
}

static void gen8_check_faults(struct intel_gt *gt)
{
	struct intel_uncore *uncore = gt->uncore;
	i915_reg_t fault_reg, fault_data0_reg, fault_data1_reg;
	u32 fault;

	if (GRAPHICS_VER(gt->i915) >= 12) {
		fault_reg = GEN12_RING_FAULT_REG;
		fault_data0_reg = GEN12_FAULT_TLB_DATA0;
		fault_data1_reg = GEN12_FAULT_TLB_DATA1;
	} else {
		fault_reg = GEN8_RING_FAULT_REG;
		fault_data0_reg = GEN8_FAULT_TLB_DATA0;
		fault_data1_reg = GEN8_FAULT_TLB_DATA1;
	}

	fault = intel_uncore_read(uncore, fault_reg);
	if (fault & RING_FAULT_VALID) {
		u32 fault_data0, fault_data1;
		u64 fault_addr;

		fault_data0 = intel_uncore_read(uncore, fault_data0_reg);
		fault_data1 = intel_uncore_read(uncore, fault_data1_reg);

		fault_addr = ((u64)(fault_data1 & FAULT_VA_HIGH_BITS) << 44) |
			     ((u64)fault_data0 << 12);

		drm_dbg(&uncore->i915->drm, "Unexpected fault\n"
			"\tAddr: 0x%08x_%08x\n"
			"\tAddress space: %s\n"
			"\tEngine ID: %d\n"
			"\tSource ID: %d\n"
			"\tLevel: %d\n",
			upper_32_bits(fault_addr),
			lower_32_bits(fault_addr),
			fault_data1 & FAULT_GTT_SEL ? "GGTT" : "PPGTT",
			GEN8_RING_FAULT_ENGINE_ID(fault),
			RING_FAULT_SRCID(fault),
			RING_FAULT_LEVEL(fault));
	}
}

static void xehpsdv_check_faults(struct intel_gt *gt)
{
	struct intel_uncore *uncore = gt->uncore;
	enum forcewake_domains fw_domains;
	u32 old_mcr;
	u8 mslice;
	u32 fault;

	/* all the fault regs are in the same FW domain. MCR is not in FW */
	fw_domains = intel_uncore_forcewake_for_reg(uncore, GEN12_RING_FAULT_REG,
						    FW_REG_READ | FW_REG_WRITE);

	fw_domains |= intel_uncore_forcewake_for_reg(uncore, GEN8_MCR_SELECTOR,
						     FW_REG_READ | FW_REG_WRITE);

	spin_lock_irq(&uncore->lock);
	intel_uncore_forcewake_get__locked(uncore, fw_domains);

	old_mcr = intel_uncore_read_fw(uncore, GEN8_MCR_SELECTOR);

	for_each_set_bit(mslice, &gt->info.mslice_mask, GEN12_MAX_MSLICES) {
		/* unicast access to selected mslice */
		intel_uncore_write_fw(uncore, GEN8_MCR_SELECTOR, GEN8_MCR_SLICE(mslice));

		fault = intel_uncore_read_fw(uncore, GEN12_RING_FAULT_REG);
		if (fault & RING_FAULT_VALID) {
			u32 fault_data0, fault_data1;
			u64 fault_addr;

			fault_data0 = intel_uncore_read_fw(uncore, GEN12_FAULT_TLB_DATA0);
			fault_data1 = intel_uncore_read_fw(uncore, GEN12_FAULT_TLB_DATA1);

			fault_addr = ((u64)(fault_data1 & FAULT_VA_HIGH_BITS) << 44) |
				     ((u64)fault_data0 << 12);

			DRM_DEBUG_DRIVER("Unexpected fault\n"
					 "\tM-slice: %u\n"
					 "\tAddr: 0x%08x_%08x\n"
					 "\tAddress space: %s\n"
					 "\tEngine ID: %d\n"
					 "\tSource ID: %d\n"
					 "\tLevel: %d\n",
					 mslice,
					 upper_32_bits(fault_addr),
					 lower_32_bits(fault_addr),
					 fault_data1 & FAULT_GTT_SEL ? "GGTT" : "PPGTT",
					 GEN8_RING_FAULT_ENGINE_ID(fault),
					 RING_FAULT_SRCID(fault),
					 RING_FAULT_LEVEL(fault));
		}
	}

	intel_uncore_write_fw(uncore, GEN8_MCR_SELECTOR, old_mcr);

	intel_uncore_forcewake_put__locked(uncore, fw_domains);
	spin_unlock_irq(&uncore->lock);
}

void intel_gt_check_and_clear_faults(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;

	if (IS_SRIOV_VF(i915))
		return;

	/* From GEN8 onwards we only have one 'All Engine Fault Register' */
	if (HAS_MSLICES(i915))
		xehpsdv_check_faults(gt);
	else if (GRAPHICS_VER(i915) >= 8)
		gen8_check_faults(gt);
	else if (GRAPHICS_VER(i915) >= 6)
		gen6_check_faults(gt);
	else
		return;

	intel_gt_clear_error_registers(gt, ALL_ENGINES);
}

void intel_gt_flush_ggtt_writes(struct intel_gt *gt)
{
	struct intel_uncore *uncore = gt->uncore;
	intel_wakeref_t wakeref;

	/*
	 * No actual flushing is required for the GTT write domain for reads
	 * from the GTT domain. Writes to it "immediately" go to main memory
	 * as far as we know, so there's no chipset flush. It also doesn't
	 * land in the GPU render cache.
	 *
	 * However, we do have to enforce the order so that all writes through
	 * the GTT land before any writes to the device, such as updates to
	 * the GATT itself.
	 *
	 * We also have to wait a bit for the writes to land from the GTT.
	 * An uncached read (i.e. mmio) seems to be ideal for the round-trip
	 * timing. This issue has only been observed when switching quickly
	 * between GTT writes and CPU reads from inside the kernel on recent hw,
	 * and it appears to only affect discrete GTT blocks (i.e. on LLC
	 * system agents we cannot reproduce this behaviour, until Cannonlake
	 * that was!).
	 */

	wmb();

	if (INTEL_INFO(gt->i915)->has_coherent_ggtt)
		return;

	intel_gt_chipset_flush(gt);

	with_intel_runtime_pm_if_in_use(uncore->rpm, wakeref) {
		unsigned long flags;

		spin_lock_irqsave(&uncore->lock, flags);
		intel_uncore_posting_read_fw(uncore,
					     RING_HEAD(RENDER_RING_BASE));
		spin_unlock_irqrestore(&uncore->lock, flags);
	}
}

void intel_gt_chipset_flush(struct intel_gt *gt)
{
	wmb();
	if (GRAPHICS_VER(gt->i915) < 6)
		intel_gtt_chipset_flush();
}

void intel_gt_driver_register(struct intel_gt *gt)
{
	if (gt->info.id == 0)
		intel_gsc_init(&gt->gsc, gt->i915);
	else
		drm_info(&gt->i915->drm, "Not initializing gsc for remote tiles\n");

	intel_rps_driver_register(&gt->rps);

	intel_gt_debugfs_register(gt);
	intel_gt_sysfs_register(gt);
	intel_iov_sysfs_setup(&gt->iov);
	intel_iov_vf_get_wakeref_wa(&gt->iov);
}

static int intel_gt_init_scratch(struct intel_gt *gt, unsigned int size)
{
	struct drm_i915_private *i915 = gt->i915;
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma;
	int ret;

	obj = intel_gt_object_create_lmem(gt, size, I915_BO_ALLOC_VOLATILE);
	if (IS_ERR(obj))
		obj = i915_gem_object_create_stolen(i915, size);
	if (IS_ERR(obj))
		obj = i915_gem_object_create_internal(i915, size);
	if (IS_ERR(obj)) {
		drm_err(&i915->drm, "Failed to allocate scratch page\n");
		return PTR_ERR(obj);
	}

	vma = i915_vma_instance(obj, &gt->ggtt->vm, NULL);
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto err_unref;
	}

	ret = i915_ggtt_pin(vma, NULL, 0, PIN_HIGH);
	if (ret)
		goto err_unref;

	gt->scratch = i915_vma_make_unshrinkable(vma);

	return 0;

err_unref:
	i915_gem_object_put(obj);
	return ret;
}

static void intel_gt_fini_scratch(struct intel_gt *gt)
{
	i915_vma_unpin_and_release(&gt->scratch, 0);
}

static void intel_gt_init_debug_pages(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma;
	int count = i915->params.debug_pages & ~BIT(31);
	bool lmem = i915->params.debug_pages & BIT(31);
	u32 size = count << PAGE_SHIFT;
	void* vaddr;

	if (!count)
		return;

	if (lmem) {
		if (!HAS_LMEM(i915)) {
			drm_err(&i915->drm, "No LMEM, skipping debug pages\n");
			return;
		}

		obj = intel_gt_object_create_lmem(gt, size,
						  I915_BO_ALLOC_CONTIGUOUS |
						  I915_BO_ALLOC_VOLATILE);
	} else {
		obj = i915_gem_object_create_shmem(i915, size);
	}
	if (IS_ERR(obj)) {
		drm_err(&i915->drm, "Failed to allocate debug pages\n");
		return;
	}

	vaddr = i915_gem_object_pin_map_unlocked(obj, I915_MAP_WC);
	if (!vaddr)
		goto err_unref;

	memset(vaddr, 0, size);

	i915_gem_object_unpin_map(obj);

	vma = i915_vma_instance(obj, &gt->ggtt->vm, NULL);
	if (IS_ERR(vma))
		goto err_unref;

	if (i915_ggtt_pin(vma, NULL, 0, PIN_HIGH))
		goto err_unref;

	gt->dbg = i915_vma_make_unshrinkable(vma);

	drm_dbg(&i915->drm,
		"gt%u debug pages allocated in %s: ggtt=0x%08x, phys=0x%016llx, size=0x%zx\n",
		gt->info.id,
		obj->mm.region->name,
		i915_ggtt_offset(vma),
		(u64)i915_gem_object_get_dma_address(obj, 0),
		obj->base.size);

	return;

err_unref:
	i915_gem_object_put(obj);
	drm_err(&i915->drm, "Failed to init debug pages\n");
	return;
}

static void intel_gt_fini_debug_pages(struct intel_gt *gt)
{
	if (gt->dbg)
		i915_vma_unpin_and_release(&gt->dbg, 0);
}

static struct i915_address_space *kernel_vm(struct intel_gt *gt)
{
	struct i915_ppgtt *ppgtt;
	int err;

	if (INTEL_PPGTT(gt->i915) <= INTEL_PPGTT_ALIASING)
		return i915_vm_get(&gt->ggtt->vm);

	ppgtt = i915_ppgtt_create(gt, 0);
	if (IS_ERR(ppgtt))
		return ERR_CAST(ppgtt);

	/* Setup a 1:1 mapping into our portion of lmem */
	if (gt->lmem) {
		gt->flat.start = round_down(gt->lmem->region.start, SZ_1G);
		gt->flat.size  = round_up(gt->lmem->region.end, SZ_1G);
		gt->flat.size -= gt->flat.start;
		gt->flat.color = I915_COLOR_UNEVICTABLE;
		drm_dbg(&gt->i915->drm,
			"Using flat ppGTT [%llx + %llx]\n",
			gt->flat.start, gt->flat.size);

		err = intel_flat_lmem_ppgtt_init(&ppgtt->vm, &gt->flat);
		if (err) {
			i915_vm_put(&ppgtt->vm);
			return ERR_PTR(err);
		}
	}

	return &ppgtt->vm;
}

static void release_vm(struct intel_gt *gt)
{
	struct i915_address_space *vm;

	vm = fetch_and_zero(&gt->vm);
	if (!vm)
		return;

	intel_flat_lmem_ppgtt_fini(vm, &gt->flat);
	i915_vm_put(vm);
}

static int __engines_record_defaults(struct intel_gt *gt)
{
	struct i915_request *requests[I915_NUM_ENGINES] = {};
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	int err = 0;

	/*
	 * As we reset the gpu during very early sanitisation, the current
	 * register state on the GPU should reflect its defaults values.
	 * We load a context onto the hw (with restore-inhibit), then switch
	 * over to a second context to save that default register state. We
	 * can then prime every new context with that state so they all start
	 * from the same default HW values.
	 */

	for_each_engine(engine, gt, id) {
		struct intel_renderstate so;
		struct intel_context *ce;
		struct i915_request *rq;

		/* We must be able to switch to something! */
		GEM_BUG_ON(!engine->kernel_context);

		ce = intel_context_create(engine);
		if (IS_ERR(ce)) {
			err = PTR_ERR(ce);
			goto out;
		}

		err = intel_renderstate_init(&so, ce);
		if (err)
			goto err;

		rq = i915_request_create(ce);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			goto err_fini;
		}

		err = intel_engine_emit_ctx_wa(rq);
		if (err)
			goto err_rq;

		err = intel_renderstate_emit(&so, rq);
		if (err)
			goto err_rq;

err_rq:
		requests[id] = i915_request_get(rq);
		i915_request_add(rq);
err_fini:
		intel_renderstate_fini(&so, ce);
err:
		if (err) {
			intel_context_put(ce);
			goto out;
		}
	}

	/* Flush the default context image to memory, and enable powersaving. */
	if (intel_gt_wait_for_idle(gt, I915_GEM_IDLE_TIMEOUT) == -ETIME) {
		err = -EIO;
		goto out;
	}

	for (id = 0; id < ARRAY_SIZE(requests); id++) {
		struct i915_request *rq;
		struct file *state;

		rq = requests[id];
		if (!rq)
			continue;

		if (rq->fence.error) {
			err = -EIO;
			goto out;
		}

		GEM_BUG_ON(!test_bit(CONTEXT_ALLOC_BIT, &rq->context->flags));
		if (!rq->context->state)
			continue;

		/* Keep a copy of the state's backing pages; free the obj */
		state = shmem_create_from_object(rq->context->state->obj);
		if (IS_ERR(state)) {
			err = PTR_ERR(state);
			goto out;
		}
		rq->engine->default_state = state;
	}

out:
	/*
	 * If we have to abandon now, we expect the engines to be idle
	 * and ready to be torn-down. The quickest way we can accomplish
	 * this is by declaring ourselves wedged.
	 */
	if (err)
		intel_gt_set_wedged(gt);

	for (id = 0; id < ARRAY_SIZE(requests); id++) {
		struct intel_context *ce;
		struct i915_request *rq;

		rq = requests[id];
		if (!rq)
			continue;

		ce = rq->context;
		i915_request_put(rq);
		intel_context_put(ce);
	}
	return err;
}

static int __engines_verify_workarounds(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	int err = 0;

	if (!IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GEM))
		return 0;

	for_each_engine(engine, gt, id) {
		if (intel_engine_verify_workarounds(engine, "load"))
			err = -EIO;
	}

	/* Flush and restore the kernel context for safety */
	if (intel_gt_wait_for_idle(gt, I915_GEM_IDLE_TIMEOUT) == -ETIME)
		err = -EIO;

	return err;
}

static void __intel_gt_disable(struct intel_gt *gt)
{
	intel_gt_set_wedged_on_fini(gt);

	intel_gt_suspend_prepare(gt);
	intel_gt_suspend_late(gt);

	GEM_BUG_ON(intel_gt_pm_is_awake(gt));
}

int intel_gt_wait_for_idle(struct intel_gt *gt, long timeout)
{
	long remaining_timeout;

	/* If the device is asleep, we have no requests outstanding */
	if (!intel_gt_pm_is_awake(gt))
		return 0;

	while ((timeout = intel_gt_retire_requests_timeout(gt, timeout,
							   &remaining_timeout)) > 0) {
		cond_resched();
		if (signal_pending(current))
			return -EINTR;
	}

	return timeout ? timeout : intel_uc_wait_for_idle(&gt->uc,
							  remaining_timeout);
}

int intel_gt_init(struct intel_gt *gt)
{
	struct i915_address_space *vm;
	int err;

	err = i915_inject_probe_error(gt->i915, -ENODEV);
	if (err)
		return err;

	intel_gt_init_workarounds(gt);

	/*
	 * This is just a security blanket to placate dragons.
	 * On some systems, we very sporadically observe that the first TLBs
	 * used by the CS may be stale, despite us poking the TLB reset. If
	 * we hold the forcewake during initialisation these problems
	 * just magically go away.
	 */
	intel_uncore_forcewake_get(gt->uncore, FORCEWAKE_ALL);

	err = intel_iov_init(&gt->iov);
	if (unlikely(err))
		goto out_fw;

	err = intel_gt_init_scratch(gt,
				    GRAPHICS_VER(gt->i915) == 2 ? SZ_256K : SZ_4K);
	if (err)
		goto err_iov;

	intel_gt_init_debug_pages(gt);

	intel_gt_pm_init(gt);

	vm = kernel_vm(gt);
	if (IS_ERR(vm)) {
		err = PTR_ERR(vm);
		goto err_pm;
	}
	gt->vm = vm;

	intel_set_mocs_index(gt);

	err = intel_engines_init(gt);
	if (err)
		goto err_engines;

	err = intel_uc_init(&gt->uc);
	if (err)
		goto err_engines;

	err = intel_gt_resume(gt);
	if (err)
		goto err_uc_init;

	err = intel_iov_init_late(&gt->iov);
	if (err)
		goto err_gt;

	err = __engines_record_defaults(gt);
	if (err)
		goto err_gt;

	err = __engines_verify_workarounds(gt);
	if (err)
		goto err_gt;

	intel_uc_init_late(&gt->uc);

	err = i915_inject_probe_error(gt->i915, -EIO);
	if (err)
		goto err_gt;

	intel_pxp_init(&gt->pxp);

	goto out_fw;
err_gt:
	__intel_gt_disable(gt);
	intel_uc_fini_hw(&gt->uc);
err_uc_init:
	intel_uc_fini(&gt->uc);
err_engines:
	intel_engines_release(gt);
	release_vm(gt);
err_pm:
	intel_gt_pm_fini(gt);
	intel_gt_fini_debug_pages(gt);
	intel_gt_fini_scratch(gt);
err_iov:
	intel_iov_fini(&gt->iov);
out_fw:
	if (err)
		intel_gt_set_wedged_on_init(gt);
	intel_uncore_forcewake_put(gt->uncore, FORCEWAKE_ALL);
	return err;
}

void intel_gt_driver_remove(struct intel_gt *gt)
{
	intel_gt_fini_clock_frequency(gt);

	intel_flat_ppgtt_pool_fini(&gt->fpp);
	intel_iov_fini_hw(&gt->iov);

	__intel_gt_disable(gt);

	intel_uc_driver_remove(&gt->uc);

	intel_engines_release(gt);

	intel_gt_flush_buffer_pool(gt);
}

void intel_gt_driver_unregister(struct intel_gt *gt)
{
	intel_wakeref_t wakeref;

	intel_iov_vf_put_wakeref_wa(&gt->iov);

	if (!gt->i915->drm.unplugged)
		intel_iov_sysfs_teardown(&gt->iov);

	intel_gt_sysfs_unregister(gt);
	intel_rps_driver_unregister(&gt->rps);
	if (gt->info.id == 0)
		intel_gsc_fini(&gt->gsc);

	intel_pxp_fini(&gt->pxp);

	/*
	 * Upon unregistering the device to prevent any new users, cancel
	 * all in-flight requests so that we can quickly unbind the active
	 * resources.
	 */
	intel_gt_set_wedged_on_fini(gt);

	/* Scrub all HW state upon release */
	with_intel_runtime_pm(gt->uncore->rpm, wakeref)
		__intel_gt_reset(gt, ALL_ENGINES);

	xa_destroy(&gt->errors.soc);
}

void intel_gt_driver_release(struct intel_gt *gt)
{
	release_vm(gt);

	intel_wa_list_free(&gt->wa_list);
	intel_gt_pm_fini(gt);
	intel_gt_fini_debug_pages(gt);
	intel_gt_fini_scratch(gt);
	intel_gt_fini_buffer_pool(gt);
	intel_iov_fini(&gt->iov);
}

void intel_gt_driver_late_release(struct intel_gt *gt)
{
	/* We need to wait for inflight RCU frees to release their grip */
	rcu_barrier();

	mutex_destroy(&gt->mutex);

	intel_iov_release(&gt->iov);
	intel_uc_driver_late_release(&gt->uc);
	intel_gt_fini_requests(gt);
	intel_gt_fini_reset(gt);
	intel_gt_fini_timelines(gt);
	intel_engines_free(gt);
}

void intel_gt_shutdown(struct intel_gt *gt)
{
	intel_iov_vf_put_wakeref_wa(&gt->iov);
}

/**
 * intel_gt_reg_needs_read_steering - determine whether a register read
 *     requires explicit steering
 * @gt: GT structure
 * @reg: the register to check steering requirements for
 * @type: type of multicast steering to check
 *
 * Determines whether @reg needs explicit steering of a specific type for
 * reads.
 *
 * Returns false if @reg does not belong to a register range of the given
 * steering type, or if the default (subslice-based) steering IDs are suitable
 * for @type steering too.
 */
static bool intel_gt_reg_needs_read_steering(struct intel_gt *gt,
					     i915_reg_t reg,
					     enum intel_steering_type type)
{
	const u32 offset = i915_mmio_reg_offset(reg);
	const struct intel_mmio_range *entry;

	if (likely(!intel_gt_needs_read_steering(gt, type)))
		return false;

	for (entry = gt->steering_table[type]; entry->end; entry++) {
		if (offset >= entry->start && offset <= entry->end)
			return true;
	}

	return false;
}

/**
 * intel_gt_get_valid_steering - determines valid IDs for a class of MCR steering
 * @gt: GT structure
 * @type: multicast register type
 * @sliceid: Slice ID returned
 * @subsliceid: Subslice ID returned
 *
 * Determines sliceid and subsliceid values that will steer reads
 * of a specific multicast register class to a valid value.
 */
static void intel_gt_get_valid_steering(struct intel_gt *gt,
					enum intel_steering_type type,
					u8 *sliceid, u8 *subsliceid)
{
	switch (type) {
	case L3BANK:
		GEM_DEBUG_WARN_ON(!gt->info.l3bank_mask); /* should be impossible! */

		*sliceid = 0;		/* unused */
		*subsliceid = __ffs(gt->info.l3bank_mask);
		break;
	case MSLICE:
		GEM_DEBUG_WARN_ON(!gt->info.mslice_mask); /* should be impossible! */

		*sliceid = __ffs(gt->info.mslice_mask);
		*subsliceid = 0;	/* unused */
		break;
	case LNCF:
		GEM_DEBUG_WARN_ON(!gt->info.mslice_mask); /* should be impossible! */

		/*
		 * An LNCF is always present if its mslice is present, so we
		 * can safely just steer to LNCF 0 in all cases.
		 */
		*sliceid = __ffs(gt->info.mslice_mask) << 1;
		*subsliceid = 0;	/* unused */
		break;
	case BSLICE:
		*sliceid = 0;		/* first (half-)bslice is always present */
		*subsliceid = 0;	/* first instance is always present */
		break;
	default:
		MISSING_CASE(type);
		*sliceid = 0;
		*subsliceid = 0;
	}
}

/**
 * intel_gt_read_register_fw - reads a GT register with support for multicast
 * @gt: GT structure
 * @reg: register to read
 *
 * This function will read a GT register.  If the register is a multicast
 * register, the read will be steered to a valid instance (i.e., one that
 * isn't fused off or powered down by power gating).
 *
 * Returns the value from a valid instance of @reg.
 */
u32 intel_gt_read_register_fw(struct intel_gt *gt, i915_reg_t reg)
{
	int type;
	u8 sliceid, subsliceid;

	for (type = 0; type < NUM_STEERING_TYPES; type++) {
		if (intel_gt_reg_needs_read_steering(gt, reg, type)) {
			intel_gt_get_valid_steering(gt, type, &sliceid,
						    &subsliceid);
			return intel_uncore_read_with_mcr_steering_fw(gt->uncore,
								      reg,
								      sliceid,
								      subsliceid);
		}
	}

	return intel_uncore_read_fw(gt->uncore, reg);
}

u32 intel_gt_read_register(struct intel_gt *gt, i915_reg_t reg)
{
	int type;
	u8 sliceid, subsliceid;

	for (type = 0; type < NUM_STEERING_TYPES; type++) {
		if (intel_gt_reg_needs_read_steering(gt, reg, type)) {
			intel_gt_get_valid_steering(gt, type, &sliceid,
						    &subsliceid);
			return intel_uncore_read_with_mcr_steering(gt->uncore,
								   reg,
								   sliceid,
								   subsliceid);
		}
	}

	return intel_uncore_read(gt->uncore, reg);
}

/**
 * intel_gt_get_valid_steering_for_reg - get a valid steering for a register
 * @gt: GT structure
 * @reg: register for which the steering is required
 * @sliceid: return variable for slice steering
 * @subsliceid: return variable for subslice steering
 *
 * This function returns a slice/subslice pair that is guaranteed to work for
 * read steering of the given register. Note that a value will be returned even
 * if the register is not replicated and therefore does not actually require
 * steering.
 */
void intel_gt_get_valid_steering_for_reg(struct intel_gt *gt, i915_reg_t reg,
					 u8 *sliceid, u8 *subsliceid)
{
	int type;

	for (type = 0; type < NUM_STEERING_TYPES; type++) {
		if (intel_gt_reg_needs_read_steering(gt, reg, type)) {
			intel_gt_get_valid_steering(gt, type, sliceid,
						    subsliceid);
			return;
		}
	}

	*sliceid = gt->default_steering.groupid;
	*subsliceid = gt->default_steering.instanceid;
}

static void report_steering_type(struct drm_printer *p,
				 struct intel_gt *gt,
				 enum intel_steering_type type,
				 bool dump_table)
{
	const struct intel_mmio_range *entry;
	u8 slice, subslice;

	BUILD_BUG_ON(ARRAY_SIZE(intel_steering_types) != NUM_STEERING_TYPES);

	if (!gt->steering_table[type]) {
		drm_printf(p, "%s steering: uses default steering\n",
			   intel_steering_types[type]);
		return;
	}

	intel_gt_get_valid_steering(gt, type, &slice, &subslice);
	drm_printf(p, "%s steering: sliceid=0x%x, subsliceid=0x%x\n",
		   intel_steering_types[type], slice, subslice);

	if (!dump_table)
		return;

	for (entry = gt->steering_table[type]; entry->end; entry++)
		drm_printf(p, "\t0x%06x - 0x%06x\n", entry->start, entry->end);
}

void intel_gt_report_steering(struct drm_printer *p, struct intel_gt *gt,
			      bool dump_table)
{
	drm_printf(p, "Default steering: sliceid=0x%x, subsliceid=0x%x\n",
		   gt->default_steering.groupid,
		   gt->default_steering.instanceid);

	if (HAS_MSLICES(gt->i915)) {
		report_steering_type(p, gt, MSLICE, dump_table);
		report_steering_type(p, gt, LNCF, dump_table);
	} else if (HAS_BSLICES(gt->i915)) {
		report_steering_type(p, gt, BSLICE, dump_table);
	}
}

static int
tile_setup(struct intel_gt *gt,
	   unsigned int id,
	   struct drm_i915_private *i915,
	   phys_addr_t phys_addr)
{
	struct intel_uncore *uncore;
	struct intel_uncore_mmio_debug *mmio_debug;
	int ret;

	gt->phys_addr = phys_addr;
	gt->info.id = id;

	if (id) {
		uncore = kzalloc(sizeof(*uncore), GFP_KERNEL);
		if (!uncore)
			return -ENOMEM;

		mmio_debug = kzalloc(sizeof(*mmio_debug), GFP_KERNEL);
		if (!mmio_debug) {
			kfree(uncore);
			return -ENOMEM;
		}

		__intel_gt_init_early(gt, uncore, mmio_debug, i915);
	} else {
		uncore = &i915->uncore;
		mmio_debug = &i915->mmio_debug;
	}

	uncore->gt = gt;

	intel_uncore_mmio_debug_init_early(mmio_debug);
	intel_uncore_init_early(uncore, gt, mmio_debug);

	ret = intel_uncore_setup_mmio(gt->uncore, phys_addr);
	if (ret)
		return ret;

	ret = intel_iov_init_mmio(&gt->iov);
	if (unlikely(ret))
		return ret;

	intel_iov_init_early(&gt->iov);

	/* Which tile am I? default to zero on single tile systems */
	if (HAS_REMOTE_TILES(i915) && !IS_SRIOV_VF(i915)) {
		u32 instance =
			__raw_uncore_read32(gt->uncore, XEHPSDV_MTCFG_ADDR) &
			TILE_NUMBER;

		if (GEM_WARN_ON(instance != id))
			return -ENXIO;
	}

	return 0;
}

static void tile_cleanup(struct intel_gt *gt)
{
	intel_uncore_cleanup_mmio(gt->uncore);

	if (gt->info.id) {
		kfree(gt->mmio_debug);
		kfree(gt->uncore);
		kfree(gt);
	}
}

static unsigned int tile_count(struct drm_i915_private *i915)
{
	u32 mtcfg;

	/*
	 * VFs can't access XEHPSDV_MTCFG_ADDR register directly.
	 * But they only care about tiles where they were assigned.
	 */
	if (IS_SRIOV_VF(i915)) {
		u32 tile_mask = to_root_gt(i915)->iov.vf.config.tile_mask;

		if (GEM_WARN_ON(!tile_mask))
			return 1;

		return fls(tile_mask);
	}

	/*
	 * We use raw MMIO reads at this point since the
	 * MMIO vfuncs are not setup yet
	 */
	mtcfg = __raw_uncore_read32(&i915->uncore, XEHPSDV_MTCFG_ADDR);
	return REG_FIELD_GET(TILE_COUNT, mtcfg) + 1;
}

static unsigned int tile_mask(struct drm_i915_private *i915)
{
	unsigned long mask;

	if (!HAS_REMOTE_TILES(i915))
		mask = BIT(0);
	else if (IS_SRIOV_VF(i915))
		mask = to_root_gt(i915)->iov.vf.config.tile_mask;
	else
		mask = GENMASK(tile_count(i915) - 1, 0);

	return mask;
}

int intel_gt_tiles_setup(struct drm_i915_private *i915)
{
	struct pci_dev *pdev = i915->drm.pdev;
	phys_addr_t phys_addr;
	unsigned int mmio_bar;
	unsigned int i, tiles;
	unsigned long enabled_tiles_mask;
	struct intel_gt *gt;
	int ret;

	mmio_bar = GRAPHICS_VER(i915) == 2 ? 1 : 0;
	phys_addr = pci_resource_start(pdev, mmio_bar);

	/* Setup root device first */
	gt = to_root_gt(i915);

	ret = tile_setup(gt, 0, i915, phys_addr);
	if (ret)
		return ret;

	if (!HAS_REMOTE_TILES(i915)) {
		i915->gts[0] = gt;
		return 0;
	}

	enabled_tiles_mask = tile_mask(i915);
	if (enabled_tiles_mask & BIT(0))
		i915->gts[0] = gt;

	/* Setup other tiles */
	tiles = tile_count(i915);
	drm_info(&i915->drm, "Tile count: %u\n", tiles);

	if (GEM_WARN_ON(tiles > I915_MAX_TILES))
		return -EINVAL;

	/* For Modern GENs size of GTTMMADR is 16MB (for each tile) */
	if (IS_SRIOV_VF(i915)) {
		if (GEM_WARN_ON(pci_resource_len(pdev, 0) < tiles * SZ_16M))
			return -EINVAL;
	} else {
		if (GEM_WARN_ON(pci_resource_len(pdev, 0) / tiles != SZ_16M))
			return -EINVAL;
	}

	i = 1;
	for_each_set_bit_from(i, &enabled_tiles_mask, I915_MAX_TILES) {
		gt = kzalloc(sizeof(*gt), GFP_KERNEL);
		if (!gt) {
			ret = -ENOMEM;
			goto err;
		}

		ret = tile_setup(gt, i, i915, phys_addr + SZ_16M * i);
		if (ret)
			goto err;

		i915->gts[i] = gt;
	}

	i915->remote_tiles = tiles - 1;

	return 0;

err:
	i915_probe_error(i915, "Failed to initialize tile %u! (%d)\n", i, ret);

	for_each_gt(i915, i, gt) {
		tile_cleanup(gt);
		i915->gts[i] = NULL;
	}

	return ret;
}

int intel_gt_tiles_init(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int id;
	int ret;

	for_each_gt(i915, id, gt) {
		if (id > i915->remote_tiles)
			break;

		ret = intel_gt_probe_lmem(gt);
		if (ret)
			return ret;
	}

	return 0;
}

void intel_gt_tiles_cleanup(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int id;

	for_each_gt(i915, id, gt) {
		tile_cleanup(gt);
		i915->gts[id] = NULL;
	}
}

void intel_gt_info_print(const struct intel_gt_info *info,
			 struct drm_printer *p)
{
	drm_printf(p, "GT %u info:\n", info->id);
	drm_printf(p, "available engines: %x\n", info->engine_mask);

	intel_sseu_dump(&info->sseu, p);
}

int intel_gt_get_l3bank_count(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;

	if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 60)) {
		/* FIXME: Calculate this from fuse values */
		return 64;
	} else if (GRAPHICS_VER(i915) >= 12) {
		intel_wakeref_t wakeref;
		u32 fuse3 = 0;

		with_intel_runtime_pm(gt->uncore->rpm, wakeref)
			fuse3 = intel_uncore_read(gt->uncore, GEN10_MIRROR_FUSE3);
		if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 50))
			return hweight32(REG_FIELD_GET(GEN12_MEML3_EN_MASK, fuse3)) * 8;
		else
			return hweight32(REG_FIELD_GET(GEN12_GT_L3_MODE_MASK, ~fuse3));
	} else {
		return -ENODEV;
	}
}
