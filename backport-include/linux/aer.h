#ifndef _BACKPORT_AER_H_
#define _BACKPORT_AER_H_

#include_next<linux/aer.h>
#ifdef BPM_PCI_ENABLE_DISABLE_PCIE_ERROR_NOT_EXPORTED
#include <linux/pci.h>

#if defined(CONFIG_PCIEAER)
int pci_enable_pcie_error_reporting(struct pci_dev *dev);
int pci_disable_pcie_error_reporting(struct pci_dev *dev);
#else
static inline int pci_enable_pcie_error_reporting(struct pci_dev *dev)
{
               return -EINVAL;
}
static inline int pci_disable_pcie_error_reporting(struct pci_dev *dev)
{
               return -EINVAL;
}
#endif
#endif /* BPM_PCI_ENABLE_DISABLE_PCIE_ERROR_NOT_EXPORTED */
#endif /* _BACKPORT_AER_H_ */
