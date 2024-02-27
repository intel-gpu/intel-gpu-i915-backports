// SPDX-License-Identifier: MIT

/*
 * Copyright © 2019 Intel Corporation
 */

#include <linux/seq_file.h>
#include <linux/string_helpers.h>

#include "i915_drv.h"
#include "i915_reg.h"
#include "intel_gt.h"
#include "intel_gt_clock_utils.h"
#include "intel_gt_debugfs.h"
#include "intel_gt_pm.h"
#include "intel_gt_pm_debugfs.h"
#include "intel_gt_regs.h"
#include "intel_llc.h"
#include "intel_mchbar_regs.h"
#include "intel_pcode.h"
#include "intel_rc6.h"
#include "intel_rps.h"
#include "intel_runtime_pm.h"
#include "intel_uncore.h"

void intel_gt_pm_debugfs_forcewake_user_open(struct intel_gt *gt)
{
	atomic_inc(&gt->user_wakeref);
	intel_gt_pm_get_untracked(gt);
	if (GRAPHICS_VER(gt->i915) >= 6)
		intel_uncore_forcewake_user_get(gt->uncore);
}

void intel_gt_pm_debugfs_forcewake_user_release(struct intel_gt *gt)
{
	if (GRAPHICS_VER(gt->i915) >= 6)
		intel_uncore_forcewake_user_put(gt->uncore);
	intel_gt_pm_put_untracked(gt);
	atomic_dec(&gt->user_wakeref);
}

static int forcewake_user_open(struct inode *inode, struct file *file)
{
	struct intel_gt *gt = inode->i_private;

	intel_gt_pm_debugfs_forcewake_user_open(gt);

	return 0;
}

static int forcewake_user_release(struct inode *inode, struct file *file)
{
	struct intel_gt *gt = inode->i_private;

	intel_gt_pm_debugfs_forcewake_user_release(gt);

	return 0;
}

DEFINE_I915_GT_RAW_ATTRIBUTE(forcewake_user_fops, forcewake_user_open,
			     forcewake_user_release, NULL, NULL, NULL);

static int fw_domains_show(struct seq_file *m, void *data)
{
	struct intel_gt *gt = m->private;
	struct intel_uncore *uncore = gt->uncore;
	struct intel_uncore_forcewake_domain *fw_domain;
	unsigned int tmp;

	seq_printf(m, "user.bypass_count = %u\n",
		   uncore->user_forcewake_count);

	for_each_fw_domain(fw_domain, uncore, tmp)
		seq_printf(m, "%s.wake_count = %u\n",
			   intel_uncore_forcewake_domain_to_str(fw_domain->id),
			   READ_ONCE(fw_domain->wake_count));

	return 0;
}
DEFINE_INTEL_GT_DEBUGFS_ATTRIBUTE(fw_domains);

static void print_rc6_res(struct seq_file *m,
			  const char *title,
			  const i915_reg_t reg)
{
	struct intel_gt *gt = m->private;
	intel_wakeref_t wakeref;

	with_intel_runtime_pm(gt->uncore->rpm, wakeref)
		seq_printf(m, "%s %u (%llu us)\n", title,
			   intel_uncore_read(gt->uncore, reg),
			   intel_rc6_residency_us(&gt->rc6, reg));
}

