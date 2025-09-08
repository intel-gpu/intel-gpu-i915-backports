// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include "pvc_fatal_error_dump.h"
#include "intel_uncore.h"
#include "gt/intel_gt.h"

static struct pci_dev *get_slot2(struct pci_dev *dev)
{
	struct pci_dev *devices;

	list_for_each_entry(devices, &dev->bus->devices, bus_list) {
		if (PCI_SLOT(devices->devfn) == 2)
			return devices;
	}

	return NULL;
}

static struct pci_dev *get_vsp_dev(struct pci_dev *dev)
{
	return pci_upstream_bridge(dev);
}

static struct pci_dev *get_oob_msm_devices(struct pci_dev *pdev, struct pci_dev **c1,
					    struct pci_dev **c2)
{
	struct pci_dev *vsp, *oob_msm_dev, *dev;
	struct pci_bus *child;
	u16 sriov_ctrl, target_msm_bus;

	/* MSM device depends on SRIOV CTRL register */
	pci_read_config_word(pdev, PVC_SRIOV_CTRL_CONFIG, &sriov_ctrl);
	if (sriov_ctrl & 0x1)
		target_msm_bus = pdev->bus->number + 2;
	else
		target_msm_bus = pdev->bus->number + 1;

	vsp = get_vsp_dev(pdev);
	list_for_each_entry(child, &vsp->bus->children, node) {
		if (child->number == target_msm_bus) {
			list_for_each_entry(dev, &child->devices, bus_list) {
				switch (PCI_FUNC(dev->devfn)) {
				case 0:
					oob_msm_dev = dev;
					break;
				case 1:
					*c1 = dev;
					break;
				case 2:
					*c2 = dev;
					break;
				}
			}

			return oob_msm_dev;
		}
	}

	return NULL;
}

void pvc_fatal_error_dump_msm_pci_config(struct drm_i915_private *i915, struct pci_dev *msm,
					 struct pci_dev *c1, struct pci_dev *c2)
{
	u32 num_regs, i, val32;
	u16 val16;

	pci_read_config_word(msm, msm_cfg[0].offset, &val16);
	PVC_FATAL_ERR_PCI_DEBUG(i915, msm_cfg[0].offset, val16);

	pci_read_config_word(c1, msm_cfg[0].offset, &val16);
	PVC_FATAL_ERR_PCI_DEBUG(i915, msm_cfg[0].offset, val16);

	pci_read_config_word(c2, msm_cfg[0].offset, &val16);
	PVC_FATAL_ERR_PCI_DEBUG(i915, msm_cfg[0].offset, val16);

	num_regs = ARRAY_SIZE(msm_offsets);
	for (i = 0; i < num_regs; i++) {
		if (msm_offsets[i].size == 16) {
			pci_read_config_word(msm, msm_offsets[i].offset, &val16);
			PVC_FATAL_ERR_PCI_DEBUG(i915, msm_offsets[i].offset, val16);
		} else {
			pci_read_config_dword(msm, msm_offsets[i].offset, &val32);
			PVC_FATAL_ERR_PCI_DEBUG(i915, msm_offsets[i].offset, val32);
		}
	}
}

void pvc_fatal_error_dump_vsp_pci_config(struct drm_i915_private *i915, struct pci_dev *vsp)
{
	u32 num_regs, i, val32;
	u16 val16;

	num_regs = ARRAY_SIZE(vsp_offsets);
	for (i = 0; i < num_regs; i++) {
		if (vsp_offsets[i].size == 16) {
			pci_read_config_word(vsp, vsp_offsets[i].offset, &val16);
			PVC_FATAL_ERR_PCI_DEBUG(i915, vsp_offsets[i].offset, val16);
		} else {
			pci_read_config_dword(vsp, vsp_offsets[i].offset, &val32);
			PVC_FATAL_ERR_PCI_DEBUG(i915, vsp_offsets[i].offset, val32);
		}
	}
}

void pvc_fatal_error_dump_usp_pci_config(struct drm_i915_private *i915, struct pci_dev *usp)
{
	u32 num_regs, i, val32;
	u16 val16;

	num_regs = ARRAY_SIZE(usp_offsets);
	for (i = 0; i < num_regs; i++) {
		if (usp_offsets[i].size == 16) {
			pci_read_config_word(usp, usp_offsets[i].offset, &val16);
			PVC_FATAL_ERR_PCI_DEBUG(i915, usp_offsets[i].offset, val16);
		} else {
			pci_read_config_dword(usp, usp_offsets[i].offset, &val32);
			PVC_FATAL_ERR_PCI_DEBUG(i915, usp_offsets[i].offset, val32);
		}
	}
}

