/* SPDX-License-Identifier: MIT
 * Copyright © 2025 Intel Corporation
 */
#ifndef _PVC_FATAL_ERROR_DUMP_H
#define _PVC_FATAL_ERROR_DUMP_H

#include "i915_drv.h"
#include "i915_reg.h"
#include "i915_reg_defs.h"
#include "pvc_fatal_error_dump_reg.h"
#include "gt/intel_gt_regs.h"

struct intel_gt;

#define PVC_DUMP_HEADER(gt, s, b, d, f) \
			dev_info(gt->i915->drm.dev, \
			"Dumping registers on S: %d, B: %x, D: %d, F: %d", \
			s, b, d, f)

#define PVC_FATAL_ERR_DEBUG(gt, offset, type, val) \
			    dev_info(gt->i915->drm.dev, "0x%llx      %s       0x%x", \
			    (u64)(offset + (0x1000000 * gt->info.id)), type, val)

#define PVC_FATAL_ERR_PCI_DEBUG(i915, offset, val) \
			    dev_info(i915->drm.dev, "0x%04x        Pcie        0x%x", \
			    offset, val)

#define PVC_FATAL_ERR_GT_DEBUG(gt, steer_offset, steer_val, val) \
	dev_info(gt->i915->drm.dev, "0x%x   0x%x     0x%x", \
		 steer_offset, steer_val, val)

#define PVC_FATAL_ERR_HBM_DEBUG(gt, val1, val2, val3) \
	dev_info(gt->i915->drm.dev, "0x%x   0x%llx     0x%llx", \
		 val1, val2, val3)

struct mmio_reg32 {
	const i915_reg_t offset;
};

struct pci_info {
	const char * const reg_name;
	const u32 offset;
	const u32 size;
};

struct gt_slice_reg {
	const i915_reg_t steer_offset;
	u32 steer_val;
	const i915_reg_t offset;
};

struct hbm_reg_info {
	const char * const reg_name;
	const i915_reg_t offset;
	const u8 size;
};

static const u32 uncore_ieh_offsets[] = {_SOC_GCOERRSTS, _SOC_GCOFERRSTS, _SOC_GCONERRSTS,
					 _SOC_GNFERRSTS, _SOC_GNFFERRSTS, _SOC_GNFNERRSTS,
					 _SOC_GFAERRSTS, _SOC_GFAFERRSTS, _SOC_GFANERRSTS,
					 _SOC_LERRUNCSTS, _SOC_LFERRUNCSTS, _SOC_LNERRUNCSTS,
					 _SOC_LERRCORSTS, _SOC_LFERRCORSTS, _SOC_LNERRCORSTS};

static const struct pci_info usp_offsets[] = {{"secsts", 0x001e, 16}, {"devsts", 0x004a, 16},
					      {"linksts", 0x0052, 16}, {"erruncsts", 0x0104, 32},
					      {"errcorsts", 0x0110, 32}, {"frmerrsts", 0x099e, 16},
					      {"laneerrsts", 0x0a38, 32}};

static const struct pci_info vsp_offsets[] = {{"secsts", 0x001e, 16}, {"devsts", 0x004a, 16},
					      {"erruncsts", 0x0104, 32},
					      {"errcorsts", 0x0110, 32}};

static const struct pci_info msm_cfg[] = {{"cfg", 0x9a, 16}};

static const struct pci_info msm_offsets[] = {{"erruncsts", 0x0104, 32},
					      {"errcorsts", 0x0110, 32},
					      {"uncerstst", 0x0170, 32},
					      {"corerrsts", 0x0174, 32},
					      {"faberrsts", 0x01a8, 32}};

static const struct mmio_reg32 uncore_punit_offsets[] = {{PUNIT_MMIO_GT0_MC_STATUS},
							 {PUNIT_MMIO_GT0_FIRST_IERR_TSC_HI_CFG},
							 {PUNIT_MMIO_GT0_FIRST_IERR_TSC_LO_CFG},
							 {PUNIT_MMIO_GT0_PCU_UC_DEBUG}};