static int gen6_drpc(struct seq_file *m)
{
	struct intel_gt *gt = m->private;
	struct drm_i915_private *i915 = gt->i915;
	struct intel_uncore *uncore = gt->uncore;
	u32 gt_core_status, mt_fwake_req, rcctl1, rc6vids = 0;
	u32 gen9_powergate_enable = 0, gen9_powergate_status = 0;

	mt_fwake_req = intel_uncore_read_fw(uncore, FORCEWAKE_MT);
	gt_core_status = intel_uncore_read_fw(uncore, GEN6_GT_CORE_STATUS);

	rcctl1 = intel_uncore_read(uncore, GEN6_RC_CONTROL);
	if (GRAPHICS_VER(i915) >= 9) {
		gen9_powergate_enable =
			intel_uncore_read(uncore, GEN9_PG_ENABLE);
		gen9_powergate_status =
			intel_uncore_read(uncore, GEN9_PWRGT_DOMAIN_STATUS);
	}

	if (GRAPHICS_VER(i915) <= 7)
		snb_pcode_read(gt->uncore, GEN6_PCODE_READ_RC6VIDS, &rc6vids, NULL);

	seq_printf(m, "RC1e Enabled: %s\n",
		   str_yes_no(rcctl1 & GEN6_RC_CTL_RC1e_ENABLE));
	seq_printf(m, "RC6 Enabled: %s\n",
		   str_yes_no(rcctl1 & GEN6_RC_CTL_RC6_ENABLE));
	if (GRAPHICS_VER(i915) >= 9) {
		seq_printf(m, "Render Well Gating Enabled: %s\n",
			   str_yes_no(gen9_powergate_enable & GEN9_RENDER_PG_ENABLE));
		seq_printf(m, "Media Well Gating Enabled: %s\n",
			   str_yes_no(gen9_powergate_enable & GEN9_MEDIA_PG_ENABLE));
	}
	seq_printf(m, "Deep RC6 Enabled: %s\n",
		   str_yes_no(rcctl1 & GEN6_RC_CTL_RC6p_ENABLE));
	seq_printf(m, "Deepest RC6 Enabled: %s\n",
		   str_yes_no(rcctl1 & GEN6_RC_CTL_RC6pp_ENABLE));
	seq_puts(m, "Current RC state: ");
	switch (gt_core_status & GEN6_RCn_MASK) {
	case GEN6_RC0:
		if (gt_core_status & GEN6_CORE_CPD_STATE_MASK)
			seq_puts(m, "Core Power Down\n");
		else
			seq_puts(m, "on\n");
		break;
	case GEN6_RC3:
		seq_puts(m, "RC3\n");
		break;
	case GEN6_RC6:
		seq_puts(m, "RC6\n");
		break;
	case GEN6_RC7:
		seq_puts(m, "RC7\n");
		break;
	default:
		seq_puts(m, "Unknown\n");
		break;
	}

	seq_printf(m, "Core Power Down: %s\n",
		   str_yes_no(gt_core_status & GEN6_CORE_CPD_STATE_MASK));
	seq_printf(m, "Multi-threaded Forcewake Request: 0x%x\n", mt_fwake_req);
	if (GRAPHICS_VER(i915) >= 9) {
		seq_printf(m, "Render Power Well: %s\n",
			   (gen9_powergate_status &
			    GEN9_PWRGT_RENDER_STATUS_MASK) ? "Up" : "Down");
		seq_printf(m, "Media Power Well: %s\n",
			   (gen9_powergate_status &
			    GEN9_PWRGT_MEDIA_STATUS_MASK) ? "Up" : "Down");
	}

	/* Not exactly sure what this is */
	print_rc6_res(m, "RC6 \"Locked to RPn\" residency since boot:",
		      GEN6_GT_GFX_RC6_LOCKED);
	print_rc6_res(m, "RC6 residency since boot:", GEN6_GT_GFX_RC6);

	/*
	 * TODO:As per BSpec:52453 GT RPM unit residency NS should be equal to
	 * intel_rc6_rpm_unit_residency(&gt->rc6) * gt->clock_period_ns
	 * But that is no where equivlent to GEN6_GT_GFX_RC6 NS on actual HW.
	 * Need to figure out correct counter increment frequency from HW Team.
	 */
	if (GRAPHICS_VER(i915) >= 12)
		seq_printf(m, "GT RC6 RPM Unit Residency since last RC6 exit: 0x%llx\n",
			   intel_rc6_rpm_unit_residency(&gt->rc6));

	print_rc6_res(m, "RC6+ residency since boot:", GEN6_GT_GFX_RC6p);
	print_rc6_res(m, "RC6++ residency since boot:", GEN6_GT_GFX_RC6pp);

	if (GRAPHICS_VER(i915) <= 7) {
		seq_printf(m, "RC6   voltage: %dmV\n",
			   GEN6_DECODE_RC6_VID(((rc6vids >> 0) & 0xff)));
		seq_printf(m, "RC6+  voltage: %dmV\n",
			   GEN6_DECODE_RC6_VID(((rc6vids >> 8) & 0xff)));
		seq_printf(m, "RC6++ voltage: %dmV\n",
			   GEN6_DECODE_RC6_VID(((rc6vids >> 16) & 0xff)));
	}

	return fw_domains_show(m, NULL);
}