void pvc_fatal_error_dump_pci(struct intel_gt *gt, struct pci_dev *usp)
{
	struct drm_i915_private *i915 = gt->i915;
	struct pci_dev *pdev = to_pci_dev(gt->i915->drm.dev);
	struct pci_dev *vsp, *vsp1, *oob_msm, *oob_msm_c1, *oob_msm_c2;

	if (gt->info.id == 0) {
		vsp = get_vsp_dev(pdev);
		if (!usp || !vsp) {
			intel_gt_log_driver_error(gt, INTEL_GT_DRIVER_ERROR_GT_OTHER,
						  "USP or VSP device not found\n");
			return;
		}
		vsp1 = get_slot2(vsp);
		oob_msm = get_oob_msm_devices(pdev, &oob_msm_c1, &oob_msm_c2);
		if (!vsp1 || !oob_msm || !oob_msm_c1 || !oob_msm_c2) {
			intel_gt_log_driver_error(gt, INTEL_GT_DRIVER_ERROR_GT_OTHER,
						  "VSP1 or OOB_MSM devices not found\n");
			return;
		}

		pvc_fatal_error_dump_usp_pci_config(i915, usp);
		pvc_fatal_error_dump_vsp_pci_config(i915, vsp);
		pvc_fatal_error_dump_vsp_pci_config(i915, vsp1);
		pvc_fatal_error_dump_msm_pci_config(i915, oob_msm, oob_msm_c1, oob_msm_c2);
	}
	dev_info(gt->i915->drm.dev, "T%d_MMIO_END", gt->info.id);
}

static u32 mmio_reg_count(struct intel_gt *gt)
{
	u32 total_num_regs = 0;

	total_num_regs = ARRAY_SIZE(uncore_ieh_offsets) + ARRAY_SIZE(uncore_ieh_offsets)
			 + ARRAY_SIZE(uncore_punit_offsets) + ARRAY_SIZE(uncore_mdfi_offsets)
			 + ARRAY_SIZE(thermal_regs) + ARRAY_SIZE(sgunit_regs_1)
			 + ERR_STAT_GT_FATAL_VCTR_LEN + ERR_STAT_GT_COR_VCTR_NUM_REGS
			 + ARRAY_SIZE(sgunit_regs_2) + ARRAY_SIZE(intr_regs)
			 + ARRAY_SIZE(mert_regs);

	if (gt->info.id == 0)
		total_num_regs += (_GSC_HEC_CORR_ERR_STATUS - _GSC_HEC_UNCORR_ERR_STATUS) / 4
				  + ARRAY_SIZE(usp_offsets) + (2 * ARRAY_SIZE(vsp_offsets))
				  + ARRAY_SIZE(msm_offsets) + 3;

	return total_num_regs;
}

void pvc_fatal_error_dump_soc_regs(struct intel_gt *gt)
{
	struct pci_dev *pdev = to_pci_dev(gt->i915->drm.dev);
	void __iomem * const regs = gt->uncore->regs;
	u32 offset, num_regs, val, i;

	PVC_DUMP_HEADER(gt, gt->info.id, pdev->bus->number, PCI_SLOT(pdev->devfn),
			PCI_FUNC(pdev->devfn));

	dev_info(gt->i915->drm.dev, "T%d_MMIO register count = %u", gt->info.id,
				     mmio_reg_count(gt));
	dev_info(gt->i915->drm.dev, "T%d_MMIO_BEGIN", gt->info.id);

	/* MMIO IEH MASTER regs */
	num_regs = ARRAY_SIZE(uncore_ieh_offsets);
	for (i = 0; i < num_regs; i++) {
		offset = SOC_PVC_BASE + uncore_ieh_offsets[i];
		val = raw_reg_read(regs, _MMIO(offset));
		PVC_FATAL_ERR_DEBUG(gt, offset, "Mmio", val);
	}
	/* MMIO IEH SLAVE regs */
	num_regs = ARRAY_SIZE(uncore_ieh_offsets);
	for (i = 0; i < num_regs; i++) {
		offset = SOC_PVC_SLAVE_BASE + uncore_ieh_offsets[i];
		val = raw_reg_read(regs, _MMIO(offset));
		PVC_FATAL_ERR_DEBUG(gt, offset, "Mmio", val);
	}
}

