#ifndef BP_LINUX_BACKPORT_MACRO_H
#define BP_LINUX_BACKPORT_MACRO_H
#include <linux/version.h>
#include <backport/autoconf.h>

#if LINUX_VERSION_IS_LESS(6,0,0)

#if !((REDHAT_RELEASE_VERSION_IS_GEQ(8,4)) || FBK_VERSION)
/*
 * 0ade638655f0 intel-gtt: introduce drm/intel-gtt.h
 */
#define INTEL_GMCH_GTT_RENAMED
#endif
#endif

#if LINUX_VERSION_IS_LESS(5,15,0)
#if !(SUSE_RELEASE_VERSION_IS_GEQ(15,4,0))

/*
 * f0ab00174eb7 PCI: Make saved capability state private to core
 * 621f7e354fd8 PCI: Make pci_set_of_node(), etc private
 */
#define BPM_PCI_INTERFACES_NOT_PRESENT
#endif

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6))
/*
 * d19c81378829e locking/lockdep: Provide lockdep_assert{,_once}() helpers
 *
 */
#define BPM_LOCKDEP_ASSERT_API_NOT_PRESENT
#endif

#if !(UBUNTU_RELEASE_VERSION_IS_GEQ(20,04))

/*
 * bf44e8cecc03c vgaarb: don't pass a cookie to vga_client_register
 *
 */
#define BPM_VGA_SET_DECODE_ARG_PCI_DEV_NOT_PRESENT
#endif

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || \
	UBUNTU_RELEASE_VERSION_IS_GEQ(20,04))

/*
 * 90e7a6de62781 lib/scatterlist: Provide a dedicated function to support table append
 *
 */
#define BPM_SG_ALLOC_TABLE_FROM_PAGES_SEGMENT_NOT_PRESENT
#ifdef BPM_SG_ALLOC_TABLE_FROM_PAGES_SEGMENT_NOT_PRESENT
#if !(REDHAT_RELEASE_VERSION_IS_LEQ(8,4))
/*
 * 89d8589cd72c6 Introduce and export __sg_alloc_table_from_pages
 */
#define BPM_SG_ALLOC_TABLE_FROM_PAGES_RETURNS_SCATTERLIST
#endif
#endif
#endif

#if !(SUSE_RELEASE_VERSION_IS_GEQ(15,4,0) || \
	UBUNTU_RELEASE_VERSION_IS_GEQ(20,04))

#define BPM_VGA_CLIENT_UNREGISTER_NOT_PRESENT
#endif


#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || \
	SUSE_RELEASE_VERSION_IS_GEQ(15,4,0) || \
		UBUNTU_RELEASE_VERSION_IS_GEQ(20,04))

/*
 * 6f2beb268a5d swiotlb: Update is_swiotlb_active to add a struct device argument
 *
 */
#define BPM_IS_SWIOTLB_ACTIVE_ARG_DEV_NOT_PRESENT
#endif

/*
 * 1072ed3431f5ba2 drm/dp: Move panel DP AUX backlight support to
 * drm_dp_helper
 * 10f7b40e4f3050 drm/panel: add basic DP AUX backlight support
 */
#define BPM_AUX_BACKLIGHT_SUPPORT_TO_DRM_DP_NOT_PRESENT
#endif

#if LINUX_VERSION_IS_LESS(5,14,0)
/* 
 * Use kernel.h incase of math.h is not available
 * aa6159ab99a9ab kernel.h: split out mathematical helpers
 */
#define BPM_INCLUDE_KERNEL_H_IN_ASCII85_H

/* TBD: Need to check if its generic or controllable with version */
#define BPM_PTRACE_MAY_ACCESS_NOT_PRESENT

#if !LINUX_VERSION_IN_RANGE(5,10,80, 5,11,0)
/*
 * 438153d5ba7f
 * arch/cc: Introduce a function to check for confidential computing features
 */
#define BPM_CC_PLATFORM_H_NOT_PRESENT
#endif
#endif

#if LINUX_VERSION_IS_LESS(5,13,0)
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6))

