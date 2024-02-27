// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */
#include "gt/intel_gt.h"

#include "i915_drv.h"
#include "pvc_ras.h"

struct ras_reg64_info {
	const char * const reg_name;
	const i915_reg_t offset;
};

struct ras_reg32_info {
	const char * const reg_name;
	const i915_reg_t offset;
	const u32  default_value;
};

static const struct ras_reg64_info pvc_memory_cntrlr_reg64[] = {
	{"INTERNAL_ERROR_2LMISCC",		_MMIO(0x286f70)},
	{"INTERNAL_ERROR_SCHEDSPQ",		_MMIO(0x287d80)},
	{"INTERNAL_ERROR_SCHEDSBS",		_MMIO(0x287a70)},
	{"INTERNAL_ERROR_DP in Pchnl0",		_MMIO(0x288a00)},
	{"IMC0_MC_STATUS_SHADOW in Pchnl0",	_MMIO(0x287030)},
	{"IMC0_MC8_ADDR_SHADOW in Pchnl0",	_MMIO(0x286ed0)},
	{"IMC0_MC_MISC_SHADOW in Pchnl0",	_MMIO(0x287040)},
	{"INTERNAL_ERROR_DP in Pchnl1",		_MMIO(0x288e00)},
	{"IMC0_MC_STATUS_SHADOW in Pchnl1",	_MMIO(0x287430)},
	{"IMC0_MC8_ADDR_SHADOW_DP1 in Pchnl1",	_MMIO(0x286fa0)},
	{"IMC0_MC_MISC_SHADOW in Pchnl1",	_MMIO(0x287440)},
};

static const struct ras_reg32_info pvc_memory_cntrlr_reg32[] = {
	{"CPGC_SEQ_STATUS",			_MMIO(0x0028a11c),		0x90030000},
	{"CPGC_ERR_TEST_ERR_STAT in Pchnl0",	_MMIO(0x0028a2cc),		0x3000000},
	{"CPGC_ERR_TEST_ERR_STAT in Pchnl1",	_MMIO(0x0028a6cc),		0x1000000},
};

