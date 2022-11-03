// SPDX-License-Identifier: MIT
/*
 * Copyright © 2014-2019 Intel Corporation
 *
 * Authors:
 *    Vinit Azad <vinit.azad@intel.com>
 *    Ben Widawsky <ben@bwidawsk.net>
 *    Dave Gordon <david.s.gordon@intel.com>
 *    Alex Dai <yu.dai@intel.com>
 */

#include "gt/intel_gt.h"
#include "gt/intel_gt_regs.h"
#include "gt/intel_rps.h"
#include "intel_guc_fw.h"
#include "i915_drv.h"

static void guc_prepare_xfer(struct intel_uncore *uncore)
{
	u32 shim_flags = GUC_ENABLE_READ_CACHE_LOGIC |
			 GUC_ENABLE_READ_CACHE_FOR_SRAM_DATA |
			 GUC_ENABLE_READ_CACHE_FOR_WOPCM_DATA |
			 GUC_ENABLE_MIA_CLOCK_GATING;

	if (GRAPHICS_VER_FULL(uncore->i915) < IP_VER(12, 50))
		shim_flags |= GUC_DISABLE_SRAM_INIT_TO_ZEROES |
			      GUC_ENABLE_MIA_CACHING;

	/* Make GUC transactions uncacheable on PVC */
	if (HAS_GUC_PROGRAMMABLE_MOCS(uncore->i915))
		shim_flags |= PVC_GUC_MOCS_INDEX(PVC_MOCS_UC_INDEX);

	/* Must program this register before loading the ucode with DMA */
	intel_uncore_write(uncore, GUC_SHIM_CONTROL, shim_flags);

#if IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GEM)
	/* Enable the EIP counter for debug */
	if (GRAPHICS_VER_FULL(uncore->i915) >= IP_VER(12, 50))
		intel_uncore_rmw(uncore, GUC_SHIM_CONTROL2, 0, ENABLE_EIP);
#endif

	if (IS_GEN9_LP(uncore->i915))
		intel_uncore_write(uncore, GEN9LP_GT_PM_CONFIG, GT_DOORBELL_ENABLE);
	else
		intel_uncore_write(uncore, GEN9_GT_PM_CONFIG, GT_DOORBELL_ENABLE);

	if (GRAPHICS_VER(uncore->i915) == 9) {
		/* DOP Clock Gating Enable for GuC clocks */
		intel_uncore_rmw(uncore, GEN7_MISCCPCTL,
				 0, GEN8_DOP_CLOCK_GATE_GUC_ENABLE);

		/* allows for 5us (in 10ns units) before GT can go to RC6 */
		intel_uncore_write(uncore, GUC_ARAT_C6DIS, 0x1FF);
	}
}

static int guc_xfer_rsa_mmio(struct intel_uc_fw *guc_fw,
			     struct intel_uncore *uncore)
{
	u32 rsa[UOS_RSA_SCRATCH_COUNT];
	size_t copied;
	int i;

	copied = intel_uc_fw_copy_rsa(guc_fw, rsa, sizeof(rsa));
	if (copied < sizeof(rsa))
		return -ENOMEM;

	for (i = 0; i < UOS_RSA_SCRATCH_COUNT; i++)
		intel_uncore_write(uncore, UOS_RSA_SCRATCH(i), rsa[i]);

	return 0;
}

static int guc_xfer_rsa_vma(struct intel_uc_fw *guc_fw,
			    struct intel_uncore *uncore)
{
	struct intel_guc *guc = container_of(guc_fw, struct intel_guc, fw);

	intel_uncore_write(uncore, UOS_RSA_SCRATCH(0),
			   intel_guc_ggtt_offset(guc, guc_fw->rsa_data));

	return 0;
}

/* Copy RSA signature from the fw image to HW for verification */
static int guc_xfer_rsa(struct intel_uc_fw *guc_fw,
			struct intel_uncore *uncore)
{
	if (guc_fw->rsa_data)
		return guc_xfer_rsa_vma(guc_fw, uncore);
	else
		return guc_xfer_rsa_mmio(guc_fw, uncore);
}

/*
 * Read the GuC status register (GUC_STATUS) and store it in the
 * specified location; then return a boolean indicating whether
 * the value matches either completion or a known failure code.
 *
 * This is used for polling the GuC status in a wait_for()
 * loop below.
 */