/*
 * eb2dafbba8b82 tasklets: Prevent tasklet_unlock_spin_wait() deadlock on RT
 *
 */
#define BPM_TASKLET_UNLOCK_SPIN_WAIT_NOT_PRESENT
/*
 * dma-buf/dmabuf: Don't export dma_fence symbols
 *
 */
#define BPM_DMA_FENCE_PRIVATE_STUB_NOT_PRESENT

/*
 * f21ffe9f6da6d swiotlb: Expose swiotlb_nr_tlb function to modules
 *
 */
#define BPM_SWIOTLB_NR_TBL_NO_ARG_PRESENT
#endif
#if !(LINUX_VERSION_IN_RANGE(5,10,70, 5,11,0) || FBK_VERSION)
/*
 * 4f0f586bf0c8 treewide: Change list_sort to use const pointers
 *
 */
#define BPM_LIST_CMP_FUNC_T_NOT_PRESENT
#endif
#endif

#if LINUX_VERSION_IS_LESS(5,12,0)

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5))

/*
 * dma-buf/dmabuf: Don't export dma_fence symbols
 */
#define BPM_DMA_FENCE_TIMESTAMP_NOT_PRESENT

/*
 * 276b738deb5bf PCI: Add resizable BAR infrastructure
 * 192f1bf7559e8 PCI: Add pci_rebar_bytes_to_size()
 *
 */
#define BPM_PCI_REBAR_SIZE_NOT_PRESENT

/*
 * 2d24dd5798d0 rbtree: Add generic add and find helpers
 */
#define RB_FIND_NOT_PRESENT

#endif


#endif

#if LINUX_VERSION_IS_LESS(5,11,0)
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5))

/*
 * f0dbd2bd1c22c66 mm: slab: provide krealloc_array()
 *
 * Backport krealloc_array() api.
 *
 */
#define BPM_KREALLOC_ARRAY_NOT_PRESENT

/*
 * 23c887522e91 Relay: add CPU hotplug support
 *
 */
#define BPM_CONST_STRUCT_RCHAN_CALLBACKS_NOT_PRESENT
#endif
/*
 * 295992fb815e7 mm: introduce vma_set_file function v5
 *
 */
#define BPM_VMA_SET_FILE_NOT_PRESENT

/*
 * ab22dd46b60 Backport "drm/i915: Change shrink ordering to use locking
 * around unbinding" and deps
 *
 */
#define BPM_MIGHT_ALLOC_NOT_PRESENT
#endif

#if LINUX_VERSION_IS_LESS(5,10,0)


#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5))

/*
 * 4f6ec8602341e mm/vmalloc: separate put pages and flush VM flags
 *
 */
#define BPM_VM_MAP_PUT_PAGES_NOT_PRESENT

/*
 * Resolve issues of minmax.h
 */
#define BPM_LINUX_MINMAX_H_PRESENT

/*
 * 3e9a9e256b1e mm: add a vmap_pfn function
 *
 */
#define BPM_VMAP_PFN_NOT_PRESENT

/*
*48526a0f4ca2b4 genetlink: bring back per op policy
*/
#define BPM_GENL_OPS_POLICY_MEMBER_NOT_PRESENT
#endif

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6))

/*
 * 8117ab508f9c476 seqlock: seqcount_LOCKNAME_t: Introduce PREEMPT_RT support
 *
 */
#define BPM_SEQCOUNT_SEQUENCE_NOT_PRESENT
#endif

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || \
        SUSE_RELEASE_VERSION_IS_GEQ(15,3,0))

/*
 * b7b3c01b19159 mm/memremap_pages: support multiple ranges per invocation
 *
 */
#define BPM_PAGEMAP_RANGE_START_NOT_PRESENT
#endif

/*
 * 7a9f50a05843 irq_work: Cleanup
 *
 */
#define BPM_IRQ_WORK_NODE_LLIST_NOT_PRESENT

/*
 * 8af2fa888eaf0e Show slab cache occupancy for debug
 *
 */
