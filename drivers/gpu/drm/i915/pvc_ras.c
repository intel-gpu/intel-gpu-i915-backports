// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include "i915_drv.h"
#include "i915_sysfs.h"
#include "pvc_ras.h"

#include "gt/intel_gt.h"
#include "gt/intel_gt_print.h"

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
#define KOBJ_ATTR_RO(_name) \
	struct kobj_attribute dev_attr_##_name = __ATTR_RO(_name)
#endif

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

static void record_eye_margin(struct intel_gt *gt, const char *msg, int val)
{
	gt->i915->eye_margin.error |= val;
	gt->i915->eye_margin.status = __raw_uncore_read32(gt->uncore, SWF_ILK(2));
	gt_warn(gt, "%s MDFI eye margin detected, status %08x\n", msg, gt->i915->eye_margin.status);
}

int pvc_ras_telemetry_probe(struct drm_i915_private *i915)
{
	struct intel_gt *gt = to_gt(i915);

	struct hbm_status hbm_info = {};
	bool hbm_error = false;
	unsigned long errsrc;
	int ret = 0, id, bit;

	if (!IS_PONTEVECCHIO(i915) || IS_SRIOV_VF(i915))
		return 0;

	errsrc = __raw_uncore_read32(gt->uncore, GT0_TELEMETRY_MSGREGADDR);
	if (!errsrc)
		return 0;

	gt_dbg(gt, "GT0_TELEMETRY_MSGREGADDR = 0x%08lx\n", errsrc);
	for_each_set_bit(bit, &errsrc, 32) {
		const char *name = NULL;

		switch (bit) {
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
		case HBM_REPLACE:
			name = "HBM state transitioned to REPLACE";
			ret = -ENXIO;
			break;
		case MDFI_BAD_EYE_MARGIN_AFTER_TRAINING:
			record_eye_margin(gt, "Zero", 1);
			break;
		case MDFI_BAD_DLL_CODES_AFTER_TRAINING:
			record_eye_margin(gt, "Low", 2);
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
			gt_err(gt, "diagnostics is incomplete, HBM may be un-reliable\n");
		} else if (hbm_info.hbm_repair_attempted) {
				const char *msg;

				if (hbm_info.hbm_existing_fault && hbm_info.hbm_new_fault)
					msg = "existing and new HBM faults present";
				else if (hbm_info.hbm_existing_fault)
					msg = "existing HBM fault present";
				else if (hbm_info.hbm_new_fault)
					msg = "new HBM fault present";
				else
					msg = "no new or existing faults";

			if (hbm_info.hbm_val_failure || hbm_info.hbm_repair_exhausted) {
				gt->mem_sparing.health_status = MEM_HEALTH_REPLACE;
				gt_err(gt, "unrepairable HBM: %s\n", msg);
			} else {
				gt_notice(gt, "repaired HBM: %s\n", msg);
			}
		} else {
			if (hbm_info.hbm_new_fault)
				gt_err(gt, "new / unrepaired HBM fault present, recommended to run diagnostics and repair\n");
			else
				gt_info(gt, "Diagnostics completed no faults found\n");
		}
	}

	if (!(errsrc & REG_BIT(FSP2_SUCCESSFUL))) {
		__i915_printk(i915, KERN_CRIT, "FSP stage 2 not completed!\n");
		ret = -ENXIO;
	}

	for_each_gt(gt, i915, id) {
		/*
		 * Memory controller register checks for
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

		gt_dbg(gt, "FUSE3_HBM_STACK_STATUS = 0x%08lx\n", hbm_mask);

		if (hbm_info.hbm_training_failed) {
			for_each_set_bit(bit, &hbm_mask, HBM_STACK_MAX) {
				u32 ctrl_reg = __raw_uncore_read32(gt->uncore, PVC_UC_BIOS_MAILBOX_CTL_REG(bit));
				u32 hbm_training_status = FIELD_GET(HBM_TRAINING_INFO, ctrl_reg);

				gt_dbg(gt, "uc_bios_mailbox_ctrl_creg[%d] = 0x%08x\n", bit, ctrl_reg);

				if (hbm_training_status == HBM_TRAINING_FAILED) {
					gt_err(gt, "Reported HBM training error on HBM%d."
					       "uc_bios_mailbox_data0_creg = 0x%08x, uc_bios_mailbox_data1_creg = 0x%08x\n",
					       bit,
					       __raw_uncore_read32(gt->uncore, PVC_UC_BIOS_MAILBOX_DATA0_REG_HBM(bit)),
					       __raw_uncore_read32(gt->uncore, PVC_UC_BIOS_MAILBOX_DATA1_REG_HBM(bit)));
				}
			}
		}

		for_each_set_bit(bit, &hbm_mask, HBM_STACK_MAX) {
			u32 channel;

			for (channel = 0; channel < CHANNEL_MAX; channel++) {
				u32 hbm_chnl_id = (CHANNEL_MAX * bit) + channel;
				int num_regs, n;

				__raw_uncore_write32(gt->uncore, MMIO_INDX_REG, hbm_chnl_id);

				num_regs = ARRAY_SIZE(pvc_memory_cntrlr_reg64);
				for (n = 0; n < num_regs; n++) {
					const struct ras_reg64_info *r = &pvc_memory_cntrlr_reg64[n];
					u64 val;

					val = __raw_uncore_read64(gt->uncore, r->offset);
					if (val != DEFAULT_VALUE_RAS_REG64) {
						gt_err(gt, "Register %s read value=[0x%016llx], expected value=[0x%016x]. Reported error on HBM%d:CHANNEL%d\n",
						       r->reg_name, val, DEFAULT_VALUE_RAS_REG64,
						       bit, channel);

						hbm_error = true;
						ret = -ENXIO;
					}
				}

				num_regs = ARRAY_SIZE(pvc_memory_cntrlr_reg32);
				for (n = 0; n < num_regs; n++) {
					const struct ras_reg32_info *r = &pvc_memory_cntrlr_reg32[n];
					u32 val;

					val = __raw_uncore_read32(gt->uncore, r->offset);
					if (val != r->default_value) {
						gt_err(gt, "Register %s read value=[0x%08x], expected value=[0x%08x]. Reported error on HBM%d:CHANNEL%d\n",
						       r->reg_name, val,
						       r->default_value, bit,
						       channel);

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

static ssize_t
__mfdi_eye_margin_error_show(struct device *dev, char *buf)
{
	struct drm_i915_private *i915 = kdev_minor_to_i915(dev);

	/*
	 * mfdi_eye_margin_error:
	 * 0 - no errors
	 * 1 - zero eye margin
	 * 2 - low eye margin
	 * 3 - both low and zero eye margins
	 */
	return sysfs_emit(buf, "%d\n", i915->eye_margin.error);
}
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t
mfdi_eye_margin_error_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return __mfdi_eye_margin_error_show(kobj_to_dev(kobj), buf);
}
static KOBJ_ATTR_RO(mfdi_eye_margin_error);
#else
static ssize_t
mfdi_eye_margin_error_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return __mfdi_eye_margin_error_show(dev, buf);
}
static DEVICE_ATTR_RO(mfdi_eye_margin_error);
#endif

