#ifndef __BACKPORT_PCI_H
#define __BACKPORT_PCI_H
#include <linux/version.h>
#include_next <linux/pci.h>

#if RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(8,5) && LINUX_VERSION_IS_LESS(5,4,0)
u32 pci_rebar_get_possible_sizes(struct pci_dev *pdev, int bar);
#endif

/*actually introduced in 5.12, for now keeping 5.10*/
#if RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(8,5) && LINUX_VERSION_IS_LESS(5,10,0)
static inline int pci_rebar_bytes_to_size(u64 bytes)
{
        bytes = roundup_pow_of_two(bytes);

        /* Return BAR size as defined in the resizable BAR specification */
        return max(ilog2(bytes), 20) - 20;
}

#endif
#endif /* __BACKPORT_PCI_H */

