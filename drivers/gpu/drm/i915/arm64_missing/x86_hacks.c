#include <linux/pci.h>

// THIS IS PLAIN WRONG AS THIS - COMPILE EFFORT ONLY

#define BIOS_END              0x00100000

static int
skip_isa_ioresource_align(struct pci_dev *dev) {
#if 0
        if ((pci_probe & PCI_CAN_SKIP_ISA_ALIGN) &&
            !(dev->bus->bridge_ctl & PCI_BRIDGE_CTL_ISA))
                return 1;
#endif
        return 0;
}

resource_size_t
pcibios_align_resource(void *data, const struct resource *res,
                        resource_size_t size, resource_size_t align)
{
        struct pci_dev *dev = data;
        resource_size_t start = res->start;

        if (res->flags & IORESOURCE_IO) {
                if (skip_isa_ioresource_align(dev))
                        return start;
                if (start & 0x300)
                        start = (start + 0x3ff) & ~0x3ff;
        } else if (res->flags & IORESOURCE_MEM) {
                /* The low 1MB range is reserved for ISA cards */
                if (start < BIOS_END)
                        start = BIOS_END;
        }
        return start;
}
EXPORT_SYMBOL(pcibios_align_resource);