static const struct mmio_reg32 uncore_mdfi_offsets[] = {{MDFI_T2T_CTRL0_ERROR_STATUS},
							{MDFI_T2T_CTRL0_ECC_ERROR_STATUS},
							{MDFI_T2C_CTRL1_ERROR_STATUS},
							{MDFI_T2T_CTRL1_ECC_ERROR_STATUS},
							{MDFI_DEBUG_TRIGGER_ECC_ERROR_STATUS},
							{MDFI_DEBUG_TRIGGER_ERROR_STATUS}};

static const struct mmio_reg32 sgunit_regs_1[] = {{POISON_DATA_STATUS}, {MSG_GT_FATAL_ERROR},
						  {FUSA_IOSF_PARITY_CONTROL}, {ERR_STAT_GT_CORR},
						  {ERR_STAT_GT_NONFATAL}, {ERR_STAT_GT_FATAL},
						  {DEV_ERR_STAT_FATAL}, {DEV_ERR_STAT_NONFATAL},
						  {DEV_ERR_STAT_CORRECTABLE}, {DEV_PCIEERR_STATUS}
						  };

static const struct mmio_reg32 sgunit_regs_2[] = {{SGUNIT_FUSE_ERROR}, {SGUNIT_SPI_ERR_SRC}};

static const struct mmio_reg32 intr_regs[] = {{DG1_MSTR_TILE_INTR}, {GEN11_GFX_MSTR_IRQ}};

static const struct mmio_reg32 mert_regs[] = {{GTGP_MERTCTL2}, {GEN6_GT_GFX_RC6p}};

static const struct mmio_reg32 thermal_regs[] = {{PVC_GT0_PACKAGE_RAPL_LIMIT},
						 {PVC_PUNIT_PACKAGE_THERM_STATUS},
						 {PVC_PUNIT_DOMAIN_ENERGY_11},
						 {PVC_PUNIT_DOMAIN_ENERGY_3},
						 {PVC_PUNIT_DOMAIN_ENERGY_4},
						 {PVC_PUNIT_DOMAIN_ENERGY_5},
						 {PVC_PUNIT_DOMAIN_ENERGY_6},
						 {PVC_PUNIT_DOMAIN_ENERGY_7},
						 {PVC_PUNIT_DOMAIN_ENERGY_8},
						 {PVC_PUNIT_DOMAIN_ENERGY_20},
						 {PVC_PUNIT_DOMAIN_ENERGY_21},
						 {PVC_PUNIT_DOMAIN_ENERGY_22},
						 {PVC_PUNIT_DOMAIN_ENERGY_23},
						 {PVC_PUNIT_DOMAIN_ENERGY_24}};

static const struct mmio_reg32 gt_bc_counter[] = {{NODEA_ANY_ERROR}, {NODEB_ANY_ERROR},
						  {NODEA_BLCE_LNGP_UCE_I}, {NODEA_BMCB_LNGP_UCE_I},
						  {NODEA_L3BANKOUT_PREL3_FAT_ERR0},
						  {NODEA_LNEP_FATAL_ERROR},
						  {NODEA_LNGP_PARITY_ERROR},
						  {NODEA_LNGP_RDRTN_PARITY_ERR},
						  {NODEA_RR_LNGP_FAT_ERR_INT_F_NOA},
						  {NODEB_BLCE_LNGP_UCE_I}, {NODEB_BMCB_LNGP_UCE_I},
						  {NODEB_L3BANKOUT_PREL3_FAT_ERR0},
						  {NODEB_LNEP_FATAL_ERROR},
						  {NODEB_LNGP_PARITY_ERROR},
						  {NODEB_LNGP_RDRTN_PARITY_ERR},
						  {NODEB_RR_LNGP_FAT_ERR_INT_F_NOA}};