static inline bool guc_load_done(struct intel_uncore *uncore, u32 *status, bool *success)
{
	u32 val = intel_uncore_read(uncore, GUC_STATUS);
	u32 uk_val = REG_FIELD_GET(GS_UKERNEL_MASK, val);
	u32 br_val = REG_FIELD_GET(GS_BOOTROM_MASK, val);

	*status = val;
	*success = true;
	switch (uk_val) {
		case INTEL_GUC_LOAD_STATUS_READY:
			return true;

		case INTEL_GUC_LOAD_STATUS_ERROR_DEVID_BUILD_MISMATCH:
		case INTEL_GUC_LOAD_STATUS_GUC_PREPROD_BUILD_MISMATCH:
		case INTEL_GUC_LOAD_STATUS_ERROR_DEVID_INVALID_GUCTYPE:
		case INTEL_GUC_LOAD_STATUS_HWCONFIG_DECRYPTION_ERROR:
		case INTEL_GUC_LOAD_STATUS_DPC_ERROR:
		case INTEL_GUC_LOAD_STATUS_EXCEPTION:
		case INTEL_GUC_LOAD_STATUS_INIT_DATA_INVALID:
		case INTEL_GUC_LOAD_STATUS_MPU_DATA_INVALID:
		case INTEL_GUC_LOAD_STATUS_INIT_MMIO_SAVE_RESTORE_INVALID:
			*success = false;
			return true;
	}

	switch (br_val) {
		case INTEL_BOOTROM_STATUS_RSA_FAILED:
		case INTEL_BOOTROM_STATUS_PAVPC_FAILED:
		case INTEL_BOOTROM_STATUS_WOPCM_FAILED:
		case INTEL_BOOTROM_STATUS_LOADLOC_FAILED:
		case INTEL_BOOTROM_STATUS_JUMP_FAILED:
		case INTEL_BOOTROM_STATUS_RC6CTXCONFIG_FAILED:
		case INTEL_BOOTROM_STATUS_MPUMAP_INCORRECT:
		case INTEL_BOOTROM_STATUS_EXCEPTION:
			*success = false;
			return true;
	}

	return false;
}

