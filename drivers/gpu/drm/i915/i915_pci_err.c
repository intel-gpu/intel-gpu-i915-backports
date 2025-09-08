// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <linux/pci.h>
#include <linux/workqueue.h>

#if !IS_ENABLED(CONFIG_AUXILIARY_BUS)
#include <linux/mfd/core.h>
#endif

#include "gt/intel_gt.h"
#include "gt/intel_gt_requests.h"

#include "i915_drv.h"
#include "i915_driver.h"
#include "i915_pci.h"
#include "intel_iaf.h"

static int hw_error_count;

/**
 * i915_pci_error_detected - Called when a PCI error is detected.
 * @pdev: PCI device struct
 * @state: PCI channel state
 *
 * Description: Called when a PCI error is detected.
 * the intention here is to terminate the driver state without touching the device
 *
 * Return: PCI_ERS_RESULT_NEED_RESET or PCI_ERS_RESULT_DISCONNECT.
 */
static pci_ers_result_t i915_pci_error_detected(struct pci_dev *pdev,
						pci_channel_state_t state)
{
	struct drm_i915_private *i915;
	struct intel_gt *gt;
	int i;

	i915 = pci_get_drvdata(pdev);
	if (!i915) /* already removed / shutdown */
		return PCI_ERS_RESULT_DISCONNECT;

	dev_err(&pdev->dev, "PCI error detected, state %d\n", state);

	/*
	 * Record the fault on the device to skip waits-for-ack and other
	 * low level HW access and unplug the device from userspace.
	 */
	i915_pci_error_set_in_recovery(i915);
	if (i915_pci_error_set_fault(i915))
		return PCI_ERS_RESULT_DISCONNECT;

	dev_warn(i915->drm.dev, "removing device access to userspace\n");
	add_taint_for_CI(i915, TAINT_DIE);
	for_each_gt(gt, i915, i)
		intel_gt_set_wedged_async(gt);

	wake_up_all(&i915->user_fence_wq);

	/*
	 * On the current generation HW we do not expect
	 * pci_channel_io_normal to be reported in pci_channel_state as it
	 * is only related to non-fatal error handling.
	 */
	if (state == pci_channel_io_perm_failure)
		return PCI_ERS_RESULT_DISCONNECT;

	/*
	 * The offline field in struct device is used by MEI driver when
	 * trying to access the device. The mei will check this flag in
	 * mei_gsc_remove() and will complete the remove flow without
	 * read/write to the HW registers
	 */
	i915_pci_set_offline(pdev);
	intel_iaf_pcie_error_notify(i915);

	pci_disable_device(pdev);
	return PCI_ERS_RESULT_NEED_RESET;
}

static bool check_enable_pci(struct pci_dev *pdev)
{
	u16 pmcsr;

	pci_read_config_word(pdev, pdev->pm_cap + PCI_PM_CTRL, &pmcsr);
	return pmcsr != (u16)-1;
}

/**
 * i915_pci_slot_reset - Called after PCI slot is reset
 * @pdev: PCI device struct
 *
 * Description: This is called by PCIe error recovery after the PCI slot
 * has been reset. Device should be in fresh uninitialized state driver is
 * expected to reinitialize the device similar to boot process but not
 * accepting any work
 *
 * Return: PCI_ERS_RESULT_RECOVERED or PCI_ERS_RESULT_DISCONNECT
 */
static pci_ers_result_t i915_pci_slot_reset(struct pci_dev *pdev)
{
	const struct pci_device_id *ent = pci_match_id(pdev->driver->id_table, pdev);

	/*
	 * We want to completely clean the driver and even destroy
	 * the i915 private data and reinitialize afresh similar to
	 * probe
	 */
	device_release_driver(&pdev->dev);

	/* Arbitrary wait time for HW to come out of reset */
	dev_info(&pdev->dev,
		 "PCI slot has been reset, waiting upto 60s to re-enable\n");
	if (wait_for(check_enable_pci(pdev), 60000))
		return PCI_ERS_RESULT_DISCONNECT;

	if (!i915_driver_probe(pdev, ent)) {
		if (i915_save_pci_state(pdev))
			pci_restore_state(pdev);
		return PCI_ERS_RESULT_RECOVERED;
	}

	return PCI_ERS_RESULT_DISCONNECT;
}

/*
 * i915_pci_err_resume - called when device start IO again
 * @pdev PCI device struct
 *
 * This callback is called when the error recovery driver tells us that
 * its OK to resume normal operation. Driver exposes the device to
 * userspace
 */
static void i915_pci_err_resume(struct pci_dev *pdev)
{
	struct drm_i915_private *i915 = pci_get_drvdata(pdev);
	intel_wakeref_t wakeref;

	dev_info(&pdev->dev,
		 "recovered from PCIe error, resuming GPU submission\n");

	with_intel_runtime_pm(&i915->runtime_pm, wakeref)
		i915_driver_register(i915);
}

const struct pci_error_handlers i915_pci_err_handlers = {
	.error_detected = i915_pci_error_detected,
	.slot_reset = i915_pci_slot_reset,
	.resume = i915_pci_err_resume,
};

static struct pci_dev *get_usp_dev(struct pci_dev *dev)
{
	int i;

	/*
	 * PVC GFX device(SGUNIT) heirarchy is:
	 *
	 * RP--->USP-------->VSP0-->SGUNIT
	 *
	 * so iterate twice to reach the USP.
	 *
	 */
	for (i = 0; i < 2 ; i++)
		dev = pci_upstream_bridge(dev);

	return dev;
}