static ssize_t
__mfdi_eye_margin_status_show(struct device *dev, char *buf)
{
	struct drm_i915_private *i915 = kdev_minor_to_i915(dev);

	/*
	 * mfdi_eye_margin_status:
	 * union {
	 *	struct {
	 *		uint32_t eye_val    : 8; //  Hold the worst low eye margin encountered
	 *		uint32_t eye_source : 8; //  The source of the value, T2C = 0, T2T = 1, ANR = 2
	 *		uint32_t counter    : 8; //  How many low eye margin discovered
	 *		uint32_t eye_side   : 1; //  0 = left side, 1 = right side
	 *		uint32_t tile       : 1; //  Which Tile reported the worst value
	 *		uint32_t reserved   : 5;
	 *		uint32_t valid      : 1; // Indicate if the content of the register is valid
	 *	};
	 *	uint32_t val;
	 * };
	 */
	return sysfs_emit(buf, "%x\n", i915->eye_margin.status);
}
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t
mfdi_eye_margin_status_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return __mfdi_eye_margin_status_show(kobj_to_dev(kobj), buf);
}
static KOBJ_ATTR_RO(mfdi_eye_margin_status);
#else
static ssize_t
mfdi_eye_margin_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return __mfdi_eye_margin_status_show(dev, buf);
}
static DEVICE_ATTR_RO(mfdi_eye_margin_status);
#endif

void pvc_ras_register_sysfs(struct drm_i915_private *i915)
{
	static const struct attribute *files[] = {
		&dev_attr_mfdi_eye_margin_error.attr,
		&dev_attr_mfdi_eye_margin_status.attr,
		NULL
	};
	struct device *kdev = i915->drm.primary->kdev;

	if (!IS_PONTEVECCHIO(i915) || IS_SRIOV_VF(i915))
		return;

	if (sysfs_create_files(&kdev->kobj, files))
		dev_warn(i915->drm.dev, "Failed to install MDFI sysfs entries\n");
}
