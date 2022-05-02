#ifndef __BACKPORT_PCI_H
#define __BACKPORT_PCI_H
#include <linux/version.h>
#include_next <linux/pci.h>

#if LINUX_VERSION_IS_LESS(5,10,0)

#define pci_rebar_get_possible_sizes LINUX_I915_BACKPORT(pci_rebar_get_possible_sizes)
u32 pci_rebar_get_possible_sizes(struct pci_dev *pdev, int bar);

#define pci_rebar_bytes_to_size LINUX_I915_BACKPORT(pci_rebar_bytes_to_size)
static inline int pci_rebar_bytes_to_size(u64 bytes)
{
        bytes = roundup_pow_of_two(bytes);

        /* Return BAR size as defined in the resizable BAR specification */
        return max(ilog2(bytes), 20) - 20;
}

#endif

#endif /* __BACKPORT_PCI_H */