static inline u32 dg1_master_intr_disable(void __iomem * const regs)
{
	u32 val;

	/* First disable interrupts */
	raw_reg_write(regs, DG1_MSTR_TILE_INTR, 0);

	/* Get the indication levels and ack the master unit */
	val = raw_reg_read(regs, DG1_MSTR_TILE_INTR);
	if (unlikely(!val))
		return 0;

	raw_reg_write(regs, DG1_MSTR_TILE_INTR, val);

	return val;
}

#define PCI_HOTPLUG_MASK \
	(PCI_EXP_SLTCTL_ABPE |	PCI_EXP_SLTCTL_HPIE | PCI_EXP_SLTCTL_CCIE | PCI_EXP_SLTCTL_DLLSCE)

static void disable_hotplug_interrupts(struct pci_dev *pdev)
{
	u16 slot_ctrl;

	if (!pdev->is_hotplug_bridge)
		return;

	pci_read_config_word(pdev, pdev->pcie_cap + PCI_EXP_SLTCTL, &slot_ctrl);
	pci_write_config_word(pdev, pdev->pcie_cap + PCI_EXP_SLTCTL, slot_ctrl & ~PCI_HOTPLUG_MASK);
}

static void enable_hotplug_interrupts(struct pci_dev *pdev)
{
	u16 slot_ctrl;
	u16 slot_stat;

	if (!pdev->is_hotplug_bridge)
		return;

	pci_read_config_word(pdev, pdev->pcie_cap + PCI_EXP_SLTCTL, &slot_ctrl);
	pci_read_config_word(pdev, pdev->pcie_cap + PCI_EXP_SLTSTA, &slot_stat);

	pci_write_config_word(pdev, pdev->pcie_cap + PCI_EXP_SLTSTA,
			      slot_stat | PCI_EXP_SLTSTA_DLLSC | PCI_EXP_SLTSTA_CC);
	pci_write_config_word(pdev, pdev->pcie_cap + PCI_EXP_SLTCTL,
			      slot_ctrl | PCI_HOTPLUG_MASK);
}

static void error_notify_cb(struct drm_i915_private *i915)
{
	struct pci_dev *pdev = to_pci_dev(i915->drm.dev);
	struct pci_dev *rdev = pcie_find_root_port(pdev);
	struct pci_dev *usp = get_usp_dev(pdev);
	bool no_sbr = i915->params.enable_fatal_error_recovery == MSI_NO_SBR;

	/*
	 * Record the fault on the device to skip waits-for-ack and other
	 * low level HW access and unplug the device from userspace.
	 */
	i915_pci_error_detected(pdev, pci_channel_io_normal);
	device_release_driver(&pdev->dev);

	if (no_sbr)
		return;

	pci_lock_rescan_remove();

	pci_stop_and_remove_bus_device(usp);
	msleep(500);

	disable_hotplug_interrupts(rdev);

	pci_bridge_secondary_bus_reset(rdev);
	msleep(500);

	enable_hotplug_interrupts(rdev);
	pci_unlock_rescan_remove();
}

static LIST_HEAD(error_notifier);
static DEFINE_MUTEX(error_mutex);
static void error_fn(struct work_struct *wrk)
{
	struct drm_i915_private *i915;
	struct list_head bookmark;
	struct pci_bus *bus = NULL;

	pr_notice(DRIVER_NAME " Disabling all devices\n");
	hw_error_count++;

	mutex_lock(&error_mutex);
	list_for_each_entry(i915, &error_notifier, error_notify) {
		list_add(&bookmark, &i915->error_notify);
		mutex_unlock(&error_mutex);

		error_notify_cb(i915);

		mutex_lock(&error_mutex);
		i915 = container_of(&bookmark, typeof(*i915), error_notify);
		__list_del_entry(&bookmark);
	}
	mutex_unlock(&error_mutex);

	pr_info(DRIVER_NAME " Rescanning PCI bus\n");
	pci_lock_rescan_remove();
	while ((bus = pci_find_next_bus(bus)) != NULL)
		pci_rescan_bus(bus);
	pci_unlock_rescan_remove();
}

static DECLARE_DELAYED_WORK(error_work, error_fn);

void i915_pci_error_notify(struct drm_i915_private *i915)
{
	struct pci_dev *pdev = to_pci_dev(i915->drm.dev);

	if (i915_pci_error_detected(pdev, pci_channel_io_normal) == PCI_ERS_RESULT_DISCONNECT)
		return;

	queue_delayed_work(system_unbound_wq, &error_work,
			   round_jiffies_up_relative(msecs_to_jiffies(CPTCFG_DRM_I915_PCI_RECOVERY_DELAY_MS)));
}

void i915_pci_error_register(struct drm_i915_private *i915)
{
	mutex_lock(&error_mutex);
	list_add(&i915->error_notify, &error_notifier);
	mutex_unlock(&error_mutex);
}

void i915_pci_error_unregister(struct drm_i915_private *i915)
{
	mutex_lock(&error_mutex);
	list_del(&i915->error_notify);
	mutex_unlock(&error_mutex);
}

void i915_pci_error_exit(void)
{
	flush_delayed_work(&error_work);
}

#if IS_ENABLED(CPTCFG_DRM_I915_DEBUG)
static int hw_error_inject_set(const char *val, const struct kernel_param *kp)
{
	mod_delayed_work(system_unbound_wq, &error_work, 0);
	return 0;
}

static int hw_error_inject_get(char *val, const struct kernel_param *kp)
{
	strcpy(val, "0");
	return 0;
}

static const struct kernel_param_ops ops = {
	.set = hw_error_inject_set,
	.get = hw_error_inject_get,
};
module_param_cb_unsafe(hw_error_inject, &ops, NULL, 0600);
MODULE_PARM_DESC(hw_error_inject, "Simulate a fatal HW error forcing device recovery");
#endif

module_param_named(hw_error_count, hw_error_count, int, 0400);