#define BPM_KMEM_CACHE_SLABINFO_API_NOT_PRESENT

/*
 * 48e2e013dc71602 drm/i915: Expose list of clients in sysfs
 *
 */
#define BPM_SYSFS_EMIT_NOT_PRESENT
/*
 * 1967f71267742 hwmon: (core) Add support for rated attributes
 */

#define POWER1_RATED_MAX_NOT_PRESENT
#endif /* LINUX_VERSION_IS_LESS(5,10,0) */


#if LINUX_VERSION_IS_LESS(5,9,11)
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || \
	LINUX_VERSION_IN_RANGE(5,4,86, 5,5,0))
/*
 * dd8088d5a896 PM: runtime: Add pm_runtime_resume_and_get
 * to deal with usage counter
 */
#define BPM_PM_RUNTIME_RESUME_AND_GET_NOT_PRESENT
#endif

#endif

#if LINUX_VERSION_IS_LESS(5,9,0)

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || \
        SUSE_RELEASE_VERSION_IS_GEQ(15,3,0))

#define BPM_MIGRATE_VMA_PAGE_OWNER_NOT_PRESENT
#endif

/*
 * 229f5879facf96e5 Defined PTR_ALIGN_DOWN() in kernel.h
 *
 */
#define BPM_PTR_ALIGN_DOWN_NOT_PRESENT

/*
 * 12cc923f1ccc tasklet: Introduce new initialization API
 *
 */
#define BPM_TASKLET_STRUCT_CALLBACK_NOT_PRESENT

/*
 * aedcade6f4fa debugobjects: Allow debug_obj_descr to be const
 *
 */
#define BPM_DEBUG_OBJECT_ACTIVATE_NO_CONST_ARG
#endif

#if LINUX_VERSION_IS_LESS(5,8,0)

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4) || \
        SUSE_RELEASE_VERSION_IS_GEQ(15,3,0))

/*
 * 709d6d73c7561 scatterlist: add generic wrappers for iterating over sgtable objects
 *
 */
#define BPM_FOR_EACH_SGTABLE_PAGE_NOT_PRESENT

/*
 * 999a22890cb1 uaccess: Add user_read_access_begin/end and
 * user_write_access_begin/end
 *
 */
#define BPM_USER_WRITE_ACCESS_BEGIN_NOT_PRESENT

/*
 * 07e5bfe651f8 mmap locking API: add mmap_lock_is_contended()
 *
 */
#define BPM_MMAP_WRITE_LOCK_UNLOCK_NOT_PRESENT

/*
 * f5678e7f2ac3 kernel: better document the use_mm/unuse_mm API contract
 *
 */
#define BPM_KTHREAD_USE_MM_NOT_PRESET
#endif

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6))
/*
 * 479da1f538a2 backlight: Add backlight_device_get_by_name()
 */
#define BPM_BACKLIGHT_DEV_GET_BY_NAME_NOT_PRESENT

/*
 * f45ce9336ff0640 video/hdmi: Add Unpack only function for DRM infoframe
 *
 */
#define BPM_HDMI_DRM_INFOFRAME_UNPACK_NOT_PRESENT

/*
 * 9807372 capabilities: Introduce CAP_PERFMON to kernel and user space
 *
 */
#define BPM_PERFMON_CAPABLE_NOT_PRESENT
#endif

/*
 * ca5999f mm: introduce include/linux/pgtable.h
 * 64fa30f Backport and fix intel-gtt split
 *
 */
#define BPM_ASM_PGTABLE_H_NOT_PRESENT

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5) || \
        SUSE_RELEASE_VERSION_IS_GEQ(15,3,0))

/*
 * 2733ea144dcc mm/hmm: remove the customizable
 * pfn format from hmm_range_fault
 *
 */
#define BPM_HMM_RANGE_HMM_PFNS_NOT_PRESENT
#endif

#endif

#if LINUX_VERSION_IS_LESS(5,7,0)
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5))

/*
 * c111566bea7c PM: runtime: Add pm_runtime_get_if_active()
 */
