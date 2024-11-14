#ifndef _BACKPORT_LINUX_PCI_H
#define _BACKPORT_LINUX_PCI_H
#include_next <linux/pci.h>
#include <linux/version.h>

#ifdef BPM_PCI_ASPM_H_NOT_PRESENT
#include <linux/pci-aspm.h>
#endif

#ifndef module_pci_driver
/**
 * module_pci_driver() - Helper macro for registering a PCI driver
 * @__pci_driver: pci_driver struct
 *
 * Helper macro for PCI drivers which do not do anything special in module
 * init/exit. This eliminates a lot of boilerplate. Each module may only
 * use this macro once, and calling it replaces module_init() and module_exit()
 */
#define module_pci_driver(__pci_driver) \
	module_driver(__pci_driver, pci_register_driver, \
		       pci_unregister_driver)
#endif

#if LINUX_VERSION_IS_LESS(3,7,0)
#define pcie_capability_read_word LINUX_I915_BACKPORT(pcie_capability_read_word)
int pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val);
#define pcie_capability_read_dword LINUX_I915_BACKPORT(pcie_capability_read_dword)
int pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val);
#define pcie_capability_write_word LINUX_I915_BACKPORT(pcie_capability_write_word)
int pcie_capability_write_word(struct pci_dev *dev, int pos, u16 val);
#define pcie_capability_write_dword LINUX_I915_BACKPORT(pcie_capability_write_dword)
int pcie_capability_write_dword(struct pci_dev *dev, int pos, u32 val);
#define pcie_capability_clear_and_set_word LINUX_I915_BACKPORT(pcie_capability_clear_and_set_word)
int pcie_capability_clear_and_set_word(struct pci_dev *dev, int pos,
				       u16 clear, u16 set);
#define pcie_capability_clear_and_set_dword LINUX_I915_BACKPORT(pcie_capability_clear_and_set_dword)
int pcie_capability_clear_and_set_dword(struct pci_dev *dev, int pos,
					u32 clear, u32 set);

#define pcie_capability_set_word LINUX_I915_BACKPORT(pcie_capability_set_word)
static inline int pcie_capability_set_word(struct pci_dev *dev, int pos,
					   u16 set)
{
	return pcie_capability_clear_and_set_word(dev, pos, 0, set);
}

#define pcie_capability_set_dword LINUX_I915_BACKPORT(pcie_capability_set_dword)
static inline int pcie_capability_set_dword(struct pci_dev *dev, int pos,
					    u32 set)
{
	return pcie_capability_clear_and_set_dword(dev, pos, 0, set);
}

#define pcie_capability_clear_word LINUX_I915_BACKPORT(pcie_capability_clear_word)
static inline int pcie_capability_clear_word(struct pci_dev *dev, int pos,
					     u16 clear)
{
	return pcie_capability_clear_and_set_word(dev, pos, clear, 0);
}

#define pcie_capability_clear_dword LINUX_I915_BACKPORT(pcie_capability_clear_dword)
static inline int pcie_capability_clear_dword(struct pci_dev *dev, int pos,
					      u32 clear)
{
	return pcie_capability_clear_and_set_dword(dev, pos, clear, 0);
}
#endif

#ifndef PCI_DEVICE_SUB
/**
 * PCI_DEVICE_SUB - macro used to describe a specific pci device with subsystem
 * @vend: the 16 bit PCI Vendor ID
 * @dev: the 16 bit PCI Device ID
 * @subvend: the 16 bit PCI Subvendor ID
 * @subdev: the 16 bit PCI Subdevice ID
 *
 * This macro is used to create a struct pci_device_id that matches a
 * specific device with subsystem information.
 */
#define PCI_DEVICE_SUB(vend, dev, subvend, subdev) \
	.vendor = (vend), .device = (dev), \
	.subvendor = (subvend), .subdevice = (subdev)
#endif /* PCI_DEVICE_SUB */

