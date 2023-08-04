#ifndef _BP_LINUX_BACKPORT_MACRO_H
#define _BP_LINUX_BACKPORT_MACRO_H
#include <linux/version.h>

#if LINUX_VERSION_IS_LESS(5,15,46)

/*
 * 0425473037db list: introduce list_is_head() helper and re-use it in list.h
 */
#define BPM_LIST_IS_HEAD_NOT_PRESENT 

#endif 

#if LINUX_VERSION_IS_LESS(5,15,0)

/*
 * f0ab00174eb7 PCI: Make saved capability state private to core
 * 621f7e354fd8 PCI: Make pci_set_of_node(), etc private
 */
#define PCI_INTERFACES_NOT_PRESENT

/*
 * Add macro to export pci_find_host_bridge()
 * 59dc33252ee7 PCI: VMD: ACPI: Make ACPI companion lookup work for VMD bus
 */
#define BPM_PCI_FIND_HOST_BRIDGE_NOT_EXPORTED

#endif

#if LINUX_VERSION_IS_LESS(5,13,0)

/* 
 * 3e31f94752e4 lockdep: Add lockdep_assert_not_held()
 */
#define BPM_LOCKDEP_ASSERT_NOT_HELD_NOT_PRESENT
#endif

#if LINUX_VERSION_IS_LESS(5,12,0)

/*
 * 2d24dd5798d0 rbtree: Add generic add and find helpers
 */
#define RB_FIND_NOT_PRESENT

/*
 * 97a7e4733b9b mm: introduce page_needs_cow_for_dma() for deciding whether cow
 */
#define BPM_IS_COW_MAPPING_NOT_PRESENT

#endif

#if LINUX_VERSION_IS_LESS(5,11,0)

/*
 *  aa6159ab99a9ab kernel.h: split out mathematical helpers
 */
#define BPM_MATH_H_NOT_PRESENT
#endif

#if LINUX_VERSION_IS_LESS(5,10,0)

/*
 * 1967f71267742 hwmon: (core) Add support for rated attributes
 */
#define POWER1_RATED_MAX_NOT_PRESENT

#endif /* LINUX_VERSION_IS_LESS(5,10,0) */

#if LINUX_VERSION_IS_LESS(5,8,0)

/*
 * 479da1f538a2 backlight: Add backlight_device_get_by_name()
 */
#define BACKLIGHT_DEV_GET_BY_NAME_NOT_PRESENT

/*
 * 42fc541404f2 mmap locking API: add mmap_assert_locked() and mmap_assert_write_locked()
 */
#define BPM_MMAP_ASSERT_LOCKED_NOT_PRESENT

#endif

#if LINUX_VERSION_IS_LESS(5,6,0)

/*
 * f1f6a7dd9b53 mm, tree-wide: rename put_user_page*() to unpin_user_page*()
 */
#define BPM_PIN_OR_UNPIN_USER_PAGE_NOT_PRESENT

#endif

/*
 * 64fa30f9ffc0ed Backport and fix intel-gtt split
 * intl_gtt Api name has been changed to intel_gmch_gtt
 * so remapping names to older API via backported header.
 */

#define INTEL_GMCH_GTT_RENAMED

/*
 * Disable Lowmem reservation for dg1
 */
#define BPC_LOWMEM_FOR_DG1_NOT_SUPPORTED

/*
 * 7c87cfa503ad04 drm/i915/pvc: Enable rc6 by default
 * Disable RC6 support for SLES15SP3
 */
#define RC6_NOT_SUPPORTED 

/*
 * upstream changes not landed in mainline kernel yet
 * Introduced in DII_6042
 * 9299148acf5422 VFIO - SR-IOV VF migration
 */
#define BPM_VFIO_SR_IOV_VF_MIGRATION_NOT_PRESENT

#endif /* _BP_LINUX_BACKPORT_MACRO_H */