static const struct gt_slice_reg slice_regs[] = {
						 {MCFG_MCR_SELECTOR, 0x80000000, IDIPARITY_CONTROL},
						 {MCFG_MCR_SELECTOR, 0x81000000, IDIPARITY_CONTROL},
						 {MCFG_MCR_SELECTOR, 0x82000000, IDIPARITY_CONTROL},
						 {MCFG_MCR_SELECTOR, 0x83000000, IDIPARITY_CONTROL},
						 {MCFG_MCR_SELECTOR, 0x84000000, IDIPARITY_CONTROL},
						 {MCFG_MCR_SELECTOR, 0x85000000, IDIPARITY_CONTROL},
						 {MCFG_MCR_SELECTOR, 0x86000000, IDIPARITY_CONTROL},
						 {MCFG_MCR_SELECTOR, 0x87000000, IDIPARITY_CONTROL},
						 {MCFG_MCR_SELECTOR, 0x88000000, IDIPARITY_CONTROL},
						 {MCFG_MCR_SELECTOR, 0x89000000, IDIPARITY_CONTROL},
						 {MCFG_MCR_SELECTOR, 0x8a000000, IDIPARITY_CONTROL},
						 {MCFG_MCR_SELECTOR, 0x8b000000, IDIPARITY_CONTROL},
						 {MCFG_MCR_SELECTOR, 0x8c000000, IDIPARITY_CONTROL},
						 {MCFG_MCR_SELECTOR, 0x8d000000, IDIPARITY_CONTROL},
						 {MCFG_MCR_SELECTOR, 0x8e000000, IDIPARITY_CONTROL},
						 {MCFG_MCR_SELECTOR, 0x8f000000, IDIPARITY_CONTROL},
						 {GEN8_MCR_SELECTOR, 0x80000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x81000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x82000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x83000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x84000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x85000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x86000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x87000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x88000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x89000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x8a000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x8b000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x8c000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x8d000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x8e000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x8f000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x90000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x91000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x92000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x93000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x94000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x95000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x96000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x97000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x98000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x99000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x9a000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x9b000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x9c000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x9d000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x9e000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0x9f000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xa0000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xa1000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xa2000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xa3000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xa4000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xa5000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xa6000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xa7000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xa8000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xa9000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xaa000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xab000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xac000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xad000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xae000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xaf000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xb0000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xb1000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xb2000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xb3000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xb4000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xb5000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xb6000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xb7000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xb8000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xb9000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xba000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xbb000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xbc000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xbd000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xbe000000, SS_ERROR_LOG},
						 {GEN8_MCR_SELECTOR, 0xbf000000, SS_ERROR_LOG}};

