// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */
#include <linux/pci.h>

#if !IS_ENABLED(CONFIG_AUXILIARY_BUS)
#include <linux/mfd/core.h>
#endif

#include "gt/intel_gt.h"
#include "gt/intel_gt_requests.h"

#include "i915_drv.h"
#include "i915_driver.h"
#include "intel_iaf.h"
#include "i915_pci.h"

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
	struct drm_i915_private *i915 = pci_get_drvdata(pdev);
	struct intel_gt *gt;
	int i;

	dev_warn(&pdev->dev, "PCI error detected, state %d\n", state);

	/*
	 * Record the fault on the device to skip waits-for-ack and other
	 * low level HW access and unplug the device from userspace.
	 */
	i915_pci_error_set_fault(i915);
	drm_warn(&i915->drm, "removing device access to userspace\n");
	drm_dev_unplug(&i915->drm);

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

#if !IS_ENABLED(CONFIG_AUXILIARY_BUS)
	mfd_remove_devices(&pdev->dev);
#endif

	for_each_gt(gt, i915, i) {
		intel_gt_set_wedged(gt);
		intel_gt_retire_requests(gt);
	}
	pci_disable_device(pdev);
	return PCI_ERS_RESULT_NEED_RESET;
}

#ifdef BPM_DISABLE_DRM_DMABUF
static void fake_devm_drm_release_action(struct device *dev, void *res)
{
	struct devres_node {
		struct list_head		entry;
		dr_release_t			release;
#ifdef CONFIG_DEBUG_DEVRES
		const char			*name;
		size_t				size;
#endif
	} *node, *tmp;

	struct devres {
		struct devres_node		node;
		/*
		 * Some archs want to perform DMA into kmalloc caches
		 * and need a guaranteed alignment larger than
		 * the alignment of a 64-bit integer.
		 * Thus we use ARCH_KMALLOC_MINALIGN here and get exactly the same
		 * buffer alignment as if it was allocated by plain kmalloc().
		 */
		u8 __aligned(ARCH_KMALLOC_MINALIGN) data[];
	} *dr;

	struct action_devres {
		void *data;
		void (*action)(void *);
	} *dres = NULL;
	unsigned long flags;

	spin_lock_irqsave(&dev->devres_lock, flags);
	list_for_each_entry_safe_reverse(node, tmp, &dev->devres_head, entry) {
		dr = container_of(node, struct devres, node);
		dres = (struct action_devres *)dr->data;

		if (dres->data == res)
			break;
		dres = NULL;
	}
	spin_unlock_irqrestore(&dev->devres_lock, flags);

	if (!WARN_ON(!dres))
		devm_release_action(dev, dres->action, res);
}
#endif
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
	struct drm_i915_private *i915 = pci_get_drvdata(pdev);
	const struct pci_device_id *ent = pci_match_id(pdev->driver->id_table, pdev);

	/* Arbitrary wait time for HW to come out of reset */
	dev_info(&pdev->dev,
		 "PCI slot has been reset, waiting 5s to re-enable\n");
	msleep(5000);
	if (pci_enable_device(pdev)) {
		dev_err(&pdev->dev,
			"Cannot re-enable PCI device after reset.\n");
		return PCI_ERS_RESULT_DISCONNECT;
	}
	pci_set_master(pdev);
	i915_load_pci_state(pdev);

	/*
	 * We want to completely clean the driver and even destroy
	 * the i915 private data and reinitialize afresh similar to
	 * probe
	 */
	i915_pci_error_clear_fault(i915);
	pdev->driver->remove(pdev);
#ifdef BPM_DISABLE_DRM_DMABUF
	fake_devm_drm_release_action(&pdev->dev, &i915->drm);
#else
	devm_drm_release_action(&i915->drm);
#endif
	pci_disable_device(pdev);

	if (!i915_driver_probe(pdev, ent)) {
		if (i915_save_pci_state(pdev))
			pci_restore_state(pdev);
		return PCI_ERS_RESULT_RECOVERED;
	}

	return PCI_ERS_RESULT_DISCONNECT;
}

/**
 * i915_pci_resume - called when device start IO again
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