static int guc_wait_ucode(struct intel_uncore *uncore)
{
	bool success;
	u32 status;
	int ret;
	int count;
	ktime_t before, after, delta;
	u64 delta_ms;

	/*
	 * Wait for the GuC to start up.
	 * NB: Docs recommend not using the interrupt for completion.
	 * Measurements indicate this should take no more than 20ms
	 * (assuming the GT clock is at maximum frequency). So, a
	 * timeout here indicates that the GuC has failed and is unusable.
	 * (Higher levels of the driver may decide to reset the GuC and
	 * attempt the ucode load again if this happens.)
	 *
	 * FIXME: There is a known (but exceedingly unlikely) race condition
	 * where the asynchronous frequency management code could reduce
	 * the GT clock while a GuC reload is in progress (during a full
	 * GT reset). A fix is in progress but there are complex locking
	 * issues to be resolved. In the meantime bump the timeout to
	 * 200ms. Even at slowest clock, this should be sufficient. And
	 * in the working case, a larger timeout makes no difference.
	 *
	 * FIXME: There is possibly an unknown an even rarer race condition
	 * where 200ms is still not enough. However, there is a limit on how
	 * long an individual wait_for() can wait. So wrap it in a loop.
	 */
	before = ktime_get();
	for (count = 0; count < 20; count++) {
		ret = wait_for(guc_load_done(uncore, &status, &success), 1000);
		if (!ret || !success)
			break;
	}
	after = ktime_get();
	delta = ktime_sub(after, before);
	delta_ms = ktime_to_ms(delta);
	if (ret || !success) {
		struct drm_device *drm = &uncore->i915->drm;
		u32 ukernel = REG_FIELD_GET(GS_UKERNEL_MASK, status);
		u32 bootrom = REG_FIELD_GET(GS_BOOTROM_MASK, status);

		drm_info(drm, "GuC load failed: status = 0x%08X, time = %lldms, freq = %dMHz, ret = %d\n",
			 status, delta_ms, intel_rps_read_actual_frequency(&uncore->gt->rps), ret);
		drm_info(drm, "GuC load failed: status: Reset = %d, "
			"BootROM = 0x%02X, UKernel = 0x%02X, "
			"MIA = 0x%02X, Auth = 0x%02X\n",
			REG_FIELD_GET(GS_MIA_IN_RESET, status),
			bootrom,
			ukernel,
			REG_FIELD_GET(GS_MIA_MASK, status),
			REG_FIELD_GET(GS_AUTH_STATUS_MASK, status));

		if (bootrom == INTEL_BOOTROM_STATUS_RSA_FAILED) {
			drm_info(drm, "GuC firmware signature verification failed\n");
			ret = -ENOEXEC;
		}

		switch (ukernel) {
			case INTEL_GUC_LOAD_STATUS_EXCEPTION:
				drm_info(drm, "GuC firmware exception. EIP: %#x\n",
					 intel_uncore_read(uncore, SOFT_SCRATCH(13)));
				ret = -ENXIO;
				break;

			case INTEL_GUC_LOAD_STATUS_INIT_MMIO_SAVE_RESTORE_INVALID:
				drm_info(drm, "Illegal register in save/restore workaround list\n");
				ret = -EPERM;
				break;

			case INTEL_GUC_LOAD_STATUS_HWCONFIG_DECRYPTION_START:
				drm_info(drm, "GuC still decoding hwconfig table.\n");
				ret = -ETIMEDOUT;
				break;
		}

		/* Uncommon/unexpected error, see earlier status code print for details */
		if (ret == 0)
			ret = -ENXIO;

		/*
		 * If the GuC load has timed out, dump the instruction pointers
		 * so we can check where it stopped. The expectation here is
		 * that the GuC is stuck, so we dump the registers twice with a
		 * slight delay to confirm if the GuC has indeed stopped making
		 * forward progress or not.
		 * the 1ms was picked as a good balance between tolerating
		 * slowness and not waiting too long for the counters to
		 * increase.
		 */
		if (IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GEM) && ret == -ETIMEDOUT) {
			drm_info(drm, "EIP: %#x, EIPC: %#x\n",
				 intel_uncore_read(uncore, GUC_EIP),
				 intel_uncore_read(uncore, GUC_EIP_COUNTER));
			msleep(1);
			drm_info(drm, "EIP: %#x, EIPC: %#x\n",
				 intel_uncore_read(uncore, GUC_EIP),
				 intel_uncore_read(uncore, GUC_EIP_COUNTER));
		}
	} else if (delta_ms > 200) {
		drm_warn(&uncore->i915->drm, "Excessive GuC init time: %lldms! [freq = %dMHz, status = 0x%08X, count = %d, ret = %d]\n",
			 delta_ms, intel_rps_read_actual_frequency(&uncore->gt->rps), status, count, ret);
	} else {
		drm_dbg(&uncore->i915->drm, "GuC init took %lldms, freq = %dMHz, status = 0x%08X, count = %d, ret = %d\n",
			delta_ms, intel_rps_read_actual_frequency(&uncore->gt->rps), status, count, ret);
	}

	return ret;
}

/**
 * intel_guc_fw_upload() - load GuC uCode to device
 * @guc: intel_guc structure
 *
 * Called from intel_uc_init_hw() during driver load, resume from sleep and
 * after a GPU reset.
 *
 * The firmware image should have already been fetched into memory, so only
 * check that fetch succeeded, and then transfer the image to the h/w.
 *
 * Return:	non-zero code on error
 */
int intel_guc_fw_upload(struct intel_guc *guc)
{
	struct intel_gt *gt = guc_to_gt(guc);
	struct intel_uncore *uncore = gt->uncore;
	int ret;

	guc_prepare_xfer(uncore);

	/*
	 * Note that GuC needs the CSS header plus uKernel code to be copied
	 * by the DMA engine in one operation, whereas the RSA signature is
	 * loaded separately, either by copying it to the UOS_RSA_SCRATCH
	 * register (if key size <= 256) or through a ggtt-pinned vma (if key
	 * size > 256). The RSA size and therefore the way we provide it to the
	 * HW is fixed for each platform and hard-coded in the bootrom.
	 */
	ret = guc_xfer_rsa(&guc->fw, uncore);
	if (ret)
		goto out;

	/*
	 * Current uCode expects the code to be loaded at 8k; locations below
	 * this are used for the stack.
	 */
	ret = intel_uc_fw_upload(&guc->fw, 0x2000, UOS_MOVE);
	if (ret)
		goto out;

	ret = guc_wait_ucode(uncore);
	if (ret)
		goto out;

	intel_uc_fw_change_status(&guc->fw, INTEL_UC_FIRMWARE_RUNNING);
	return 0;

out:
	intel_uc_fw_change_status(&guc->fw, INTEL_UC_FIRMWARE_LOAD_FAIL);
	return ret;
}
