// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

#include <linux/string_helpers.h>

#include "i915_drv.h"
#include "i915_reg.h"
#include "intel_dram.h"
#include "intel_mchbar_regs.h"
#include "intel_pcode.h"

struct dram_dimm_info {
	u16 size;
	u8 width, ranks;
};

struct dram_channel_info {
	struct dram_dimm_info dimm_l, dimm_s;
	u8 ranks;
	bool is_16gb_dimm;
};

static int icl_pcode_read_mem_global_info(struct drm_i915_private *dev_priv)
{
	struct dram_info *dram_info = &dev_priv->dram_info;
	u32 val = 0;
	int ret;

	ret = snb_pcode_read(&dev_priv->uncore, ICL_PCODE_MEM_SUBSYSYSTEM_INFO |
			     ICL_PCODE_MEM_SS_READ_GLOBAL_INFO, &val, NULL);
	if (ret)
		return ret;

	switch (val & 0xf) {
	case 0:
		dram_info->type = INTEL_DRAM_DDR4;
		break;
	case 1:
		dram_info->type = INTEL_DRAM_DDR5;
		break;
	case 2:
		dram_info->type = INTEL_DRAM_LPDDR5;
		break;
	case 3:
		dram_info->type = INTEL_DRAM_LPDDR4;
		break;
	case 4:
		dram_info->type = INTEL_DRAM_DDR3;
		break;
	case 5:
		dram_info->type = INTEL_DRAM_LPDDR3;
		break;
	default:
		MISSING_CASE(val & 0xf);
		return -EINVAL;
	}

	dram_info->num_channels = (val & 0xf0) >> 4;
	dram_info->num_qgv_points = (val & 0xf00) >> 8;
	dram_info->num_psf_gv_points = (val & 0x3000) >> 12;

	return 0;
}

static int gen12_get_dram_info(struct drm_i915_private *i915)
{
	i915->dram_info.wm_lv_0_adjust_needed = false;

	return icl_pcode_read_mem_global_info(i915);
}

static int xelpdp_get_dram_info(struct drm_i915_private *i915)
{
	u32 val = intel_uncore_read(&i915->uncore, MTL_MEM_SS_INFO_GLOBAL);
	struct dram_info *dram_info = &i915->dram_info;

	u32 ddr_type = REG_FIELD_GET(MTL_DDR_TYPE_MASK, val);

	switch (ddr_type) {
	case 0:
		dram_info->type = INTEL_DRAM_DDR4;
		break;
	case 1:
		dram_info->type = INTEL_DRAM_DDR5;
		break;
	case 2:
		dram_info->type = INTEL_DRAM_LPDDR5;
		break;
	case 3:
		dram_info->type = INTEL_DRAM_LPDDR4;
		break;
	case 4:
		dram_info->type = INTEL_DRAM_DDR3;
		break;
	case 5:
		dram_info->type = INTEL_DRAM_LPDDR3;
		break;
	default:
		MISSING_CASE(ddr_type);
		return -EINVAL;
	}

	dram_info->num_channels = REG_FIELD_GET(MTL_N_OF_POPULATED_CH_MASK, val);
	dram_info->num_qgv_points = REG_FIELD_GET(MTL_N_OF_ENABLED_QGV_POINTS_MASK, val);
	/* PSF GV points not supported in D14+ */

	return 0;
}

void intel_dram_detect(struct drm_i915_private *i915)
{
	struct dram_info *dram_info = &i915->dram_info;
	int ret;

	if (IS_DG2(i915) || !HAS_DISPLAY(i915))
		return;

	/*
	 * Assume level 0 watermark latency adjustment is needed until proven
	 * otherwise, this w/a is not needed by bxt/glk.
	 */
	dram_info->wm_lv_0_adjust_needed = !IS_GEN9_LP(i915);

	if (DISPLAY_VER(i915) >= 14)
		ret = xelpdp_get_dram_info(i915);
	else
		ret = gen12_get_dram_info(i915);
	if (ret)
		return;

	drm_dbg_kms(&i915->drm, "DRAM channels: %u\n", dram_info->num_channels);

	drm_dbg_kms(&i915->drm, "Watermark level 0 adjustment needed: %s\n",
		    str_yes_no(dram_info->wm_lv_0_adjust_needed));
}