#define BPM_PM_RUNTIME_GET_IF_ACTIVE_NOT_PRESENT

/*
 * be957c886d92a mm/hmm: make hmm_range_fault return 0 or -1
 *
 */
#define BPM_HMM_RANGE_FAULT_ARG_PRESENT
#endif

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || \
        SUSE_RELEASE_VERSION_IS_GEQ(15,3,0))
/*
 * c0842fbc1b18 random32: move the pseudo-random 32-bit
 * definitions to prandom.h
 */
#define BPM_PRANDOM_H_NOT_PRESENT
#endif

/*
 * 132ccc0422814203ca64 INTEL_DII: drm/i915/spi: refcount spi object lifetime
 *
 */
#define BPM_MTD_PART_NOT_PRESENT
#endif

#if LINUX_VERSION_IS_LESS(5,6,0)

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6))

/*
 * a392d26f32cdd87 include/bitmap.h: add new functions to documentation
 *
 */
#define BPM_BITMAP_CLEAR_REGION_NOT_PRESENT
#endif

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4))

/*
 * bf9e25ec1287 video: fbdev: make fbops member of struct fb_info a const pointer
 *
 */
#define BPM_PIN_USER_PAGES_FAST_NOT_PRESENT
#endif

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4) || \
        SUSE_RELEASE_VERSION_IS_GEQ(15,3,0))

/*
 * b6ff753a0ca0d drm: constify fb ops across all drivers
 *
 */
#define BPM_CONST_STRUCT_FB_OPS_NOT_PRESENT
#endif


/*
 * e692b4021a2e4 lockdep: add might_lock_nested()
 *
 */
#define BPM_MIGHT_LOCK_NESTED_NOT_PRESENT

#endif

#if LINUX_VERSION_IS_LESS(5,5,0)
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4) || \
	SUSE_RELEASE_VERSION_IS_GEQ(15,3,0))

/*
 * a63fc6b75cca9 rcu: Upgrade rcu_swap_protected() to rcu_replace_pointer()
 *
 */
#define BPM_RCU_REPLACE_POINTER_NOT_PRESENT
#endif

#if (REDHAT_RELEASE_VERSION_IS_GEQ(8,5) || \
        SUSE_RELEASE_VERSION_IS_GEQ(15,3,0))

/*
 * 99cb252f5e68d72  mm/mmu_notifier: add an interval tree notifier
 *
 */
#define BPM_HMM_RANGE_NOTIFIER_NOT_PRESENT
#endif

/*
 * 8c2a2b8c2ff68 nvmem: core: add nvmem_device_find
 *
 */
#define BPM_NVMEM_DEVICE_FIND_NOT_PRESENT
#endif

#if LINUX_VERSION_IS_LESS(5,4,0)

/*
 * 12c88d840b45 module: add support for symbol namespaces (jsc#SLE-10158).
 *
 */
#define BPM_MODULE_IMPORT_NS_NOT_PRESENT
#endif

#if LINUX_VERSION_IS_LESS(5,3,0)
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5))

/*
 * d2a8ebbf8192b kernel.h: split out container_of() and typeof_member() macros
 *
 */
#define BPM_TYPEOF_MEMBER_NOT_PRESENT
#endif

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4))

/*
 * 5213d7efc8ec2 i2c: acpi: export i2c_acpi_find_adapter_by_handle
 *
 */
#define BPM_I2C_ACPI_FIND_ADAPTER_BY_HANDLE_EXPORT_NOT_PRESENT
#endif
#endif

#if LINUX_VERSION_IS_LESS(5,2,0) && \
	!(REDHAT_RELEASE_VERSION_IS_GEQ(8,4) || \
        SUSE_RELEASE_VERSION_IS_GEQ(15,3,0))

/*
 * a49294eac27c7 Add wait_var_event_interruptible()
 *
 */
#define BPM_WAIT_VAR_EVENT_INTERRUPTIBLE_NOT_PRESENT
#endif

#if LINUX_VERSION_IS_LESS(5,0,0) && \
	!(REDHAT_RELEASE_VERSION_IS_GEQ(8,4))

