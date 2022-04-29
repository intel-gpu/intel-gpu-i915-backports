/*
 * Copyright (c) 2021
 *
 * Backport functionality introduced in Linux 5.4.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/pci.h>
#include <linux/pci_regs.h>

/**
 * pci_rebar_find_pos - find position of resize ctrl reg for BAR
 * @pdev: PCI device
 * @bar: BAR to find
 *
 * Helper to find the position of the ctrl register for a BAR.
 * Returns -ENOTSUPP if resizable BARs are not supported at all.
 * Returns -ENOENT if no ctrl register for the BAR could be found.
 */
#if RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(8,5)
static int pci_rebar_find_pos(struct pci_dev *pdev, int bar)
{
	int cap = PCI_EXT_CAP_ID_REBAR;
	unsigned int pos, nbars, i;
	u32 ctrl;

#ifdef CONFIG_PCI_IOV
	if (bar >= PCI_IOV_RESOURCES) {
		cap = PCI_EXT_CAP_ID_VF_REBAR;
		bar -= PCI_IOV_RESOURCES;
	}
#endif

	pos = pci_find_ext_capability(pdev, cap);
	if (!pos)
		return -ENOTSUPP;

	pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl);
	nbars = (ctrl & PCI_REBAR_CTRL_NBAR_MASK) >>
		PCI_REBAR_CTRL_NBAR_SHIFT;

	for (i = 0; i < nbars; i++, pos += 8) {
		int bar_idx;

		pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl);
		bar_idx = ctrl & PCI_REBAR_CTRL_BAR_IDX;
		if (bar_idx == bar)
			return pos;
	}

	return -ENOENT;
}
/**
 * pci_rebar_get_possible_sizes - get possible sizes for BAR
 * @pdev: PCI device
 * @bar: BAR to query
 *
 * Get the possible sizes of a resizable BAR as bitmask defined in the spec
 * (bit 0=1MB, bit 19=512GB). Returns 0 if BAR isn't resizable.
 */
u32 pci_rebar_get_possible_sizes(struct pci_dev *pdev, int bar)
{
	int pos;
	u32 cap;

	pos = pci_rebar_find_pos(pdev, bar);
	if (pos < 0)
		return 0;

	pci_read_config_dword(pdev, pos + PCI_REBAR_CAP, &cap);
	return (cap & PCI_REBAR_CAP_SIZES) >> 4;
}
EXPORT_SYMBOL(pci_rebar_get_possible_sizes);
#endif /* RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(8,5) */
