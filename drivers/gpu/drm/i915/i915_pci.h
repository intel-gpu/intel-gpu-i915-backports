/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef __I915_PCI_H__
#define __I915_PCI_H__

#include <linux/err.h>
#include <linux/types.h>

struct drm_i915_private;
struct pci_dev;

#ifdef CONFIG_PCI_IOV
struct pci_dev *i915_pci_pf_get_vf_dev(struct pci_dev *pdev, unsigned int id);
#else
static inline struct pci_dev *i915_pci_pf_get_vf_dev(struct pci_dev *pdev, unsigned int id)
{
	return ERR_PTR(-ENODEV);
}
#endif

int i915_pci_register_driver(void);
void i915_pci_unregister_driver(void);

bool i915_pci_resource_valid(struct pci_dev *pdev, int bar);
void i915_pci_set_offline(struct pci_dev *pdev);

void i915_pci_error_notify(struct drm_i915_private *i915);
void i915_pci_error_register(struct drm_i915_private *i915);
void i915_pci_error_unregister(struct drm_i915_private *i915);

void i915_pci_error_exit(void);

#endif /* __I915_PCI_H__ */