int pvc_ras_telemetry_probe(struct drm_i915_private *i915)
{
	struct intel_gt *gt = to_gt(i915);

	const struct ras_reg64_info *reg64_info;
	const struct ras_reg32_info *reg32_info;
	u32 channel_num, hbm_num;
	unsigned long errsrc;
	u32 errbit, reg_id;
	u64 reg64_value;
	u32 reg32_value;
	const char *name;
	u32 hbm_chnl_id;
	int ret, id;
	u32 num_regs;

	bool hbm_error = false;
	bool is_fsp2_hbm_training_failure = false;

	ret = 0;

	if (!IS_PONTEVECCHIO(i915) || IS_SRIOV_VF(i915))
		return ret;

	errsrc = __raw_uncore_read32(gt->uncore, GT0_TELEMETRY_MSGREGADDR);

	if (!(errsrc & REG_BIT(FSP2_SUCCESSFUL))) {
		drm_dbg(&i915->drm, "Read value of GT0_TELEMETRY_MSGREGADDR=[0x%08lx]\n", errsrc);
		for_each_set_bit(errbit, &errsrc, 32) {
			switch (errbit) {
			case RESETOUT_N_TIMED_OUT:
				name = "resetout and timed out";
				break;
			case SOC_BOOT_IN_PROGRESS:
				name = "soc boot in progress";
				break;
			case PECI_IP_NOT_RESPONDING:
				name = "peci ip not responding";
				break;
			case CSC_FWSTS_REGISTERS_NOT_UPDATED:
				name = "csc fwst registers not updated";
				break;
			case ALERT_N_TIMED_OUT:
				name = "alert and timed out";
				break;
			case CSC_FW_LOAD_FAILED:
				name = "csc fw load failed";
				break;
			case FSP2_HBM_TRAINING_FAILED:
				name = "fsp2 hbm training failed";
				is_fsp2_hbm_training_failure = true;
				break;
			case FSP2_PUNIT_INIT_FAILED:
				name = "fsp2 punit init failed";
				break;
			case FSP2_GT_INIT_FAILED:
				name = "fsp2 gt init failed";
				break;
			case SOC_INIT_FAILED:
				name = "soc init failed";
				break;
			default:
				name = "unknown failure";
				break;
			}
			drm_err(&i915->drm, "%s reported by GT0_TELEMETRY_MSGREGADDR\n", name);
		}

		__i915_printk(i915, KERN_CRIT, "HW training failed; try a cold reboot.\n");
		ret = -ENXIO;
	}

	for_each_gt(gt, i915, id) {
		/* Memory controller register checks for
		 * status of HBM0 to HBM3 and channel0 to channel7
		 * Same set of memory controller registers are used
		 * for different HBM channels and write value
		 * to MMIO_INDX_REG selects HBM and Channel.
		 * 0x0 ... 0x7 for HBM0-channel0 ... HBM0-channel7.
		 * 0x8 ... 0xf for HBM1-Channel0 ... HBM1-channel7.
		 * 0x10 ... 0x17 for HBM2-Channel0 ... HBM2-channel7.
		 * 0x18 ... 0x1f for HBM3-Channel0 ... HBM3-channel7.
		 */

		unsigned long hbm_mask = __raw_uncore_read32(gt->uncore, FUSE3_HBM_STACK_STATUS);

		drm_dbg(&i915->drm, "GT%d: FUSE3_HBM_STACK_STATUS=[0x%08lx]\n",
			gt->info.id, hbm_mask);

		if (is_fsp2_hbm_training_failure) {
			for_each_set_bit(hbm_num, &hbm_mask, HBM_STACK_MAX) {
				u32 ctrl_reg = __raw_uncore_read32(gt->uncore,
								   PVC_UC_BIOS_MAILBOX_CTL_REG(hbm_num));
				u32 hbm_training_status = FIELD_GET(HBM_TRAINING_INFO, ctrl_reg);

				drm_info(&i915->drm, "GT%d: uc_bios_mailbox_ctrl_creg[%d] = 0x%08x\n",
					 gt->info.id, hbm_num, ctrl_reg);

				if (hbm_training_status == HBM_TRAINING_FAILED) {
					u32 data0_reg = __raw_uncore_read32(gt->uncore,
									    PVC_UC_BIOS_MAILBOX_DATA0_REG_HBM(hbm_num));
					u32 data1_reg = __raw_uncore_read32(gt->uncore,
									    PVC_UC_BIOS_MAILBOX_DATA1_REG_HBM(hbm_num));
					drm_err(&i915->drm,
						"GT%d: Reported HBM training error on HBM%d."
						" uc_bios_mailbox_data0_creg = 0x%08x, uc_bios_mailbox_data1_creg = 0x%08x\n",
						gt->info.id, hbm_num, data0_reg, data1_reg);
				}
			}
		}

		for_each_set_bit(hbm_num, &hbm_mask, HBM_STACK_MAX) {
			for (channel_num = 0; channel_num < CHANNEL_MAX; channel_num++) {
				hbm_chnl_id = (CHANNEL_MAX * hbm_num) + channel_num;
				__raw_uncore_write32(gt->uncore, MMIO_INDX_REG, hbm_chnl_id);
				num_regs = ARRAY_SIZE(pvc_memory_cntrlr_reg64);

				for (reg_id = 0; reg_id < num_regs; reg_id++) {
					reg64_info = &pvc_memory_cntrlr_reg64[reg_id];
					reg64_value = __raw_uncore_read64(gt->uncore,
									  reg64_info->offset);

					if (reg64_value != DEFAULT_VALUE_RAS_REG64) {
						drm_err(&i915->drm, "GT%d:Register %s read value=[0x%016llx], expected value=[0x%016x]. Reported error on HBM%d:CHANNEL%d\n",
							gt->info.id, reg64_info->reg_name,
							reg64_value, DEFAULT_VALUE_RAS_REG64,
							hbm_num, channel_num);

						hbm_error = true;
						ret = -ENXIO;
					}
				}

				num_regs = ARRAY_SIZE(pvc_memory_cntrlr_reg32);
				for (reg_id = 0; reg_id < num_regs; reg_id++) {
					reg32_info = &pvc_memory_cntrlr_reg32[reg_id];
					reg32_value = __raw_uncore_read32(gt->uncore,
									  reg32_info->offset);

					if (reg32_value != reg32_info->default_value) {
						drm_err(&i915->drm, "GT%d:Register %s read value=[0x%08x], expected value=[0x%08x]. Reported error on HBM%d:CHANNEL%d\n",
							gt->info.id, reg32_info->reg_name,
							reg32_value, reg32_info->default_value,
							hbm_num, channel_num);

						hbm_error = true;
						ret = -ENXIO;
					}
				}
			}
		}
	}
	if (hbm_error)
		__i915_printk(i915, KERN_CRIT,
			      "HBM is in an unreliable state; try a cold reboot.\n");

	return ret;
}