static int mtl_drpc(struct seq_file *m)
{
	struct intel_gt *gt = m->private;
	struct intel_uncore *uncore = gt->uncore;
	u32 gt_core_status, rcctl1;
	u32 mtl_powergate_enable = 0, mtl_powergate_status = 0;
	i915_reg_t reg;

	gt_core_status = intel_uncore_read(uncore, MTL_MIRROR_TARGET_WP1);

	rcctl1 = intel_uncore_read(uncore, GEN6_RC_CONTROL);
	mtl_powergate_enable = intel_uncore_read(uncore, GEN9_PG_ENABLE);
	mtl_powergate_status = intel_uncore_read(uncore,
						 GEN9_PWRGT_DOMAIN_STATUS);

	seq_printf(m, "RC6 Enabled: %s\n",
		   str_yes_no(rcctl1 & GEN6_RC_CTL_RC6_ENABLE));
	if (gt->type == GT_MEDIA) {
		seq_printf(m, "Media Well Gating Enabled: %s\n",
		   str_yes_no(mtl_powergate_enable & GEN9_MEDIA_PG_ENABLE));
	} else {
		seq_printf(m, "Render Well Gating Enabled: %s\n",
		   str_yes_no(mtl_powergate_enable & GEN9_RENDER_PG_ENABLE));
	}

	seq_puts(m, "Current RC state: ");

	switch ((gt_core_status & MTL_CC_MASK) >> MTL_CC_SHIFT) {
	case MTL_CC0:
		seq_puts(m, "on\n");
		break;
	case MTL_CC6:
		seq_puts(m, "RC6\n");
		break;
	default:
		seq_puts(m, "Unknown\n");
		break;
	}

	if (gt->type == GT_MEDIA)
		seq_printf(m, "Media Power Well: %s\n",
			   (mtl_powergate_status &
			    GEN9_PWRGT_MEDIA_STATUS_MASK) ? "Up" : "Down");
	else
		seq_printf(m, "Render Power Well: %s\n",
			   (mtl_powergate_status &
			    GEN9_PWRGT_RENDER_STATUS_MASK) ? "Up" : "Down");

	reg = (gt->type == GT_MEDIA) ? MTL_MEDIA_MC6 : GEN6_GT_GFX_RC6;
	print_rc6_res(m, "RC6 residency since boot:", reg);

	return fw_domains_show(m, NULL);
}

static int drpc_show(struct seq_file *m, void *unused)
{
	struct intel_gt *gt = m->private;
	struct drm_i915_private *i915 = gt->i915;
	intel_wakeref_t wakeref;
	int err = -ENODEV;

	with_intel_runtime_pm(gt->uncore->rpm, wakeref) {
		if (MEDIA_VER(i915) >= 13)
			err = mtl_drpc(m);
		else
			err = gen6_drpc(m);
	}

	return err;
}
DEFINE_INTEL_GT_DEBUGFS_ATTRIBUTE(drpc);