/*
 * b33a02aadcc63 i2c: acpi: Move I2C bits from acpi.h to i2c.h
 *
 */
#define BPM_I2C_ACPI_GET_I2C_RESOURCE_NOT_PRESENT
#endif

#if LINUX_VERSION_IS_LESS(4,20,0)
/*
 * a3f8a30f3f00 Compiler Attributes: use feature
 * checks instead of version checks
 */

#define BPM_COMPILER_ATTRIBUTES_HEADER_NOT_PRESENT
#endif

#if LINUX_VERSION_IS_LESS(4,19,0)

/*
 * 14d32b2 Defined jiffies_delta_to_msecs() function
 *
 */
#define BPM_JIFFIES_DELTA_TO_MSECS_NOT_PRESENT
#endif

/*
 * REDHAT
 */
#if REDHAT_RELEASE_VERSION_IS_RANGE(8,4, 9,0)
#define BPM_RH_DRM_BACKPORT_MMU_NOTIFIER_WRAPPER
#ifdef BPM_RH_DRM_BACKPORT_MMU_NOTIFIER_WRAPPER
#if REDHAT_RELEASE_VERSION_IS_LEQ(8,5)
#define BPM_RH_DRM_BACKPORT_MMU_NOTIFIER_WRAPPER_1
#elif REDHAT_RELEASE_VERSION_IS_GEQ(8,6)
#define BPM_RH_DRM_BACKPORT_MMU_NOTIFIER_WRAPPER_2
#endif
#endif
#endif

#if (REDHAT_RELEASE_VERSION_IS_RANGE(8,4, 9,0) || FBK_VERSION)
/* TBD : Need to check further need of ATTR Macro */
#define BPM_DEVICE_ATTR_NOT_PRESENT
#endif

#if REDHAT_RELEASE_VERSION_IS_RANGE(8,4, 9,0)
#define BPM_DRM_GET_PANEL_ORIENTATION_QUIRK_DONT_EXPORT
#endif

#if SUSE_RELEASE_VERSION_IS_GEQ(15,3,0)
#define BPM_DRM_GET_PANEL_ORIENTATION_QUIRK_RENAME
#endif

#if (REDHAT_RELEASE_VERSION_IS_RANGE(8,4, 9,0) || \
        SUSE_RELEASE_VERSION_IS_GEQ(15,3,0))

#define BPM_BP_MTD_MAGIC_NUMBER
#define BPM_PRELIM_OVERRIDE_P2P_DIST_DEFAULT_ENABLE
#define BPM_INCLUDE_KERNEL_H_IN_ASCII85_H
#endif
/* upstream changes not landed in mainline kernel yet.
 *
 * c1a01f290103d drm: constify sysrq_key_op
 */
#define BPM_CONST_SYSRQ_KEY_OP_NOT_PRESENT

/* upstream changes not landed in mainline kernel yet.
 * 9299148acf5422 VFIO - SR-IOV VF migration
 */
#define BPM_VFIO_SR_IOV_VF_MIGRATION_NOT_PRESENT

/*
 * MISCELLANEOUS
 */

/* Add backport Macro for all the symbols of dma-buf. */

#define BPM_ADD_BACKPORT_MACRO_TO_DMA_BUF_SYMBOLS

/* Resolve issues of dma-buf and add to compat module */

#define BPM_DMA_BUF_MOVE_FOPS_TO_DENTRY_OPS

#define BPM_LOWMEM_FOR_DG1_NOT_SUPPORTED

#define BPM_INTEL_MEI_PXP_GSC_ASSUME_ALWAYS_ENABLED
#define BPM_INTEL_VSEC_ASSUME_ALWAYS_ENABLED
#define BPM_TRACE_INCLUDE_PATH_NOT_PRESENT
#define BPM_ADD_DEBUG_PRINTS_BKPT_MOD
#define BPM_ADD_MODULE_VERSION_MACRO_IN_ALL_MOD

#endif /* BP_LINUX_BACKPORT_MACRO_H */