#if LINUX_VERSION_IS_LESS(3,2,0)
#define pci_dev_flags LINUX_I915_BACKPORT(pci_dev_flags)
#define PCI_DEV_FLAGS_MSI_INTX_DISABLE_BUG LINUX_I915_BACKPORT(PCI_DEV_FLAGS_MSI_INTX_DISABLE_BUG)
#define PCI_DEV_FLAGS_NO_D3 LINUX_I915_BACKPORT(PCI_DEV_FLAGS_NO_D3)
#define PCI_DEV_FLAGS_ASSIGNED LINUX_I915_BACKPORT(PCI_DEV_FLAGS_ASSIGNED)
enum pci_dev_flags {
	/* INTX_DISABLE in PCI_COMMAND register disables MSI
	 * generation too.
	 */
	PCI_DEV_FLAGS_MSI_INTX_DISABLE_BUG = (__force pci_dev_flags_t) 1,
	/* Device configuration is irrevocably lost if disabled into D3 */
	PCI_DEV_FLAGS_NO_D3 = (__force pci_dev_flags_t) 2,
	/* Provide indication device is assigned by a Virtual Machine Manager */
	PCI_DEV_FLAGS_ASSIGNED = (__force pci_dev_flags_t) 4,
};
#endif /* LINUX_VERSION_IS_LESS(3,2,0) */

#if LINUX_VERSION_IS_LESS(3,8,0)
#define pci_sriov_set_totalvfs LINUX_I915_BACKPORT(pci_sriov_set_totalvfs)
int pci_sriov_set_totalvfs(struct pci_dev *dev, u16 numvfs);
#endif /* LINUX_VERSION_IS_LESS(3,8,0) */

#if LINUX_VERSION_IS_LESS(3,10,0)
/* Taken from drivers/pci/pci.h */
struct pci_sriov {
	int pos;		/* capability position */
	int nres;		/* number of resources */
	u32 cap;		/* SR-IOV Capabilities */
	u16 ctrl;		/* SR-IOV Control */
	u16 total_VFs;		/* total VFs associated with the PF */
	u16 initial_VFs;	/* initial VFs associated with the PF */
	u16 num_VFs;		/* number of VFs available */
	u16 offset;		/* first VF Routing ID offset */
	u16 stride;		/* following VF stride */
	u32 pgsz;		/* page size for BAR alignment */
	u8 link;		/* Function Dependency Link */
	u16 driver_max_VFs;	/* max num VFs driver supports */
	struct pci_dev *dev;	/* lowest numbered PF */
	struct pci_dev *self;	/* this PF */
	struct mutex lock;	/* lock for VF bus */
	struct work_struct mtask; /* VF Migration task */
	u8 __iomem *mstate;	/* VF Migration State Array */
};

#define pci_vfs_assigned LINUX_I915_BACKPORT(pci_vfs_assigned)
#ifdef CONFIG_PCI_IOV
int pci_vfs_assigned(struct pci_dev *dev);
#else
static inline int pci_vfs_assigned(struct pci_dev *dev)
{
	return 0;
}
#endif

#endif /* LINUX_VERSION_IS_LESS(3,10,0) */

#if LINUX_VERSION_IS_LESS(4,8,0)
#define pci_alloc_irq_vectors LINUX_I915_BACKPORT(pci_alloc_irq_vectors)
#ifdef CONFIG_PCI_MSI
int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs,
		unsigned int max_vecs, unsigned int flags);
#else
static inline int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs,
		unsigned int max_vecs, unsigned int flags)
{ return -ENOSYS; }
#endif
#endif

#if LINUX_VERSION_IS_LESS(4,8,0)
#define pci_free_irq_vectors LINUX_I915_BACKPORT(pci_free_irq_vectors)
static inline void pci_free_irq_vectors(struct pci_dev *dev)
{
}
#endif

#if LINUX_VERSION_IS_LESS(3,14,0)
#define pci_enable_msi_range LINUX_I915_BACKPORT(pci_enable_msi_range)
#ifdef CONFIG_PCI_MSI
int pci_enable_msi_range(struct pci_dev *dev, int minvec, int maxvec);
#else
static inline int pci_enable_msi_range(struct pci_dev *dev, int minvec,
				       int maxvec)
{ return -ENOSYS; }
#endif
#endif

#ifdef CONFIG_PCI
#if LINUX_VERSION_IS_LESS(3,14,0)
#define pci_enable_msix_range LINUX_I915_BACKPORT(pci_enable_msix_range)
#ifdef CONFIG_PCI_MSI
int pci_enable_msix_range(struct pci_dev *dev, struct msix_entry *entries,
			  int minvec, int maxvec);