static int gt_c6_residency_show(struct seq_file *m, void *unused)
{
	struct intel_gt *gt = m->private;
	struct drm_i915_private *i915 = gt->i915;

	if (GRAPHICS_VER(i915) < 12)
		return -ENODEV;

	seq_printf(m, "0x%llx\n", intel_rc6_rpm_unit_residency(&gt->rc6));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(gt_c6_residency);

void intel_gt_pm_frequency_dump(struct intel_gt *gt, struct drm_printer *p)
{
	struct drm_i915_private *i915 = gt->i915;
	struct intel_uncore *uncore = gt->uncore;
	struct intel_rps *rps = &gt->rps;
	intel_wakeref_t wakeref;

	wakeref = intel_runtime_pm_get(uncore->rpm);

	gen6_rps_frequency_dump(rps, p);

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
	drm_printf(p, "Current CD clock frequency: %d kHz\n", i915->cdclk.hw.cdclk);
#endif
	drm_printf(p, "Max CD clock frequency: %d kHz\n", i915->max_cdclk_freq);
	drm_printf(p, "Max pixel clock frequency: %d kHz\n", i915->max_dotclk_freq);

	intel_runtime_pm_put(uncore->rpm, wakeref);
}

static int frequency_show(struct seq_file *m, void *unused)
{
	struct intel_gt *gt = m->private;
	struct drm_printer p = drm_seq_file_printer(m);

	intel_gt_pm_frequency_dump(gt, &p);

	return 0;
}
DEFINE_INTEL_GT_DEBUGFS_ATTRIBUTE(frequency);

static int llc_show(struct seq_file *m, void *data)
{
	struct intel_gt *gt = m->private;
	struct drm_i915_private *i915 = gt->i915;
	struct intel_rps *rps = &gt->rps;
	unsigned int max_gpu_freq, min_gpu_freq;
	intel_wakeref_t wakeref;
	int gpu_freq, ia_freq;

	seq_printf(m, "LLC: %s\n", str_yes_no(HAS_LLC(i915)));

	min_gpu_freq = rps->min_freq;
	max_gpu_freq = rps->max_freq;

	/* Convert GT frequency to 50 HZ units */
	min_gpu_freq /= GEN9_FREQ_SCALER;
	max_gpu_freq /= GEN9_FREQ_SCALER;

	seq_puts(m, "GPU freq (MHz)\tEffective CPU freq (MHz)\tEffective Ring freq (MHz)\n");

	wakeref = intel_runtime_pm_get(gt->uncore->rpm);
	for (gpu_freq = min_gpu_freq; gpu_freq <= max_gpu_freq; gpu_freq++) {
		ia_freq = gpu_freq;
		snb_pcode_read(gt->uncore, GEN6_PCODE_READ_MIN_FREQ_TABLE,
			       &ia_freq, NULL);
		seq_printf(m, "%d\t\t%d\t\t\t\t%d\n",
			   intel_gpu_freq(rps,
					  (gpu_freq *
					   (IS_GEN9_BC(i915) ||
					    GRAPHICS_VER(i915) >= 11 ?
					    GEN9_FREQ_SCALER : 1))),
			   ((ia_freq >> 0) & 0xff) * 100,
			   ((ia_freq >> 8) & 0xff) * 100);
	}
	intel_runtime_pm_put(gt->uncore->rpm, wakeref);

	return 0;
}

static bool llc_eval(void *data)
{
	struct intel_gt *gt = data;

	return HAS_LLC(gt->i915);
}

DEFINE_INTEL_GT_DEBUGFS_ATTRIBUTE(llc);

static const char *rps_power_to_str(unsigned int power)
{
	static const char * const strings[] = {
		[LOW_POWER] = "low power",
		[BETWEEN] = "mixed",
		[HIGH_POWER] = "high power",
	};

	if (power >= ARRAY_SIZE(strings) || !strings[power])
		return "unknown";

	return strings[power];
}

static int rps_boost_show(struct seq_file *m, void *data)
{
	struct intel_gt *gt = m->private;
	struct drm_i915_private *i915 = gt->i915;
	struct intel_rps *rps = &gt->rps;

	seq_printf(m, "RPS enabled? %s\n",
		   str_yes_no(intel_rps_is_enabled(rps)));
	seq_printf(m, "RPS active? %s\n",
		   str_yes_no(intel_rps_is_active(rps)));
	seq_printf(m, "GPU busy? %s, %llums\n",
		   str_yes_no(gt->awake),
		   ktime_to_ms(intel_gt_get_awake_time(gt)));
	seq_printf(m, "Boosts outstanding? %d\n",
		   atomic_read(&rps->num_waiters));
	seq_printf(m, "Interactive? %d\n", READ_ONCE(rps->power.interactive));
	seq_printf(m, "Frequency requested %d, actual %d\n",
		   intel_gpu_freq(rps, rps->cur_freq),
		   intel_rps_read_actual_frequency(rps));
	seq_printf(m, "  min hard:%d, soft:%d; max soft:%d, hard:%d\n",
		   intel_gpu_freq(rps, rps->min_freq),
		   intel_gpu_freq(rps, rps->min_freq_softlimit),
		   intel_gpu_freq(rps, rps->max_freq_softlimit),
		   intel_gpu_freq(rps, rps->max_freq));
	seq_printf(m, "  idle:%d, efficient:%d, boost:%d\n",
		   intel_gpu_freq(rps, rps->idle_freq),
		   intel_gpu_freq(rps, rps->efficient_freq),
		   intel_gpu_freq(rps, rps->boost_freq));

	seq_printf(m, "Wait boosts: %d\n", READ_ONCE(rps->boosts));

	if (GRAPHICS_VER(i915) >= 6 && intel_rps_is_active(rps)) {
		struct intel_uncore *uncore = gt->uncore;
		u32 rpup, rpupei;
		u32 rpdown, rpdownei;

		intel_uncore_forcewake_get(uncore, FORCEWAKE_ALL);
		rpup = intel_uncore_read_fw(uncore, GEN6_RP_CUR_UP) & GEN6_RP_EI_MASK;
		rpupei = intel_uncore_read_fw(uncore, GEN6_RP_CUR_UP_EI) & GEN6_RP_EI_MASK;
		rpdown = intel_uncore_read_fw(uncore, GEN6_RP_CUR_DOWN) & GEN6_RP_EI_MASK;
		rpdownei = intel_uncore_read_fw(uncore, GEN6_RP_CUR_DOWN_EI) & GEN6_RP_EI_MASK;
		intel_uncore_forcewake_put(uncore, FORCEWAKE_ALL);

		seq_printf(m, "\nRPS Autotuning (current \"%s\" window):\n",
			   rps_power_to_str(rps->power.mode));
		seq_printf(m, "  Avg. up: %d%% [above threshold? %d%%]\n",
			   rpup && rpupei ? 100 * rpup / rpupei : 0,
			   rps->power.up_threshold);
		seq_printf(m, "  Avg. down: %d%% [below threshold? %d%%]\n",
			   rpdown && rpdownei ? 100 * rpdown / rpdownei : 0,
			   rps->power.down_threshold);
	} else {
		seq_puts(m, "\nRPS Autotuning inactive\n");
	}

	return 0;
}

DEFINE_INTEL_GT_DEBUGFS_ATTRIBUTE(rps_boost);

static int perf_limit_reasons_get(void *data, u64 *val)
{
	struct intel_gt *gt = data;
	intel_wakeref_t wakeref;

	with_intel_runtime_pm(gt->uncore->rpm, wakeref)
		*val = intel_uncore_read(gt->uncore, intel_gt_perf_limit_reasons_reg(gt));

	return 0;
}

static int perf_limit_reasons_clear(void *data, u64 val)
{
	struct intel_gt *gt = data;
	intel_wakeref_t wakeref;

	/* Clears the upper 16 bit ie log bits, as lower 16 bit ie status bits are read-only */
	with_intel_runtime_pm(gt->uncore->rpm, wakeref)
		intel_uncore_rmw(gt->uncore, intel_gt_perf_limit_reasons_reg(gt),
				 GT0_PERF_LIMIT_REASONS_LOG_MASK, 0);

	return 0;
}

DEFINE_I915_GT_SIMPLE_ATTRIBUTE(perf_limit_reasons_fops, perf_limit_reasons_get,
				perf_limit_reasons_clear, "%llu\n");

void intel_gt_pm_debugfs_register(struct intel_gt *gt, struct dentry *root)
{
	static const struct intel_gt_debugfs_file files[] = {
		{ "drpc", &drpc_fops, NULL },
		{ "frequency", &frequency_fops, NULL },
		{ "forcewake", &fw_domains_fops, NULL },
		{ "forcewake_user", &forcewake_user_fops, NULL},
		{ "llc", &llc_fops, llc_eval },
		{ "rps_boost", &rps_boost_fops },
		{ "perf_limit_reasons", &perf_limit_reasons_fops, NULL },
	};

	if (IS_SRIOV_VF(gt->i915))
		return;

	intel_gt_debugfs_register_files(root, files, ARRAY_SIZE(files), gt);

	debugfs_create_file("gt_c6_residency", 0444, root,
			    gt, &gt_c6_residency_fops);
}