static const struct hbm_reg_info hbm_regs[] = {{"internal_error_2lmmisc", _MMIO(0x286f70), 64},
					       {"internal_error_schedspq", _MMIO(0x287d80), 64},
					       {"internal_error_schedsbs", _MMIO(0x287a70), 64},
					       {"pc0_correrrcnt_0", _MMIO(0x288818), 32},
					       {"pc0_correrrcnt_1", _MMIO(0x28881c), 32},
					       {"pc0_correrrcnt_2", _MMIO(0x288820), 32},
					       {"pc0_correrrcnt_3", _MMIO(0x288824), 32},
					       {"pc0_internal_error_dp", _MMIO(0x288a00), 64},
					       {"pc0_imc0_mc_stat_shadow", _MMIO(0x287030), 64},
					       {"pc0_imc0_mc8_addr_shadow", _MMIO(0x286ed0), 64},
					       {"pc0_imc0_mc_misc_shadow", _MMIO(0x287040), 64},
					       {"pc0_retry_rd_err_log", _MMIO(0x288860), 32},
					       {"pc0_retry_rd_err_log_misc", _MMIO(0x288854), 32},
					       {"pc0_retry_rd_err_log_ad1", _MMIO(0x288858), 32},
					       {"pc0_retry_rd_err_log_ad2", _MMIO(0x288828), 32},
					       {"pc0_retry_rd_err_log_ad3", _MMIO(0x286ed8), 64},
					       {"pc0_retry_rd_err_log_par", _MMIO(0x288b08), 64},
					       {"pc0_retry_rd_err_set2_log", _MMIO(0x288a54), 32},
					       {"pc0_retry_rd_err_s2_log_ad1", _MMIO(0x288a58), 32},
					       {"pc0_retry_rd_err_s2_log_ad2", _MMIO(0x288a5c), 32},
					       {"pc0_retry_rd_err_s2_log_ad3", _MMIO(0x286ee0), 64},
					       {"pc0_retry_rd_err_s2_log_mis", _MMIO(0x288a60), 32},
					       {"pc0_retry_rd_err_s2_log_par", _MMIO(0x288b10), 64},
					       {"pc1_correrrcnt_0", _MMIO(0x288c18), 32},
					       {"pc1_correrrcnt_1", _MMIO(0x288c1c), 32},
					       {"pc1_correrrcnt_2", _MMIO(0x288c20), 32},
					       {"pc1_correrrcnt_3", _MMIO(0x288c24), 32},
					       {"pc1_internal_error_dp", _MMIO(0x288e00), 64},
					       {"pc1_imc0_mc_status_shadow", _MMIO(0x287430), 64},
					       {"pc1_imc0_mc8_addr_shad_dp1", _MMIO(0x286fa0), 64},
					       {"pc1_imc0_mc_misc_shadow", _MMIO(0x287440), 64},
					       {"pc1_retry_rd_err_log", _MMIO(0x288c60), 32},
					       {"pc1_retry_rd_err_log_misc", _MMIO(0x288c54), 32},
					       {"pc1_retry_rd_err_log_ad1", _MMIO(0x288c58), 32},
					       {"pc1_retry_rd_err_log_ad2", _MMIO(0x288c28), 32},
					       {"pc1_retry_rd_err_lg_ad3_dp1", _MMIO(0x286fa8), 64},
					       {"pc1_retry_rd_err_log_parity", _MMIO(0x288f08), 64},
					       {"pc1_retry_rd_err_s2_log", _MMIO(0x288e54), 32},
					       {"pc1_retry_rd_err_s2_log_ad1", _MMIO(0x288e58), 32},
					       {"pc1_retry_rd_err_s2_log_ad2", _MMIO(0x288e5c), 32},
					       {"pc1_retry_rd_err_s2_log_ad3_dp1",
						_MMIO(0x286fb0), 64},
					       {"pc1_retry_rd_err_s2_log_mis", _MMIO(0x288e60), 32},
					       {"pc1_retry_rd_err_s2_log_par", _MMIO(0x288f10), 64},
					       {"cpgc_seq_status", _MMIO(0x28a11c), 32},
					       {"pc0_cpgc_err_test_err_stat", _MMIO(0x28a2cc), 32},
					       {"pc1_cpgc_err_test_err_stat", _MMIO(0x28a6cc), 32}};

void pvc_fatal_error_dump_soc_regs(struct intel_gt *gt);
void pvc_fatal_error_dump_pci(struct intel_gt *gt, struct pci_dev *usp);
void pvc_fatal_error_dump_usp_pci_config(struct drm_i915_private *i915, struct pci_dev *usp);
void pvc_fatal_error_dump_vsp_pci_config(struct drm_i915_private *i915, struct pci_dev *vsp);
void pvc_fatal_error_dump_msm_pci_config(struct drm_i915_private *i915, struct pci_dev *vsp,
					 struct pci_dev *c1, struct pci_dev *c2);
void pvc_fatal_error_dump_punit_regs(struct intel_gt *gt);
void pvc_fatal_error_dump_mdfi_regs(struct intel_gt *gt);
void pvc_fatal_error_dump_heci_regs(struct intel_gt *gt);
void pvc_fatal_error_dump_sgunit_regs(struct intel_gt *gt);
void pvc_fatal_error_dump_thermal_regs(struct intel_gt *gt);
void pvc_fatal_error_dump_gt_bc_counter(struct intel_gt *gt);
void pvc_fatal_error_dump_slice_regs(struct intel_gt *gt);
void pvc_fatal_error_dump_hbm_regs(struct intel_gt *gt);

#endif