#else
static inline int pci_enable_msix_range(struct pci_dev *dev,
		      struct msix_entry *entries, int minvec, int maxvec)
{ return -ENOSYS; }
#endif
#endif
#endif

#if LINUX_VERSION_IS_LESS(3,13,0)
#define pci_device_is_present LINUX_I915_BACKPORT(pci_device_is_present)
bool pci_device_is_present(struct pci_dev *pdev);
#endif

#ifdef CONFIG_PCI
#if LINUX_VERSION_IS_LESS(3,14,0)
#define pci_enable_msix_exact LINUX_I915_BACKPORT(pci_enable_msix_exact)
#ifdef CONFIG_PCI_MSI
static inline int pci_enable_msix_exact(struct pci_dev *dev,
					struct msix_entry *entries, int nvec)
{
	int rc = pci_enable_msix_range(dev, entries, nvec, nvec);
	if (rc < 0)
		return rc;
	return 0;
}
#else
static inline int pci_enable_msix_exact(struct pci_dev *dev,
		      struct msix_entry *entries, int nvec)
{ return -ENOSYS; }
#endif /* CONFIG_PCI_MSI */
#endif
#endif /* CONFIG_PCI */

#if LINUX_VERSION_IS_LESS(4,9,0) &&			\
	!LINUX_VERSION_IN_RANGE(3,12,69, 3,13,0) &&	\
	!LINUX_VERSION_IN_RANGE(4,4,37, 4,5,0) &&	\
	!LINUX_VERSION_IN_RANGE(4,8,13, 4,9,0)

static inline struct pci_dev *pcie_find_root_port(struct pci_dev *dev)
{
	while (1) {
		if (!pci_is_pcie(dev))
			break;
		if (pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT)
			return dev;
		if (!dev->bus->self)
			break;
		dev = dev->bus->self;
	}
	return NULL;
}

#endif/* <4.9.0 but not >= 3.12.69, 4.4.37, 4.8.13 */

#ifndef PCI_IRQ_LEGACY
#define PCI_IRQ_LEGACY		(1 << 0) /* Allow legacy interrupts */
#define PCI_IRQ_MSI		(1 << 1) /* Allow MSI interrupts */
#define PCI_IRQ_MSIX		(1 << 2) /* Allow MSI-X interrupts */
#define PCI_IRQ_ALL_TYPES \
	(PCI_IRQ_LEGACY | PCI_IRQ_MSI | PCI_IRQ_MSIX)
#endif

#if defined(CONFIG_PCI)
#if LINUX_VERSION_IS_LESS(5,3,0)
static inline int
backport_pci_disable_link_state(struct pci_dev *pdev, int state)
{
	u16 aspmc;

	pci_disable_link_state(pdev, state);

	pcie_capability_read_word(pdev, PCI_EXP_LNKCTL, &aspmc);
	if ((state & PCIE_LINK_STATE_L0S) &&
	    (aspmc & PCI_EXP_LNKCTL_ASPM_L0S))
		return -EPERM;

	if ((state & PCIE_LINK_STATE_L1) &&
	    (aspmc & PCI_EXP_LNKCTL_ASPM_L1))
		return -EPERM;

	return 0;
}
#define pci_disable_link_state LINUX_I915_BACKPORT(pci_disable_link_state)

#endif /* < 5.3 */
#endif /* defined(CONFIG_PCI) */

#ifdef BPM_PCI_REBAR_SIZE_NOT_PRESENT
u32 pci_rebar_get_possible_sizes(struct pci_dev *pdev, int bar);
static inline int pci_rebar_bytes_to_size(u64 bytes)
{
	bytes = roundup_pow_of_two(bytes);

	/* Return BAR size as defined in the resizable BAR specification */
	return max(ilog2(bytes), 20) - 20;
}
#endif /* BPM_PCI_REBAR_SIZE_NOT_PRESENT */

#ifdef BPM_STRUCT_PCI_TLP_LOG_PRESENT
#define aer_header_log_regs pcie_tlp_log
#endif /* STRUCT_PCI_TLP_LOG_PRESENT */

#endif /* _BACKPORT_LINUX_PCI_H */