void pvc_fatal_error_dump_punit_regs(struct intel_gt *gt)
{
	void __iomem * const regs = gt->uncore->regs;
	u32 num_regs, val, i;

	num_regs = ARRAY_SIZE(uncore_punit_offsets);
	for (i = 0; i < num_regs; i++) {
		val = raw_reg_read(regs, uncore_punit_offsets[i].offset);
		PVC_FATAL_ERR_DEBUG(gt, uncore_punit_offsets[i].offset.reg, "Mmio", val);
	}
}

void pvc_fatal_error_dump_mdfi_regs(struct intel_gt *gt)
{
	void __iomem * const regs = gt->uncore->regs;
	u32 num_regs, val, i;

	num_regs = ARRAY_SIZE(uncore_mdfi_offsets);
	for (i = 0; i < num_regs; i++) {
		val = raw_reg_read(regs, uncore_mdfi_offsets[i].offset);
		PVC_FATAL_ERR_DEBUG(gt,	uncore_mdfi_offsets[i].offset.reg, "Mmio", val);
	}
}

void pvc_fatal_error_dump_thermal_regs(struct intel_gt *gt)
{
	void __iomem * const regs = gt->uncore->regs;
	u32 num_regs, offset, val, i;

	num_regs = ARRAY_SIZE(thermal_regs);
	for (i = 0; i < num_regs; i++) {
		offset = thermal_regs[i].offset.reg;
		val = raw_reg_read(regs, thermal_regs[i].offset);
		PVC_FATAL_ERR_DEBUG(gt, offset, "Mmio", val);
	}
}

void pvc_fatal_error_dump_sgunit_regs(struct intel_gt *gt)
{
	void __iomem * const regs = gt->uncore->regs;
	u32 num_regs, offset, val, i;

	num_regs = ARRAY_SIZE(sgunit_regs_1);
	for (i = 0; i < num_regs; i++) {
		offset = sgunit_regs_1[i].offset.reg;
		val = raw_reg_read(regs, sgunit_regs_1[i].offset);
		PVC_FATAL_ERR_DEBUG(gt, offset, "Mmio", val);
	}

	for (i = 0; i < ERR_STAT_GT_FATAL_VCTR_LEN; i++) {
		offset = _ERR_STAT_GT_FATAL_VCTR_0 + (i * sizeof(u32));
		val = raw_reg_read(regs, _MMIO(offset));
		PVC_FATAL_ERR_DEBUG(gt, offset, "Mmio", val);
	}

	for (i = 0; i < ERR_STAT_GT_COR_VCTR_NUM_REGS; i++) {
		offset = _ERR_STAT_GT_COR_VCTR_0 + (i * sizeof(u32));
		val = raw_reg_read(regs, _MMIO(offset));
		PVC_FATAL_ERR_DEBUG(gt, offset, "Mmio", val);
	}

	num_regs = ARRAY_SIZE(sgunit_regs_2);
	for (i = 0; i < num_regs; i++) {
		offset = sgunit_regs_2[i].offset.reg;
		val = raw_reg_read(regs, sgunit_regs_2[i].offset);
		PVC_FATAL_ERR_DEBUG(gt, offset, "Mmio", val);
	}

	num_regs = ARRAY_SIZE(intr_regs);
	for (i = 0; i < num_regs; i++) {
		offset = intr_regs[i].offset.reg;
		val = raw_reg_read(regs, intr_regs[i].offset);
		PVC_FATAL_ERR_DEBUG(gt, offset, "Mmio", val);
	}

	num_regs = ARRAY_SIZE(mert_regs);
	for (i = 0; i < num_regs; i++) {
		offset = mert_regs[i].offset.reg;
		val = raw_reg_read(regs, mert_regs[i].offset);
		PVC_FATAL_ERR_DEBUG(gt, offset, "Mmio", val);
	}
}

void pvc_fatal_error_dump_heci_regs(struct intel_gt *gt)
{
	struct pci_dev *pdev = to_pci_dev(gt->i915->drm.dev);
	void __iomem * const regs = gt->uncore->regs;
	u32 base = PVC_GSC_HECI1_BASE;
	u32 offset, start, end;
	u16 val16;

	if (gt->info.id == 0) {
		start = _GSC_HEC_UNCORR_ERR_STATUS;
		end = _GSC_HEC_CORR_ERR_STATUS;
		for (offset = start; offset <= end; offset += 4) {
			PVC_FATAL_ERR_DEBUG(gt, base + offset, "Mmio",
					    raw_reg_read(regs, _MMIO(base + offset)));
		}

		pci_read_config_word(pdev, PVC_SGUNIT_DEVICESTS_PCI, &val16);
	}
}

