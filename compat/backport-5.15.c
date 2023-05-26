#include<linux/pci.h>

#ifdef BPM_PCI_FIND_HOST_BRIDGE_NOT_EXPORTED
static struct pci_bus *find_pci_root_bus(struct pci_bus *bus)
{
        while (bus->parent)
                bus = bus->parent;

        return bus;
}

struct pci_host_bridge *pci_find_host_bridge(struct pci_bus *bus)
{
	struct pci_bus *root_bus = find_pci_root_bus(bus);

	return to_pci_host_bridge(root_bus->bridge);
}
EXPORT_SYMBOL_GPL(pci_find_host_bridge);
#endif /* BPM_PCI_FIND_HOST_BRIDGE_NOT_EXPORTED */
