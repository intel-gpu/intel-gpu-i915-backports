#include <linux/aer.h>
#include <linux/pci.h>
#ifdef BPM_PCIE_AER_IS_NATIVE_API_NOT_PRESENT
#include "proc_fs.h"
#endif
#define PCI_EXP_AER_FLAGS       (PCI_EXP_DEVCTL_CERE | PCI_EXP_DEVCTL_NFERE | \
                                PCI_EXP_DEVCTL_FERE | PCI_EXP_DEVCTL_URRE)

#ifdef BPM_PCIE_AER_IS_NATIVE_API_NOT_PRESENT
#ifdef CONFIG_PCIEAER
int pcie_aer_is_native(struct pci_dev *dev)
{
        struct pci_host_bridge *host = pci_find_host_bridge(dev->bus);

        if (!dev->aer_cap)
                return 0;

        return pcie_ports_native || host->native_aer;
}
#else
static inline int pcie_aer_is_native(struct pci_dev *dev) { return 0; }
#endif
#endif

#ifdef BPM_PCI_ENABLE_DISABLE_PCIE_ERROR_NOT_EXPORTED
int pci_enable_pcie_error_reporting(struct pci_dev *dev)
{
       int rc;

       if (!pcie_aer_is_native(dev))
               return -EIO;

       rc = pcie_capability_set_word(dev, PCI_EXP_DEVCTL, PCI_EXP_AER_FLAGS);
       return pcibios_err_to_errno(rc);
}
EXPORT_SYMBOL_GPL(pci_enable_pcie_error_reporting);

int pci_disable_pcie_error_reporting(struct pci_dev *dev)
{
       int rc;

       if (!pcie_aer_is_native(dev))
               return -EIO;

       rc = pcie_capability_clear_word(dev, PCI_EXP_DEVCTL, PCI_EXP_AER_FLAGS);
       return pcibios_err_to_errno(rc);
}
EXPORT_SYMBOL_GPL(pci_disable_pcie_error_reporting);
#endif

#ifdef BPM_MODULE_IMPORT_NS_CXL_SUPPORT
MODULE_IMPORT_NS(CXL);
#endif