void pvc_fatal_error_dump_gt_bc_counter(struct intel_gt *gt)
{
	struct pci_dev *pdev = to_pci_dev(gt->i915->drm.dev);
	void __iomem * const regs = gt->uncore->regs;
	u32 num_regs, offset, val, i;

	PVC_DUMP_HEADER(gt, gt->info.id, pdev->bus->number, PCI_SLOT(pdev->devfn),
			PCI_FUNC(pdev->devfn));

	dev_info(gt->i915->drm.dev, "T%d_GT_B_C register count = %lu", gt->info.id,
				     ARRAY_SIZE(gt_bc_counter));
	dev_info(gt->i915->drm.dev, "T%d_GT_B_C_BEGIN", gt->info.id);

	num_regs = ARRAY_SIZE(gt_bc_counter);
	for (i = 0; i < num_regs; i++) {
		offset = gt_bc_counter[i].offset.reg;
		val = raw_reg_read(regs, gt_bc_counter[i].offset);
		PVC_FATAL_ERR_DEBUG(gt, offset, "Mmio", val);
	}

	dev_info(gt->i915->drm.dev, "T%d_GT_B_C_END", gt->info.id);
}

void pvc_fatal_error_dump_slice_regs(struct intel_gt *gt)
{
	struct pci_dev *pdev = to_pci_dev(gt->i915->drm.dev);
	void __iomem * const regs = gt->uncore->regs;
	u32 num_regs, offset, val, i;

	PVC_DUMP_HEADER(gt, gt->info.id, pdev->bus->number, PCI_SLOT(pdev->devfn),
			PCI_FUNC(pdev->devfn));

	dev_info(gt->i915->drm.dev, "T%d_GT_REGS register count = %lu", gt->info.id,
				     ARRAY_SIZE(slice_regs));
	dev_info(gt->i915->drm.dev, "T%d_GT_BEGIN", gt->info.id);

	num_regs = ARRAY_SIZE(slice_regs);
	for (i = 0; i < num_regs; i++) {
		/* Write the steering value first */
		raw_reg_write(regs, slice_regs[i].steer_offset, slice_regs[i].steer_val);
		offset = slice_regs[i].offset.reg;
		val = raw_reg_read(regs, slice_regs[i].offset);
		PVC_FATAL_ERR_GT_DEBUG(gt, slice_regs[i].steer_offset.reg,
				       slice_regs[i].steer_val, val);
	}
	dev_info(gt->i915->drm.dev, "T%d_GT_END", gt->info.id);
}

void pvc_fatal_error_dump_hbm_regs(struct intel_gt *gt)
{
	struct pci_dev *pdev = to_pci_dev(gt->i915->drm.dev);
	void __iomem * const regs = gt->uncore->regs;
	u32 num_regs, i, chnl_id;
	u64 val, offset;

	PVC_DUMP_HEADER(gt, gt->info.id, pdev->bus->number, PCI_SLOT(pdev->devfn),
			PCI_FUNC(pdev->devfn));

	dev_info(gt->i915->drm.dev, "T%d_GPU_MC_REGS register count = %lu", gt->info.id,
				    ARRAY_SIZE(hbm_regs) * HBM_STACK_MAX * CHANNEL_MAX);
	dev_info(gt->i915->drm.dev, "T%d_MC_BEGIN", gt->info.id);

	num_regs = ARRAY_SIZE(hbm_regs);
	for (chnl_id = 0; chnl_id < HBM_STACK_MAX * CHANNEL_MAX; chnl_id++) {
		for (i = 0; i < num_regs; i++) {
			raw_reg_write(regs, MMIO_INDX_REG, chnl_id);
			offset = hbm_regs[i].offset.reg + (0x1000000 * gt->info.id);
			if (hbm_regs[i].size == 64)
				val = __raw_uncore_read64(gt->uncore, hbm_regs[i].offset);
			else
				val = __raw_uncore_read32(gt->uncore, hbm_regs[i].offset);

			PVC_FATAL_ERR_HBM_DEBUG(gt, chnl_id, offset, val);
		}
	}
	dev_info(gt->i915->drm.dev, "T%d_MC_END", gt->info.id);
}
