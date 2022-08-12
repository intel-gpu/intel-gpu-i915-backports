/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */
#include <linux/pci.h>

bool i915_pci_resource_valid(struct pci_dev *pdev, int bar);

int i915_register_pci_driver(void);
void i915_unregister_pci_driver(void);
