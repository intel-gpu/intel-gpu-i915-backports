// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include "gt/intel_gt.h"
#include "gt/intel_gt_print.h"

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

struct hbm_status {
	u8 hbm_training_failed:1;
	u8 diag_run:1;
	u8 diag_incomplete:1;
	u8 hbm_existing_fault:1;
	u8 hbm_new_fault:1;
	u8 hbm_repair_attempted:1;
	u8 hbm_repair_exhausted:1;
	u8 hbm_val_failure:1;
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
	struct hbm_status hbm_info = {false};
	u32 channel_num, hbm_num;
	const char *name;
	unsigned long errsrc;
	u32 errbit, reg_id;
	u64 reg64_value;
	u32 reg32_value;
	u32 hbm_chnl_id;
	int ret, id;
	u32 num_regs;

	bool hbm_error = false;

	ret = 0;

	if (!IS_PONTEVECCHIO(i915) || IS_SRIOV_VF(i915))
		return ret;

	errsrc = __raw_uncore_read32(gt->uncore, GT0_TELEMETRY_MSGREGADDR);
	if (errsrc)
		gt_dbg(gt, "Read value of GT0_TELEMETRY_MSGREGADDR=[0x%08lx]\n", errsrc);

	for_each_set_bit(errbit, &errsrc, 32) {
		name = NULL;

		switch (errbit) {
		case PCIE_DEKEL_FW_LOAD_FAILED:
			name = "PCIe link downgraded to 1.0";
			break;
		case FSP2_HBM_TRAINING_FAILED:
			name = "HBM training failed";
			hbm_info.hbm_training_failed = true;
			ret = -ENXIO;
			break;
		case FSP2_PUNIT_INIT_FAILED:
			name = "punit init failed";
			ret = -ENXIO;
			break;
		case FSP2_GT_INIT_FAILED:
			name = "GT init failed";
			ret = -ENXIO;
			break;
		case HBM_DIAGNOSTICS_RUN:
			hbm_info.diag_run = true;
			break;
		case MRC_TEST_STATUS:
			name = "memory wipe encountered failure";
			ret = -ENXIO;
			break;
		case HBMIO_UC_STATUS:
			name = "HBMIO uC Failure";
			ret = -ENXIO;
			break;
		case ALL_HBMS_DISABLED_TILE0:
			name = "Tile0 HBM Disabled";
			break;
		case ALL_HBMS_DISABLED_TILE1:
			name = "Tile1 HBM Disabled";
			break;
		case FSP2_SUCCESSFUL:
			/* not an error, signifies FSP went past stage2*/
			break;
		case HBM_DIAGNOSTICS_INCOMPLETE:
			hbm_info.diag_incomplete = true;
			break;
		case HBM_IDENTIFIED_EXISTING_FAULT:
			hbm_info.hbm_existing_fault = true;
			break;
		case HBM_IDENTIFIED_NEW_FAULT:
			hbm_info.hbm_new_fault = true;
			break;
		case HBM_NEW_REPAIR_ATTEMPTED:
			hbm_info.hbm_repair_attempted = true;
			break;
		case HBM_REPAIR_SPARE_EXHAUSTED:
			hbm_info.hbm_repair_exhausted = true;
			break;
		case HBM_VAL_FAILURE:
			hbm_info.hbm_val_failure = true;
			break;
		default:
			name = "unknown failure";
			break;
		}
		if (name)
			gt_err(gt, "%s\n", name);
	}

	if (hbm_info.diag_run) {
		if (hbm_info.diag_incomplete) {
			gt_err(gt, "diagnostics is incomplete, HBM is un-reliable");
		} else if (hbm_info.hbm_repair_attempted) {
			if (hbm_info.hbm_val_failure || hbm_info.hbm_repair_exhausted) {
				gt_err(gt, "unrepairable HBM fault present\n");
				/* set hbm state to replace */
				gt->mem_sparing.health_status = MEM_HEALTH_REPLACE;
			} else if (hbm_info.hbm_existing_fault && hbm_info.hbm_new_fault) {
				gt_dbg(gt, "existing and new HBM faults present and repaired\n");
			} else if (hbm_info.hbm_existing_fault) {
				gt_dbg(gt, "repaired HBM fault present\n");
			} else if (hbm_info.hbm_new_fault) {
				gt_dbg(gt, "new HBM fault present and repaired\n");
			}
		} else {
			if (hbm_info.hbm_existing_fault & hbm_info.hbm_new_fault)
				gt_err(gt, "repaired and new HBM fault present, recommended to run diagnostics and repair\n");
			else if (hbm_info.hbm_existing_fault)
				gt_dbg(gt, "repaired HBM fault present\n");
			else if (hbm_info.hbm_new_fault)
				gt_err(gt, "new / unrepaired HBM fault present, recommended to run diagnostics and repair\n");
			else
				gt_dbg(gt, "Diagnostics completed no faults found\n");
		}
	}

	if (!(errsrc & REG_BIT(FSP2_SUCCESSFUL))) {
		__i915_printk(i915, KERN_CRIT, "FSP stage 2 not completed!\n");
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

		gt_dbg(gt, "FUSE3_HBM_STACK_STATUS=[0x%08lx]\n", hbm_mask);

		if (hbm_info.hbm_training_failed) {
			for_each_set_bit(hbm_num, &hbm_mask, HBM_STACK_MAX) {
				u32 ctrl_reg = __raw_uncore_read32(gt->uncore,
								   PVC_UC_BIOS_MAILBOX_CTL_REG(hbm_num));
				u32 hbm_training_status = FIELD_GET(HBM_TRAINING_INFO, ctrl_reg);

				gt_info(gt, "uc_bios_mailbox_ctrl_creg[%d] = 0x%08x\n",
					 hbm_num, ctrl_reg);

				if (hbm_training_status == HBM_TRAINING_FAILED) {
					u32 data0_reg = __raw_uncore_read32(gt->uncore,
									    PVC_UC_BIOS_MAILBOX_DATA0_REG_HBM(hbm_num));
					u32 data1_reg = __raw_uncore_read32(gt->uncore,
									    PVC_UC_BIOS_MAILBOX_DATA1_REG_HBM(hbm_num));
					gt_err(gt,
					       "Reported HBM training error on HBM%d."
					       " uc_bios_mailbox_data0_creg = 0x%08x, uc_bios_mailbox_data1_creg = 0x%08x\n",
					       hbm_num, data0_reg, data1_reg);
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
						gt_err(gt, "Register %s read value=[0x%016llx], expected value=[0x%016x]. Reported error on HBM%d:CHANNEL%d\n",
						       reg64_info->reg_name,
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
						gt_err(gt, "Register %s read value=[0x%08x], expected value=[0x%08x]. Reported error on HBM%d:CHANNEL%d\n",
						       reg32_info->reg_name, reg32_value, reg32_info->default_value,
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
