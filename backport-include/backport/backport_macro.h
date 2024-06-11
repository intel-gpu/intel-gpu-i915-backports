#ifndef _BP_LINUX_BACKPORT_MACRO_H
#define _BP_LINUX_BACKPORT_MACRO_H
#include <linux/version.h>
#include <linux/kconfig.h>
#include <backport/autoconf.h>

#if LINUX_VERSION_IS_GEQ(6,8,2) || \
	LINUX_VERSION_IN_RANGE(6,6,23, 6,7,0) || LINUX_VERSION_IN_RANGE(6,1,83, 6,2,0) || \
	LINUX_VERSION_IN_RANGE(5,15,153, 5,16,0) || LINUX_VERSION_IN_RANGE(5,10,214, 5,11,0) || \
	(LINUX_VERSION_IN_RANGE(5,15,0, 5,16,0) && UBUNTU_RELEASE_VERSION_IS_GEQ(111,121)) || \
        (SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0) && SUSE_LOCAL_VERSION_IS_GEQ(55,59))
/*
 * e33ee8d5e6fc PCI: Make pci_dev_is_disconnected() helper public for other drivers
 */
#define BPM_PCI_DEV_IS_DISCONNECTED_NOT_PRESENT
#endif

#if LINUX_VERSION_IS_GEQ(6,8,0)
/*
 * 8eb80946ab0c drm/edid: split out drm_eld.h from drm_edid.h
 */
#define BPM_DRM_ELD_H_PRESENT

/*
 * e435ca878821 mm: remove inc/dec lruvec page state functions
 */
#define BPM_INC_DEC_LRUVEC_PAGE_STATE_PRESENT
#endif

#if LINUX_VERSION_IS_LESS(6,8,0)
/*
 * 19975f83412f mm/slab: move the rest of slub_def.h to mm/slab.h
 */
#define BPM_SLUB_DEF_IS_PRESENT
#endif

#if LINUX_VERSION_IS_GEQ(6,7,0)
/*
 *451921e7bbc7: drm: Replace drm_framebuffer plane size
 *              functions with its equivalents
 */
#define BPM_DRM_FRAMEBUFFER_PLANE_HEIGHT_NOT_PRESENT

/*
 * 0ede61d8589c file: convert to SLAB_TYPESAFE_BY_RCU
 */
#define BPM_GET_FILE_RCU_ARG_CHANGED

/*
 * e2272bfb18ee: drm/dp: switch drm_dp_downstream_*() helpers to struct drm_edid
 */
#define BPM_STRUCT_EDID_NOT_PRESENT

/*
 * 07f9cfe2ef6c: drm/i915/dp_mst: Make sure pbn_div is up-to-date after sink reconnect
 */
#define BPM_MST_STATE_PBN_DIVE_PRESENT

/*
 * 5aa1dfcdf0a4: drm/mst: Refactor the flow for payload allocation/removement
 */
#define BPM_DRM_DP_REMOVE_PAYLOAD_NOT_PRESENT

/*
 *f2383e01507e mm: shrinker: remove old APIs
 */
#define BPM_REGISTER_SHRINKER_NOT_PRESENT

/*
 *e965a7072767 drm: remove I2C_CLASS_DDC support
 */
#define BPM_I2C_CLASS_DDC_PRESENT
#endif

#if LINUX_VERSION_IS_GEQ(6,6,0)
/*
 * 46f12960aad2 drm/i915: Move abs_diff() to math.h
 */
#define BPM_ABS_DIFF_PRESENT
/*
 * 7ec4b34be423 PCI/AER: Unexport pci_enable_pcie_error_reporting()
 */
#define BPM_PCI_ENABLE_DISABLE_PCIE_ERROR_NOT_EXPORTED

/*
 * 6f2beb268a5 swiotlb: Update is_swiotlb_active to add a struct device argument
 */
#define BPM_IS_SWIOTLB_ACTIVE_PRESENT
/*
 * 8ac20a03da56 tty: sysrq: switch the rest of keys to u8
 */
#define BPM_SYSRQ_KEY_OP_HANDLER_INT_ARG_NOT_PRESENT
/*
 * 49f776724e64 PCI/AER: Export pcie_aer_is_native()
 */
#define BPM_MODULE_IMPORT_NS_CXL_SUPPORT
#endif /* LINUX_VERSION_IS_GEQ(6,6,0) */

#if (LINUX_VERSION_IS_GEQ(6,6,0) || \
	(LINUX_VERSION_IS_GEQ(6,5,0) && ((UBUNTU_BACKPORT_VERSION_IS_GEQ(34,34) && \
	 UBUNTU_BACKPORT_VERSION_IS_LESS(35,35)) || UBUNTU_BACKPORT_VERSION_IS_GEQ(41,41) )))
/*
 * 4e042f022255 drm/dp_mst: Fix fractional DSC bpp handling
 */
#define BPM_DRM_DP_CALC_PBN_MODE_ARG_PRESENT
#endif

#if LINUX_VERSION_IS_GEQ(6,5,0)
/*
 * 6801be4f2653 slub: Replace cmpxchg_double
 */
#define BPM_FREELIST_ABA_T_NOT_PRESENT

/*
 * 3d35ddfb0713 drm/display/dp_mst: drop has_audio from struct drm_dp_mst_port
 */
#define BPM_PORT_HAS_AUDIO_MEMBER_NOT_PRESENT

/*
 * c265f340eaa8
 * drm/connector: Allow drivers to pass list of supported colorspaces
 */
#define BPM_SUPPORTED_COLORSPACES_ARG_NOT_PRESENT

/*
 * e5a1fd997cc2 i915: simplify subdirectory registration with register_sysctl
 */
#define BPM_REGISTER_SYSCTL_TABLE_NOT_PRESENT

/*
 * 1e0877d58b1e mm: remove struct pagevec
 */
#define BPM_PAGEVEC_NOT_PRESENT

/*
 * e0b72c14d8dc mm: remove check_move_unevictable_pages()
 */
#define BPM_CHECK_MOVE_UNEVICTABLE_PAGES_NOT_PRESENT

#endif /* LINUX_VERSION_IS_GEQ(6,5,0) */

#if (LINUX_VERSION_IS_GEQ(6,4,5) || \
	LINUX_VERSION_IN_RANGE(6,1,42, 6,2,0) || \
	(LINUX_VERSION_IN_RANGE(6,2,16, 6,3,0) && UBUNTU_RELEASE_VERSION_IS_GEQ(36,37)) || \
	(SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0) && SUSE_LOCAL_VERSION_IS_GEQ(55,19)) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,3))
/*
 * 104d79eb58aa drm/dp_mst: Clear MSG_RDY flag before sending new message
 */
#define BPM_DRM_DP_MST_HPD_IRQ_IS_NOT_PRESENT
#endif /* (LINUX_VERSION_IS_GEQ(6,4,5) || LINUX_VERSION_IN_RANGE(6,1,42, 6,2,0) ... */

#if LINUX_VERSION_IS_GEQ(6,4,0)
/*
 * 1fb1ea0d9cb8 mei: Move uuid.h to the MEI namespace
 */
#define BPM_UUID_H_NOT_PRESET

/*
 * 6e30a66433af class: remove struct module owner out of struct class
 */
#define BPM_STRUCT_CLASS_OWNER_MEMBER_NOT_PRESENT

/*
 * 1aaba11da9aa driver core: class: remove module * from class_create()
 */
#define BPM_THIS_MODULE_ARG_NOT_PRESENT

/*
 * 5d844091f237 drm/scdc-helper: Pimp SCDC debugs
 */
#define BPM_I2C_ADAPTER_ARG_NOT_PRESENT

#endif /* LINUX_VERSION_IS_GEQ(6,4,0) */

#if (LINUX_VERSION_IS_GEQ(6,4,0) || LINUX_VERSION_IS_LESS(5,5,0))
#if !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0))
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4))
/*
 * fa83433c92e3 iommu: Add I/O ASID allocator
 * 99b5726b4423 iommu: Remove ioasid infrastructure
 */
#define BPM_IOASID_H_NOT_PRESENT
#endif
#endif
#endif

#if LINUX_VERSION_IS_GEQ(6,3,0)
/*
 * f5b3c341a46e mei: Move uuid_le_cmp() to its only user
 */
#define BPM_UUID_LE_CMP_NOT_PRESENT

/*
 * 2a81ada32f0e driver core: make struct bus_type.uevent() take a const *
 */
#define BPM_UEVENT_STRUCT_DEVICE_CONST_ARG_NOT_PRESENT

/*
 * 1c71222e5f23
 * mm: replace vma->vm_flags direct modifications with modifier calls
 */
#define BPM_VM_FLAGS_IS_READ_ONLY_FLAG

/*
 * 5e6a51787fef uuid: Decouple guid_t and uuid_le types and respective macros
 */
#define BPM_GUID_INIT_NOT_EXPORTED

#endif /*LINUX_VERSION_IS_GEQ(6,3,0) */

#if (LINUX_VERSION_IS_GEQ(6,3,0) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,3))
/*
 * a3185f91d057 drm/ttm: merge ttm_bo_api.h and ttm_bo_driver.h v2
 */
#define BPM_TTM_BO_API_H_NOT_PRESENT

/*
 * 80ed86d4b6d7 drm/connector: Rename drm_mode_create_tv_properties
 */
#define BPM_DRM_MODE_CREATE_TV_PROP_NOT_PRESENT

/*
 * 6c80a93be62d
 * drm/fb-helper: Initialize fb-helper's preferred BPP in prepare function
 */
#define BPM_DRM_FB_PREPARE_AND_INITIAL_CFG_NOT_PRESENT
#endif /* (LINUX_VERSION_IS_GEQ(6,3,0) || REDHAT_RELEASE_VERSION_IS_GEQ(9,3)) */

#if (LINUX_VERSION_IS_GEQ(6,3,0) || \
	LINUX_VERSION_IS_LESS(4,10,0) || \
	REDHAT_RELEASE_VERSION_IS_EQL(8,9) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,3))
/*
 * 5e7b9a6ae8c3 swiotlb: remove swiotlb_max_segment
 */
#define BPM_SWIOTLB_MAX_SEGMENT_NOT_PRESENT
#endif /* (LINUX_VERSION_IS_GEQ(6,3,0) || LINUX_VERSION_IS_LESS(4,10,0)... */

#if (LINUX_VERSION_IS_GEQ(6,2,0) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,3))
/*
 * afb0ff78c13c51
 * drm/fb-helper: Rename drm_fb_helper_unregister_fbi() to use _info postfix
 */
#define BPM_DRM_FB_HELPER_ALLOC_UNREGISTER_FBI_NOT_PRESENT

/*
 * 90b575f52c6
 * drm/edid: detach debugfs EDID override from EDID property update
 */
#define BPM_STRUCT_DRM_CONNECTOR_OVERRIDE_EDID_NOT_PRESENT

/*
 * 9877d8f6bc
 * drm/fb_helper: Rename field fbdev to info in struct drm_fb_helper
 */
#define BPM_STRUCT_DRM_FB_HELPER_FBDEV_NOT_PRESENT
#endif /* (LINUX_VERSION_IS_GEQ(6,2,0) || REDHAT_RELEASE_VERSION_IS_GEQ(9,3)) */

#if (LINUX_VERSION_IS_GEQ(6,2,0) || \
                (REDHAT_RELEASE_VERSION_IS_GEQ(8,9)))

#if!(REDHAT_RELEASE_VERSION_IS_EQL(9,0))
/*
 * 9a758d8756da drm: Move nomodeset kernel parameter to drivers/video
 */
#define BPM_VIDEO_FIRMWARE_DRIVERS_ONLY_NOT_EXPORTED
#endif /* !(REDHAT_RELEASE_VERSION_IS_EQL(9,0)) */

/*
 * ff62b8e6588fb driver core: make struct class.devnode() take a const *
 */
#define BPM_DMA_HEAP_AND_DRM_DEVNODE_CONST_ARG_NOT_PRESENT
#endif /* (LINUX_VERSION_IS_GEQ(6,2,0) || (REDHAT_RELEASE_VERSION_IS_GEQ(8,9))) */

#if (LINUX_VERSION_IS_GEQ(6,2,0))
/*
 * 4b21d25bf519c9
 *  overflow: Introduce overflows_type() and castable_to_type()
 */
#define BPM_OVERFLOWS_TYPE_AVAILABLE

/*
 * 3c202d14a9d73
 * prandom: remove prandom_u32_max()
 */
#define BPM_PRANDOM_U32_MAX_NOT_PRESENT
#endif /*LINUX_VERSION_IS_GEQ(6,2,0)*/

#if LINUX_VERSION_IS_GEQ(6,2,0)
/*
 * 6e1ca48d0669b
 * folio-compat: remove lru_cache_add()
 */
#define BPM_LRU_CACHE_ADD_WRAPPER_NOT_PRESENT
#endif /*LINUX_VERSION_IS_GEQ(6,2,0) || LINUX_VERSION_IN_RANGE(5,4,0, 5,5,0) */

#if (LINUX_VERSION_IS_GEQ(6,1,0) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,3))
/*
 * cce32e4e38c6
 * drm/atomic-helper: Remove _HELPER_ infix from DRM_PLANE_HELPER_NO_SCALING
 */
#define BPM_DRM_PLANE_HELPER_NO_SCALING_NOT_PRESENT
#endif /* (LINUX_VERSION_IS_GEQ(6,1,0) || REDHAT_RELEASE_VERSION_IS_GEQ(9,3)) */

#if (LINUX_VERSION_IS_GEQ(6,1,0) || \
	(SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0) && SUSE_LOCAL_VERSION_IS_GEQ(55,7)) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,3))
/*
 * 4d07b0bc40
 * drm/display/dp_mst: Move all payload info into the atomic state
 */
#define BPM_DRM_DP_MST_PORT_VCPI_NOT_PRESENT
#endif /* LINUX_VERSION_IS_GEQ(6,1,0) || (SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0) ... */

#if (LINUX_VERSION_IS_GEQ(6,1,0))
/*
 * de492c83cae prandom: remove unused functions
 */
#define BPM_GET_RANDOM_INT_NOT_PRESENT

/*
 * f683b9d61319 i915: use the VMA iterator
 */
#define BPM_STRUCT_VM_AREA_STRUCT_VM_NEXT_NOT_PRESENT
#endif /* LINUX_VERSION_IS_GEQ(6,1,0) */

#if (LINUX_VERSION_IS_GEQ(6,1,0) || \
	(REDHAT_RELEASE_VERSION_IS_GEQ(9,3)))
/*
 * 3cea8d4753 lib: add find_nth{,_and,_andnot}_bit()
 */
#define BPM_FIND_NTH_BIT_PRESENT
#endif /* (LINUX_VERSION_IS_GEQ(6,1,0)) || (REDHAT_RELEASE_VERSION_IS_GEQ(9,3)) */

#if (LINUX_VERSION_IN_RANGE(6,0,0, 6,7,0) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,3))
/*
 * e33c267ab70d
 * mm: shrinkers: provide shrinkers with names
 */
#define BPM_REGISTER_SHRINKER_SECOND_ARG_NOT_PRESENT
#endif /* (LINUX_VERSION_IS_GEQ(6,0,0) || REDHAT_RELEASE_VERSION_IS_GEQ(9,3)) */

#if (LINUX_VERSION_IS_GEQ(6,0,0) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,2) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0))
/*
 * 2585a2790e7f
 * iommu/vt-d: Move include/linux/intel-iommu.h under iommu
 */
#define BPM_INTEL_IOMMU_H_NOT_PRESENT

/*
 * 720cf96d8fec drm: Drop drm_framebuffer.h from drm_crtc.h
 */
#define BPM_DRM_FRAMEBUFFER_NOT_INCLUDED_IN_DRM_CRTC_H

/*
 * 90bb087f6674 drm: Drop drm_blend.h from drm_crtc.h
 */
#define BPM_DRM_BLEND_H_NOT_INCLUDED_IN_DRM_CRTC_H

/*
 * 255490f9150d drm: Drop drm_edid.h from drm_crtc.h
 */
#define BPM_DRM_EDID_NOT_INCLUDED_IN_DRM_CRTC_H

/*
 * 14da21cc4671 drm/i915: axe lots of unnecessary includes from i915_drv.h
 * 73289afe0361 drm: Remove linux/fb.h from drm_crtc.h
 */
#define BPM_BACKLIGHT_H_NOT_INCLUDED_IN_DRM_CRTC_H
#endif /* LINUX_VERSION_IS_GEQ(6,0,0) || REDHAT_RELEASE_VERSION_IS_GEQ(9,2) ... */

#if (LINUX_VERSION_IS_GEQ(5,19,0) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,3))
/*
 * 84a1041c60ff fs: Remove pagecache_write_begin() and pagecache_write_end()
 */
#define BPM_PAGECACHE_WRITE_BEGIN_AND_END_NOT_PRESENT

/*
 * 68189fef88c7 fs: Change try_to_free_buffers() to take a folio
 */
#define BPM_CANCEL_DIRTY_PAGE_NOT_PRESENT
#endif /* (LINUX_VERSION_IS_GEQ(5,19,0) || REDHAT_RELEASE_VERSION_IS_GEQ(9,3)) */

#if (LINUX_VERSION_IS_GEQ(5,19,0) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,2) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0))
/*
 * da68386d9edb1f57a drm: Rename dp/ to display/
 */
#define BPM_DRM_DP_HELPER_DIR_DISPLAY_PRESENT

/*
 * 912ff2ebd695 drm/i915: use the new iterator in i915_gem_busy_ioctl v2
 */
#define BPM_DMA_RESV_ITER_BEGIN_PRESENT

/*
 * 6a99099fe1d6 drm/display: Move HDCP helpers into display-helper module
 */
#define BPM_DISPLAY_DRM_HDCP_PRESENT

/*
 * 2a64b147350f drm/display: Move DSC header and helpers into display-helper module
 */
#define BPM_DISPLAY_DRM_DSC_PRESENT

/*
 * 73511edf8b19 dma-buf: specify usage while adding fences to dma_resv obj v7
 * 842d9346b2fd drm/i915: Individualize fences before adding to dma_resv obj
 */
#define BPM_DMA_RESV_ADD_EXCL_FENCE_NOT_PRESENT

/*
 * c8d4c18bfbc4 dma-buf/drivers: make reserving a shared slot mandatory v4
 */
#define BPM_DMA_RESV_RESERVE_SHARED_NOT_PRESENT

/*
 * 644edf52b630 drm/display: Move SCDC helpers into display-helper library
 */
#define BPM_DISPLAY_DRM_SCDC_HELPER_PRESENT

/*
 * 657586e474bd drm/i915: Add a DP1.2 compatible way to read LTTPR capabilities
 */
#define BPM_DP_READ_LTTPR_CAPS_DPCD_ARG_NOT_PRESENT

/*
 * 4fc8cb47fcfd drm/display: Move HDMI helpers into display-helper module
 */
#define BPM_DISPLAY_DRM_HDMI_HELPER_PRESENT
#endif /* LINUX_VERSION_IS_GEQ(5,19,0) || REDHAT_RELEASE_VERSION_IS_GEQ(9,2) ... */

#if (LINUX_VERSION_IS_GEQ(5,19,0) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,2))
/*
 * c4f135d64382 workqueue: Wrap flush_workqueue() using a macro
 */
#define BPM_FLUSH_WQ_WITH_WARN_WRAPPER_PRESENT
#endif /* LINUX_VERSION_IS_GEQ(5,19,0) || REDHAT_RELEASE_VERSION_IS_GEQ(9,2) */

#if LINUX_VERSION_IS_GEQ(5,19,0)
/*
 * 7bc80a5462c3 dma-buf: add enum dma_resv_usage v4
 */
#define BPM_DMA_RESV_TEST_SIGNALED_BOOLEAN_ARG_NOT_PRESENT

#elif (LINUX_VERSION_IN_RANGE(5,18,0, 5,19,0) || \
	REDHAT_RELEASE_VERSION_IS_EQL(9,1))

/*
 * 5b529e8d9c387a34 drm/dp: Move public DisplayPort headers into dp/
 */
#define BPM_DRM_DP_HELPER_DIR_DP_PRESENT
#endif /* LINUX_VERSION_IS_GEQ(5,19,0) */

#if LINUX_VERSION_IS_LESS(5,19,0)
/*
 * 0192c25c03cd2f drm/dp: add 128b/132b link status helpers from DP 2.0 E11
 */
#define BPM_DRM_DP_128B132B_API_NOT_PRESENT

/*
 * 6a99099 drm/display: Move HDCP helpers into display-helper module
 */
#define BPM_HDCP_HELPERS_NOT_IN_DISPLAY_DIRECTORY

#if (SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0))
/*
 * 4dea97f8636d
 * lib/bitmap: change type of bitmap_weight to unsigned long
 */
#define BPM_BITMAP_WEIGHT_RETURN_TYPE_CHANGED
#endif
#endif /* LINUX_VERSION_IS_LESS(5,19,0) */


#if (LINUX_VERSION_IS_GEQ(5,18,0) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,1) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0))
/*
 * 4a46e5d251a39e7c10
 * drm/edid: Rename drm_hdmi_avi_infoframe_colorspace to _colorimetry
 */
#define BPM_DRM_HDMI_AVI_INFOFRAME_COLORSPACE_NOT_PRESENT
#endif /* LINUX_VERSION_IS_GEQ(5,18,0) || REDHAT_RELEASE_VERSION_IS_GEQ(9,1) ... */

#if (LINUX_VERSION_IS_GEQ(5,18,0) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,1) || \
	LINUX_VERSION_IS_LESS(5,14,0) && !(IS_ENABLED(CPTCFG_BUILD_I915)))
/*
 * 7938f4218168ae9f
 * dma-buf-map: Rename to iosys-map
 */
#define BPM_IOSYS_MAP_PRESENT
#endif /* LINUX_VERSION_IS_GEQ(5,18,0) || REDHAT_RELEASE_VERSION_IS_GEQ(9,1) ... */

#if LINUX_VERSION_IS_GEQ(5,18,0) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0)
/*
 * 730ff52194cdb324
 * mm: remove pointless includes from <linux/hmm.h>
 */
#define BPM_MIGRATE_AND_MEMREMAP_NOT_PRESENT
#endif /* LINUX_VERSION_IS_GEQ(5,18,0) || SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0) */

#if LINUX_VERSION_IS_GEQ(5,18,0)
/*
 * 7968778914e53788a
 * PCI: Remove the deprecated "pci-dma-compat.h" API
 */
#define BPM_PCI_DMA_COMPAT_H_NOT_PRESENT
#endif /* LINUX_VERSION_IS_GEQ(5,18,0) */

#if (LINUX_VERSION_IS_LESS(5,17,2) && \
	!((LINUX_VERSION_IN_RANGE(5,17,0, 5,17,2) && UBUNTU_RELEASE_VERSION_IS_GEQ(1004,4)) || \
	LINUX_VERSION_IN_RANGE(5,15,33, 5,16,0) || LINUX_VERSION_IN_RANGE(5,4,0, 5,5,0) || \
	(LINUX_VERSION_IN_RANGE(5,14,0, 5,15,0) && UBUNTU_RELEASE_VERSION_IS_GEQ(1035,38)) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,1) || (REDHAT_RELEASE_VERSION_IS_RANGE(8,2, 8,9) && !(IS_ENABLED(CPTCFG_BUILD_I915)))|| \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0) || CUSTOM_KERN_1_RELEASE_VERSION_IS_GEQ(8,6656) || \
	LINUX_VERSION_IN_RANGE(5,10,0, 5,11,0)))
/*
 * 662b372a8a72695d drm/edid: Split deep color modes between RGB and YUV444
 *
 * Introduced in 5.17.2 and backported to LTS kernel 5.15.33 as well as
 * backported in Ubuntu oem 5.17.0-1004.4 and 5.14.0-1035.38.
 */
#define BPM_EDID_HDMI_RGB444_DC_MODES_NOT_PRESENT
#endif  /* LINUX_VERSION_IS_GEQ(5,17,2) || (LINUX_VERSION_IN_RANGE(5,17,0, 5,17,2) && UBUN */


#if (LINUX_VERSION_IS_LESS(5,18,0) && \
	!(REDHAT_RELEASE_VERSION_IS_GEQ(9,1) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0)))
#if (LINUX_VERSION_IS_GEQ(5,14,0) || IS_ENABLED(CPTCFG_BUILD_I915))
/*
 * 398d06216ff27b7 iosys-map: Add offset to iosys_map_memcpy_to()
 *
 */
#define BPM_IOSYS_MAP_MEMCPY_TO_ARG_OFFSET_ADDED
/*
 * 210d0b65d94f5f iosys-map: Add a few more helpers
 *
 */
#define BPM_IOSYS_MAP_FEW_MORE_HELPER_APIS
#endif
#endif

#if (LINUX_VERSION_IN_RANGE(5,17,0, 5,19,0) || \
	REDHAT_RELEASE_VERSION_IS_EQL(9,1))
#define BPM_DMA_RESV_EXCL_UNLOCKED_NOT_PRESENT
#endif /* LINUX_VERSION_IN_RANGE(5,17,0, 5,19,0) || REDHAT_RELEASE_VERSION_IS_EQL(9,1) */

#if (LINUX_VERSION_IS_GEQ(5,17,0) || \
	REDHAT_RELEASE_VERSION_IS_RANGE(8,7, 8,8) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,1) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0))
/*
 * 6a2d2ddf2c345e0
 * drm: Move nomodeset kernel parameter to the DRM subsystem
 */
#define BPM_VGACON_TEXT_FORCE_NOT_PRESENT
#endif /* LINUX_VERSION_IS_GEQ(5,17,0) || REDHAT_RELEASE_VERSION_IS_GEQ(9,1)... */

#if (LINUX_VERSION_IS_GEQ(5,17,0) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,2))
/*
 * d122019bf061c mm: Split slab into its own type
 */
#define BPM_FOLIO_ADDRESS_PRESENT

/*
 * bb192ed9aa719 mm/slub: Convert most struct page to struct slab by spatch
 */
#define BPM_COUNT_STRUCT_SLAB_PRESENT

/*
 * ec288a2cf7ca40a9 bitmap: unify find_bit operations
 */
#define BPM_BITMAP_FOR_REGION_NOT_PRESENT
#endif /* LINUX_VERSION_IS_GEQ(5,17,0) || REDHAT_RELEASE_VERSION_IS_GEQ(9,2) */

#if LINUX_VERSION_IS_GEQ(5,17,0)
/*
 * 502fee2499277c
 * drm/i915/dp: Use the drm helpers for getting max FRL rate.
 */
#define BPM_MAX_FLR_NOT_PRESENT
#endif /* LINUX_VERSION_IS_GEQ(5,17,0) */

#if LINUX_VERSION_IS_LESS(5,17,0)
/*
 * 2d8b5b3b9e40f7 drm/i915/dp: use new link training delay helpers
 *
 * Required DRM changes are not present in KV < 5.17 so modified code
 * to follow previous implementation.
 */
#define BPM_DP_LINK_TRAINING_CR_DELAY_PRESENT

/*
 * f58a435311672
 * drm/dp, drm/i915: Add support for VESA backlights using PWM for brightness control
 */
#define BPM_DRM_EDP_BACKLIGHT_NOT_PRESENT

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(9,2))
/*
 * 781b2ba6eb5f2 SLUB: Out-of-memory diagnostics
 */
#define BPM_COUNT_STRUCT_PAGE_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(9,2)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(9,1))
/*
 * 9dd3d069406c mm/filemap: Add filemap_add_folio()
 */
#define BPM_ADD_PAGE_CACHE_LOCKED_NOT_PRESENT
/*
 * 97cecb5a254f mm: introduce delete_from_page_cache()
 */
#define BPM_DELETE_FROM_PAGE_CACHE_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(9,1)) */

#if !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,4,0) || \
	REDHAT_RELEASE_VERSION_IS_RANGE(8,7, 8,9) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,1))
/*
 * 365481e42a8a driver core: auxiliary bus: Add driver data helpers
 */
#define BPM_AUXILIARY_BUS_HELPERS_NOT_PRESENT
#endif /* !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,4,0) || REDHAT_RELEASE_VERSION_IS_GEQ(8,7) ... */
#endif /* LINUX_VERSION_IS_LESS(5,17,0) */


#if (LINUX_VERSION_IS_GEQ(5,16,0) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,2) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0))
/*
 * ab09243aa95a7 mm/migrate.c: remove MIGRATE_PFN_LOCKED
 */
/* TBD: Need to reverify the patch as there is no issue */
#define BPM_MIGRATE_PFN_LOCKED_REMOVED
#endif /* (LINUX_VERSION_IS_GEQ(5,16,0) || REDHAT_RELEASE_VERSION_IS_GEQ(9,2) ... */

#if LINUX_VERSION_IS_GEQ(5,16,0) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,0) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0)
/*
 * 16b0314aa746be
 * dma-buf: move dma-buf symbols into the DMA_BUF module namespace
 */
#define BPM_MODULE_IMPORT_NS_SUPPORT
#endif /* LINUX_VERSION_IS_GEQ(5,16,0) || REDHAT_RELEASE_VERSION_IS_GEQ(9,0) ... */

#if (LINUX_VERSION_IS_GEQ(5,16,0) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(8,7))
/*
 *12235da8c80a1 kernel/locking: Add context to ww_mutex_trylock()
 */
#define BPM_WW_MUTEX_TRYLOCK_WITH_CTX_PRESENT
 
/* c921ff373b469 dma-buf: add dma_resv_for_each_fence_unlocked v8
 */
#define BPM_DMA_RESV_ITER_UNLOCKED_PRESENT
#endif	/* (LINUX_VERSION_IS_GEQ(5,16,0) || REDHAT_RELEASE_VERSION_IS_GEQ(8,7)) */

#if LINUX_VERSION_IS_LESS(5,16,0)
/* DP 2.0 E11 feature */
/*
 * d6c6a76f80a1c9 drm: Update MST First Link Slot Information
 * Based on Encoding Format
 */
#define BPM_DRM_DP_MST_UPDATE_SLOTS_NOT_PRESENT
/*
 * c78b4a85721f3 drm/dp: add helper for extracting adjust 128b/132b TX FFE preset
 */
#define BPM_DRM_DP_GET_ADJUST_NOT_PRESENT

/*
 * 103c7044be5b207 drm/i915/edp: use MSO pixel overlap from DisplayID data
 */
#define BPM_MSO_PIXEL_OVERLAP_DISPLAY_NOT_PRESENT

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(9,1) || \
     (REDHAT_RELEASE_VERSION_IS_RANGE(8,2, 8,9) && !(IS_ENABLED(CPTCFG_BUILD_I915))) || \
      SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0) || \
      CUSTOM_KERN_1_RELEASE_VERSION_IS_GEQ(8,6656) || \
      LINUX_VERSION_IN_RANGE(5,10,0, 5,11,0) || \
      LINUX_VERSION_IN_RANGE(5,4,0, 5,5,0))
/*
 * d6c6a76f80a1c drm: Update MST First Link Slot Information Based on Encoding Format
 */
#define BPM_DRM_PAYLOAD_PART1_START_SLOT_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(9,1) ... */
#endif /* LINUX_VERSION_IS_LESS(5,16,0) */

#if LINUX_VERSION_IS_LESS(5,15,46)
#if !((SUSE_RELEASE_VERSION_IS_GEQ(1,15,4,0) && !(SUSE_LOCAL_VERSION_IS_LESS(24,11))) || \
        UBUNTU_RELEASE_VERSION_IS_GEQ(20,04) || \
        REDHAT_RELEASE_VERSION_IS_EQL(8,9) || REDHAT_RELEASE_VERSION_IS_GEQ(9,3))
/*
 * 0425473037db list: introduce list_is_head() helper and re-use it in list.h
 */
#define BPM_LIST_IS_HEAD_NOT_PRESENT
#endif /* !((SUSE_RELEASE_VERSION_IS_GEQ(1,15,4,0) && !(SUSE_LOCAL_VERSION_IS_LESS(24,11))) ... */
#endif /* LINUX_VERSION_IS_LESS(5,15,46) */

#if LINUX_VERSION_IS_LESS(5,15,8)
#if !((REDHAT_RELEASE_VERSION_IS_RANGE(8,7, 8,9) || REDHAT_RELEASE_VERSION_IS_GEQ(9,1)) || \
	(SUSE_RELEASE_VERSION_IS_GEQ(1,15,4,0) && SUSE_LOCAL_VERSION_IS_GEQ(24,41)))
/*
 * e4779015fd5d timers: implement usleep_idle_range()
 */
#define BPM_USLEEP_RANGE_STATE_NOT_PRESENT
#endif /* !((REDHAT_RELEASE_VERSION_IS_RANGE(8,7, 8,9) || REDHAT_RELEASE_VERSION_IS_GEQ(9,1)) ... */
#endif /* LINUX_VERSION_IS_LESS(5,15,8) */

#if (LINUX_VERSION_IS_GEQ(5,15,0) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,1) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0))
/*
 * ac1723c16b drm/i915: Track IRQ state in local device state.
 */
#define BPM_DRM_DEVICE_IRQ_ENABLED_INSIDE_LEGACY_ADDED
#endif /* (LINUX_VERSION_IS_GEQ(5,15,0) || REDHAT_RELEASE_VERSION_IS_GEQ(9,1) ... */

#if LINUX_VERSION_IS_LESS(5,15,0)
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,4,0) || \
	LINUX_VERSION_IN_RANGE(5,10,211, 5,11,0) || \
	LINUX_VERSION_IN_RANGE(5,4,270, 5,5,0))
/*
 * d19c81378829e locking/lockdep: Provide lockdep_assert{,_once}() helpers
 */
#define BPM_LOCKDEP_ASSERT_API_NOT_PRESENT
#endif //!(REDHAT_RELEASE_VERSION_IS_GEQ(8,6))

#if !(UBUNTU_RELEASE_VERSION_IS_GEQ(20,04) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(8,7))

#if !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,4,0))
/*
 * b8779475869a vgaarb: provide a vga_client_unregister wrapper
 */
#define BPM_VGA_CLIENT_UNREGISTER_NOT_PRESENT
#endif //!(SUSE_RELEASE_VERSION_IS_GEQ(1,15,4,0)

#if !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0))
/*
 * bf44e8cecc03c vgaarb: don't pass a cookie to vga_client_register
 */
#define BPM_VGA_SET_DECODE_ARG_PCI_DEV_NOT_PRESENT
#endif //!(SUSE_RELEASE_VERSION_IS_GEQ(1,15,4,0)
#endif //!(UBUNTU_RELEASE_VERSION_IS_GEQ(20,04) || REDHAT_RELEASE_VERSION_IS_GEQ(8,7))

#if (SUSE_RELEASE_VERSION_IS_GEQ(1,15,4,0))
/*
 * 867cf9cd73c3d drm/dp: Extract i915's eDP backlight code into DRM helpers
 */
#define BPM_DRM_EDP_BACKLIGHT_SUPPORT_PRESENT
#endif

#if !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,4,0))
/*
 * 1072ed3431f5ba2 drm/dp: Move panel DP AUX backlight support to
 * drm_dp_helper
 * 10f7b40e4f3050 drm/panel: add basic DP AUX backlight support
 */
#define BPM_AUX_BACKLIGHT_SUPPORT_TO_DRM_DP_NOT_PRESENT

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(9,2))
/*
 * fc7a620 bus: Make remove callback return void
 *
 * In file bus.h in bus_type sturct, return type of remove function
 * changed from int to void
 */
#define BPM_BUS_REMOVE_FUNCTION_RETURN_TYPE_CHANGED
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(9,2)) */


#if !(REDHAT_RELEASE_VERSION_IS_GEQ(9,1)|| REDHAT_RELEASE_VERSION_IS_RANGE(8,2, 8,9) || \
       LINUX_VERSION_IN_RANGE(5,10,0, 5,11,0) || \
       LINUX_VERSION_IN_RANGE(5,4,0, 5,5,0) || \
       CUSTOM_KERN_1_RELEASE_VERSION_IS_GEQ(8,6656))
/*
 * 97c9bfe3f660 drm/aperture: Pass DRM driver structure instead of driver name
 */
#define BPM_API_ARG_DRM_DRIVER_REMOVED

/*
 * 440d0f12b52a dma-buf: add dma_fence_chain_alloc/free v3
 */
#define BPM_DMA_FENCE_CHAIN_ALLOC_NOT_PRESENT
#endif /* REDHAT_RELEASE_VERSION_IS_EQL(9,0)) || !(LINUX_VERSION_IN_RANGE(5,10,0, 5,11,0)... */


#if !(REDHAT_RELEASE_VERSION_IS_GEQ(9,0))
/*
 * f0ab00174eb7 PCI: Make saved capability state private to core
 */
#define BPM_PCI_INTERFACES_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(9,0)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6))
/*
 *6f2beb268a5d swiotlb: Update is_swiotlb_active to add a struct device argument
 */
#define BPM_IS_SWIOTLB_ACTIVE_ARG_DEV_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6)) */
#endif /* !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,4,0)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || \
	UBUNTU_RELEASE_VERSION_IS_GEQ(20,04))
/*
 * 90e7a6de62781
 * lib/scatterlist: Provide a dedicated function to support table append
 */
#define BPM_SG_ALLOC_TABLE_FROM_PAGES_SEGMENT_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || UBUNTU_RELEASE_VERSION_IS_GEQ(20,04)) */

#if !((LINUX_VERSION_IN_RANGE(5,14,0, 5,15,0) && UBUNTU_RELEASE_VERSION_IS_GEQ(1011,0)) || \
	(REDHAT_RELEASE_VERSION_IS_EQL(8,6) && REDHAT_BACKPORT_MINOR_VERSION_IS_GEQ(372,70,1)) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(8,7) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0))
/*
 * 59dc33252ee7 PCI: VMD: ACPI: Make ACPI companion lookup work for VMD bus
 */
#define BPM_PCI_FIND_HOST_BRIDGE_NOT_EXPORTED
#endif /* !((LINUX_VERSION_IN_RANGE(5,14,0, 5,15,0) && UBUNTU_RELEASE_VERSION_IS_GEQ(1011,0)) ... */
#endif /* LINUX_VERSION_IS_LESS(5,15,0) */

#if LINUX_VERSION_IS_LESS(5,14,19)
#if !(LINUX_VERSION_IN_RANGE(5,10,68, 5,11,0) || \
	REDHAT_RELEASE_VERSION_IS_RANGE(8,6, 9,0) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,2) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,4,0))

/* aeef8b5089b7 x86/pat: Pass valid address to sanitize_phys()*/
#define BPM_ROUND_DOWN_IOMEM_RESOURCE_END
#endif /* !(LINUX_VERSION_IN_RANGE(5,10,68, 5,11,0) || REDHAT_RELEASE_VERSION_IS_RANGE(8,6, 9,0) ... */

#if LINUX_VERSION_IN_RANGE(5,4,0, 5,5,0) || \
	!(LINUX_VERSION_IN_RANGE(5,10,80, 5,11,0) || \
                REDHAT_RELEASE_VERSION_IS_RANGE(8,7, 9,1) || \
		CUSTOM_KERN_1_RELEASE_VERSION_IS_LESS(8,6656))

/* 74ba917cfddd arch/cc: Introduce a function to check for confidential computing features*/
#define BPM_CC_PLATFORM_H_NOT_PRESENT
#endif
#endif /* LINUX_VERSION_IS_LESS(5,14,19) */

#if LINUX_VERSION_IS_GEQ(5,13,0) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(8,6)

/*
 * dma-buf/dmabuf: Don't export dma_fence symbols
 */
#define BPM_DMA_FENCE_PRIVATE_STUB_PRESENT
#endif

#if LINUX_VERSION_IS_LESS(5,13,0)
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6))
/*
 * eb2dafbba8b82 tasklets: Prevent tasklet_unlock_spin_wait() deadlock on RT
 */
#define BPM_TASKLET_UNLOCK_SPIN_WAIT_NOT_PRESENT

/*
 * f21ffe9f6da6d swiotlb: Expose swiotlb_nr_tlb function to modules
 */
#define BPM_SWIOTLB_NR_TBL_NO_ARG_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5))
/*
 * 3e31f94752e4 lockdep: Add lockdep_assert_not_held()
 */
#define BPM_LOCKDEP_ASSERT_NOT_HELD_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5)) */

#if !(LINUX_VERSION_IN_RANGE(5,10,70, 5,11,0) || \
	CUSTOM_KERN_1_RELEASE_VERSION_IS_GEQ(8,6656) || \
	CUSTOM_KERN_3_RELEASE_VERSION_IS_GEQ(136,12,0))
/*
 * 4f0f586bf0c8 treewide: Change list_sort to use const pointers
 */
#define BPM_LIST_CMP_FUNC_T_NOT_PRESENT
#endif /* !(LINUX_VERSION_IN_RANGE(5,10,70, 5,11,0) || CUSTOM_KERN_1_RELEASE_VERSION_IS_GEQ(8,6656) ... */
#endif /* LINUX_VERSION_IS_LESS(5,13,0) */

#if (LINUX_VERSION_IS_GEQ(5,12,0) || \
       REDHAT_RELEASE_VERSION_IS_GEQ(8,9))
/*
 * a28a6e860c6c string.h: move fortified functions definitions in a dedicated header
 */
#define BPM_FORTIFY_STRING_H_NOT_PRESENT
#endif /* (LINUX_VERSION_IS_GEQ(5,12,0) || REDHAT_RELEASE_VERSION_IS_GEQ(8,9)) */

#if (LINUX_VERSION_IS_GEQ(5,12,0) || \
        REDHAT_RELEASE_VERSION_IS_GEQ(8,5))

/*
 * dma-buf/dmabuf: Don't export dma_fence symbols
 */
#define BPM_DMA_FENCE_TIMESTAMP_PRESENT
#endif

#if (LINUX_VERSION_IS_LESS(5,12,0) && \
	!(REDHAT_RELEASE_VERSION_IS_GEQ(8,5)))
/*
 * 276b738deb5bf PCI: Add resizable BAR infrastructure
 * 192f1bf7559e8 PCI: Add pci_rebar_bytes_to_size()
 */
#define BPM_PCI_REBAR_SIZE_NOT_PRESENT

/*
 * 2d24dd5798d0 rbtree: Add generic add and find helpers
 */
#define BPM_RB_FIND_NOT_PRESENT

/*
 * 97a7e4733b9b mm: introduce page_needs_cow_for_dma() for deciding whether cow
 */
#define BPM_IS_COW_MAPPING_NOT_PRESENT

/*
 * 23c887522e91 Relay: add CPU hotplug support
 *
 */
#define BPM_CONST_STRUCT_RCHAN_CALLBACKS_NOT_PRESENT
#endif /* LINUX_VERSION_IS_LESS(5,12,0) && !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5)) */

#if LINUX_VERSION_IS_LESS(5,11,0)
#ifndef CPTCFG_BUILD_I915
/*
 * 295992fb815e7 mm: introduce vma_set_file function v5
 *
 */
#define BPM_VMA_SET_FILE_NOT_PRESENT
#endif /* CPTCFG_BUILD_I915 */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,9))
/*
 * aa6159ab99a9ab kernel.h: split out mathematical helpers
 */
#define BPM_MATH_H_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,9)) */

/*
 * ab22dd46b60 Backport "drm/i915: Change shrink ordering to use locking
 * around unbinding" and deps
 */
#define BPM_MIGHT_ALLOC_NOT_PRESENT

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5))
/*
 * f0dbd2bd1c22c66 mm: slab: provide krealloc_array()
 *
 * Backport krealloc_array() api.
 */
#define BPM_KREALLOC_ARRAY_NOT_PRESENT

/*
 * 23c887522e91 Relay: add CPU hotplug support
 */
#define BPM_CONST_STRUCT_RCHAN_CALLBACKS_NOT_PRESENT

/*
 *cfc78dfd9b36 iommu/sva: Add PASID helpers
 */
#define BPM_IOMMU_SVA_LIB_H_NOT_PRESENT

/*
 * 23c887522e91 Relay: add CPU hotplug support
 */
#define BPM_CONST_STRUCT_RCHAN_CALLBACKS_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5)) */

/* TBD: check if this can be converted to in_range. reverify for 8.4 and 8.5 */
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || \
	REDHAT_RELEASE_VERSION_IS_LEQ(8,3) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,2,0) || \
	LINUX_VERSION_IN_RANGE(5,4,0, 5,5,0))
/*
 * ab440b2c604b seqlock: Rename __seqprop() users
 */
#define BPM_SEQPROP_SEQUENCE_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || REDHAT_RELEASE_VERSION_IS_LEQ(8,3) ... */

#if !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0))

/* Need to check the need of panel_orientatio_quirks */
#define BPM_DRM_GET_PANEL_ORIENTATION_QUIRK_DONT_EXPORT
#endif /* !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0)) */

#if (REDHAT_RELEASE_VERSION_IS_EQL(8,4))
/*
 * f0c0c115fb81 mm: memcontrol: account pagetables per node
 */
#define BPM_MOD_LRUVEC_PAGE_STATE_NOT_EXPORTED
#endif /* (REDHAT_RELEASE_VERSION_IS_EQL(8,4)) */
#endif /* LINUX_VERSION_IS_LESS(5,11,0) */

#if LINUX_VERSION_IS_LESS(5,10,0)
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5))
/*
 * 07da1223ec93 lib/scatterlist: Add support in dynamic allocation of SG table from pages
 */
#define BPM_SG_CHAIN_NOT_PRESENT
#endif
#endif /* LINUX_VERSION_IS_LESS(5,10,0) */

#if LINUX_VERSION_IS_LESS(5,12,0)
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5))
/*
 * 97a7e4733b9b mm: introduce page_needs_cow_for_dma() for deciding whether cow
 */
#define BPM_IS_COW_MAPPING_NOT_PRESENT
#endif
#endif /*LINUX_VERSION_IS_LESS(5,12,0)*/

#if LINUX_VERSION_IS_LESS(5,10,0)
/*
 * f0907827a8a9 compiler.h: enable builtin overflow checkers and add fallback code
 */
#define BPM_OVERFLOW_H_NOT_PRESENT

/*
 * 1967f71267742 hwmon: (core) Add support for rated attributes
 */
#define BPM_POWER1_RATED_MAX_NOT_PRESENT

/*
 * aedcade6f4fa debugobjects: Allow debug_obj_descr to be const
 */
#define BPM_DEBUG_OBJECT_ACTIVATE_NO_CONST_ARG

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || \
        SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0))
/*
 * b7b3c01b19159 mm/memremap_pages: support multiple ranges per invocation
 */
#define BPM_PAGEMAP_RANGE_START_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0)) */

#if !(LINUX_VERSION_IN_RANGE(5,4,103, 5,5,0) || \
	LINUX_VERSION_IN_RANGE(4,19,179, 4,20,0))
/*
 * 48e2e013dc71602 drm/i915: Expose list of clients in sysfs
 */
#ifndef CPTCFG_BUILD_I915
#define BPM_SYSFS_EMIT_NOT_PRESENT
#endif /* CPTCFG_BUILD_I915 */
#endif /* !(LINUX_VERSION_IN_RANGE(5,4,103, 5,5,0) || LINUX_VERSION_IN_RANGE(4,19,179, 4,20,0)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5))
/*
 * 4f6ec8602341e mm/vmalloc: separate put pages and flush VM flags
 */
#define BPM_VM_MAP_PUT_PAGES_NOT_PRESENT

/*
 * Resolve issues of minmax.h
 */
#define BPM_LINUX_MINMAX_H_PRESENT

/*
 * 3e9a9e256b1e mm: add a vmap_pfn function
 */
#define BPM_VMAP_PFN_NOT_PRESENT

/*
*48526a0f4ca2b4 genetlink: bring back per op policy
*/
#define BPM_GENL_OPS_POLICY_MEMBER_NOT_PRESENT

/*
 * 07da1223ec93 lib/scatterlist: Add support in dynamic allocation of SG table from pages
 */
#define BPM_SG_CHAIN_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5)) */

/* TBD: verify for 8.4 and 8.5  */
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || \
	REDHAT_RELEASE_VERSION_IS_LEQ(8,3) || \
        SUSE_RELEASE_VERSION_IS_GEQ(1,15,2,0) || \
	LINUX_VERSION_IN_RANGE(5,4,0, 5,5,0))
/*
 * 8117ab508f9c476 seqlock: seqcount_LOCKNAME_t: Introduce PREEMPT_RT support
 */
#define BPM_SEQCOUNT_SEQUENCE_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || REDHAT_RELEASE_VERSION_IS_LEQ(8,3) ... */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,7))
/*
 * 7a9f50a05843 irq_work: Cleanup
 */
#define BPM_IRQ_WORK_NODE_LLIST_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,7)) */

/*
 * 8af2fa888eaf0e Show slab cache occupancy for debug
 */
#define BPM_KMEM_CACHE_SLABINFO_API_NOT_PRESENT

#if (LINUX_VERSION_IN_RANGE(4,19,249, 4,20,0) || \
	LINUX_VERSION_IN_RANGE(5,4,200, 5,5,0) || \
	LINUX_VERSION_IN_RANGE(5,10,119, 5,11,0) || \
	LINUX_VERSION_IN_RANGE(5,15,44, 5,16,0))
/*
 *0cc41e2c73f70 x86/tsc: Use fallback for random_get_entropy() instead of zero
 */
#define BPM_INCLUDE_CPUFEATURE_IN_TSC
#endif /* (LINUX_VERSION_IN_RANGE(4,19,249, 4,20,0) || LINUX_VERSION_IN_RANGE(5,4,200, 5,5,0) ... */

#if !(LINUX_VERSION_IN_RANGE(5,4,103, 5,4,224) || \
	LINUX_VERSION_IN_RANGE(4,19,179, 4,19,266))
/*
 * 48e2e013dc71602 drm/i915: Expose list of clients in sysfs
 */
#ifndef CPTCFG_BUILD_I915
#define BPM_SYSFS_EMIT_NOT_PRESENT
#endif /* CPTCFG_BUILD_I915 */
#endif /* !(LINUX_VERSION_IN_RANGE(5,4,103, 5,4,224) || LINUX_VERSION_IN_RANGE(4,19,179, 4,19,266)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4) || \
        SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0) || \
	LINUX_VERSION_IN_RANGE(5,4,0, 5,5,0))
/*
 * e130816164e include/linux/list.h: add a macro to test if entry is pointing to the head
 */
#define BPM_LIST_ENTRY_IS_HEAD_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4) || SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0) */
#endif /* LINUX_VERSION_IS_LESS(5,10,0) */

#if LINUX_VERSION_IS_LESS(5,9,11)
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || \
	LINUX_VERSION_IN_RANGE(5,4,86, 5,5,0) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0) || \
	(SUSE_RELEASE_VERSION_IS_GEQ(1,15,2,0) && SUSE_LOCAL_VERSION_IS_GEQ(24,61)))
/*
 * dd8088d5a896 PM: runtime: Add pm_runtime_resume_and_get
 * to deal with usage counter
 */
#define BPM_PM_RUNTIME_RESUME_AND_GET_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || LINUX_VERSION_IN_RANGE(5,4,86, 5,5,0) ... */
#endif // LINUX_VERSION_IS_LESS(5,9,11)

#if LINUX_VERSION_IN_RANGE(5,9,0, 5,11,0) && \
	!(CUSTOM_KERN_3_RELEASE_VERSION_IS_GEQ(136,12,0))
/*
 * c47d5032ed30 mm: move lruvec stats update functions to vmstat.h
 */
#define BPM_MOD_LRUVEC_STATE_NOT_EXPORTED
#endif /* LINUX_VERSION_IN_RANGE(5,9,0, 5,11,0) && !(CUSTOM_KERN_3_RELEASE_VERSION_IS_GEQ(136,12,0)) */

#if LINUX_VERSION_IS_LESS(5,9,0)
/*
 * 12cc923f1ccc tasklet: Introduce new initialization API
 */
#define BPM_TASKLET_STRUCT_CALLBACK_NOT_PRESENT

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,2))
/*
 * eedc4e5a142c
 * mm: memcg: factor out memcg- and lruvec-level changes out of __mod_lruvec_state()
 */
#define BPM_MOD_MEMCG_LRUVEC_STATE_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,2)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4))
/*
 * 267580db047ef428 seqlock: Unbreak lockdep
 */
#define BPM_SEQCOUNT_WW_MUTEX_INIT_NOT_PRESESNT

#if !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0))
/* 8b700983de82f sched: Remove sched_set_*() return value */
#define BPM_SCHED_SET_FIFO_NOT_PRESENT
#endif /* !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0)) */
#endif /* (REDHAT_RELEASE_VERSION_IS_GEQ(8,4)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || \
        SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0))
/*
 * 5143192cd410c mm/migrate: add a flags parameter to migrate_vma
 */
#define BPM_MIGRATE_VMA_PAGE_OWNER_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0) || \
	(SUSE_RELEASE_VERSION_IS_GEQ(1,15,2,0) && SUSE_LOCAL_VERSION_IS_GEQ(24,24)))

/* 3022c6a1b4b7 driver-core: Introduce DEVICE_ATTR_ADMIN_{RO,RW} */
#define BPM_DEVICE_ATTR_ADMIN_RX_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4) || SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0) ... */
#endif /* LINUX_VERSION_IS_LESS(5,9,0) */

#if LINUX_VERSION_IS_LESS(5,8,0)
/*
 * ca5999f mm: introduce include/linux/pgtable.h
 * 64fa30f Backport and fix intel-gtt split
 */
#define BPM_ASM_PGTABLE_H_NOT_PRESENT

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4))
/*
 * 6058eaec816f mm: fold and remove lru_cache_add_anon() and lru_cache_add_file()
 */
#define BPM_LRU_CACHE_ADD_EXPORT_NOT_PRESENT
/*
 * 376a34efa4ee mm/gup: refactor and de-duplicate gup_fast() code
 */
#define BPM_FOLL_FAST_ONLY_NOT_PRESENT

#if !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0))

/*3d2d827f5ca5e3  mm: move use_mm/unuse_mm from aio.c to mm */
#define BPM_KTHREAD_HEADER_NOT_PRESENT

/*
 * 999a22890cb1 uaccess: Add user_read_access_begin/end and
 * user_write_access_begin/end
 */
#define BPM_USER_WRITE_ACCESS_BEGIN_NOT_PRESENT

/*
 * 999a22890cb1 uaccess: Add user_read_access_begin/end and
 * user_write_access_begin/end
 */
#define BPM_USER_WRITE_ACCESS_BEGIN_NOT_PRESENT

/*
 * 3f50f132d8400e1 bpf: Verifier, do explicit ALU32 bounds tracking
 */
#define BPM_U32_MIN_NOT_PRESESNT

/* dc5bdb68b5b drm/fb-helper: Fix vt restore */
#define BPM_FB_ACTIVATE_KD_TEXT_NOT_PRESENT

/* e07515563d010d8b PM: sleep: core: Rename DPM_FLAG_NEVER_SKIP */
#define BPM_DPM_FLAG_NEVER_SKIP_RENAMED

#if !(LINUX_VERSION_IN_RANGE(5,4,207, 5,5,0))
/*
 * 9740ca4e95b43b mmap locking API: initial implementation as rwsem wrappers
 */
#define BPM_MMAP_WRITE_LOCK_NOT_PRESENT
#endif /* !(LINUX_VERSION_IN_RANGE(5,4,207, 5,5,0)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0) || \
	LINUX_VERSION_IN_RANGE(5,4,0, 5,5,0))
/*
 * 709d6d73c7561 scatterlist: add generic wrappers for iterating over sgtable objects
 */
#define BPM_FOR_EACH_SGTABLE_PAGE_NOT_PRESENT
/* d9d200bcebc1f6e dma-mapping: add generic helpers
 * for mapping sgtable objects
 */
#define BPM_DMA_MAP_UNMAP_SGTABLE_NOT_PRESENT
#endif /* REDHAT_RELEASE_VERSION_IS_GEQ(8,4) || SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0) ... */

#if !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,2,0) && SUSE_LOCAL_VERSION_IS_GEQ(24,43))
/*
 * f5678e7f2ac3 kernel: better document the use_mm/unuse_mm API contract
 */
#define BPM_KTHREAD_USE_MM_NOT_PRESENT
#endif /* !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,2,0) && SUSE_LOCAL_VERSION_IS_GEQ(24,43)) */
#endif /* !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0)) */
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5))
/* 
 * 42fc541404f2 mmap locking API: add mmap_assert_locked() and mmap_assert_write_locked()
 */
#define BPM_MMAP_ASSERT_LOCKED_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5)) */

/*
 * 97a32539b956 proc: convert everything to "struct proc_ops"
 */
#define BPM_STRUCT_PROC_OPS_NOT_PRESENT

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6))

/*
 * 479da1f538a2 backlight: Add backlight_device_get_by_name()
 */
#define BPM_BACKLIGHT_DEV_GET_BY_NAME_NOT_PRESENT
/*
 * 9807372 capabilities: Introduce CAP_PERFMON to kernel and user space
 */
#define BPM_PERFMON_CAPABLE_NOT_PRESENT

#if !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0))
/*
 * f45ce9336ff0640 video/hdmi: Add Unpack only function for DRM infoframe
 */
#define BPM_HDMI_DRM_INFOFRAME_UNPACK_NOT_PRESENT
#endif /* !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0)) */
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6)) */
#endif /* LINUX_VERSION_IS_LESS(5,8,0) */

#if LINUX_VERSION_IS_LESS(5,7,0)
/*
 * 132ccc042281420 INTEL_DII: drm/i915/spi: refcount spi object lifetime
 */
#define BPM_MTD_PART_NOT_PRESENT

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5))

/*
 * c111566bea7c PM: runtime: Add pm_runtime_get_if_active()
 */
#define BPM_PM_RUNTIME_GET_IF_ACTIVE_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,2,0) || \
	LINUX_VERSION_IN_RANGE(5,4,57, 5,5,0))
/*
 * c0842fbc1b18 random32: move the pseudo-random 32-bit
 * definitions to prandom.h
 */
#define BPM_PRANDOM_H_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0))
/*
 * 67b06ba01857 PM: QoS: Drop PM_QOS_CPU_DMA_LATENCY and rename
 */
#define BPM_CPU_LATENCY_QOS_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4) ||SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0) */
#endif /* LINUX_VERSION_IS_LESS(5,7,0) */

#if LINUX_VERSION_IS_LESS(5,6,0)
/*
 * 97a32539b956 proc: convert everything to "struct proc_ops"
 */
#define BPM_STRUCT_PROC_OPS_NOT_PRESENT

/*
 * 32d5109a9d86 netlink: rename nl80211_validate_nested() to nla_validate_nested()
 */
#define BPM_NLA_VALIDATE_NESTED_NOT_PRESENT

/*
 * e692b4021a2e4 lockdep: add might_lock_nested()
 */
#define BPM_MIGHT_LOCK_NESTED_NOT_PRESENT

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6))
/*
 * a392d26f32cdd87 include/bitmap.h: add new functions to documentation
 */
#define BPM_BITMAP_CLEAR_REGION_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4))
/*
 * bf9e25ec1287 video: fbdev: make fbops member of struct fb_info a const pointer
 */
#define BPM_PIN_USER_PAGES_FAST_NOT_PRESENT

/*
 * f1f6a7dd9b53 mm, tree-wide: rename put_user_page*() to unpin_user_page*()
 */
#define BPM_PIN_OR_UNPIN_USER_PAGE_NOT_PRESENT

#if !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0))
/*
 * b6ff753a0ca0d drm: constify fb ops across all drivers
 */
#define BPM_CONST_STRUCT_FB_OPS_NOT_PRESENT

/* c72bed23b9e45ac pinctrl: Allow modules to
 * use pinctrl_[un]register_mappings
 */
#define BPM_PINCTRL_UNREGISTER_MAPPINGS_NOT_PRESENT
/* 
 * 28ca0d6d39ab list: introduce list_for_each_continue()
 */
#define BPM_LIST_FOR_EACH_CONTINUE_NOT_PRESENT
#endif /* !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0)) */
#endif /* (REDHAT_RELEASE_VERSION_IS_GEQ(8,4)) */
#endif /* LINUX_VERSION_IS_LESS(5,6,0) */

#if LINUX_VERSION_IS_LESS(5,5,0)
/*
 * 8c2a2b8c2ff68 nvmem: core: add nvmem_device_find
 */
#define BPM_NVMEM_DEVICE_FIND_NOT_PRESENT

#if !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0))
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4))

/*
 * a63fc6b75cca9 rcu: Upgrade rcu_swap_protected() to rcu_replace_pointer()
 */
#define BPM_RCU_REPLACE_POINTER_NOT_PRESENT

/*
 * c9c13ba428ef9 PCI: Add PCI_STD_NUM_BARS for the number of standard BARs
 */
#define BPM_PCI_STD_NUM_BARS_NOT_DEFINED

/* 
 * 0a8459693238a339
 * fbdev: drop res_id parameter from remove_conflicting_pci_framebuffers
 */
#define BPM_REMOVE_CONF_PCI_FB_ARG_NOT_PRESENT

/*
 * 5facae4f354
 * locking/lockdep: Remove unused @nested argument from lock_release()
 */
#define BPM_LOCKING_NESTED_ARG_NOT_PRESENT

/*
 * 8c9312a925ad8 i2c: add helper to check if a client has a driver attached
 */
#define BPM_I2C_CLIENT_HAS_DRIVER_NOT_PRESENT

#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4)) */

#if (REDHAT_RELEASE_VERSION_IS_GEQ(8,5))
/*
 * 99cb252f5e68d72  mm/mmu_notifier: add an interval tree notifier
 */
#define BPM_HMM_RANGE_NOTIFIER_NOT_PRESENT
#endif /* (REDHAT_RELEASE_VERSION_IS_GEQ(8,5)) */
#endif /* !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4) || \
        SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0))

/* 5facae4f354 locking/lockdep: Remove unused
 * @nested argument from lock_release()
 */
#define BPM_LOCKING_NESTED_ARG_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4) || SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0)) */
#endif /* LINUX_VERSION_IS_LESS(5,5,0) */

#if LINUX_VERSION_IS_LESS(5,4,0)
/*
 * 7240b60c98d6 linux: Add skb_frag_t page_offset accessors
 */
#define BPM_SKB_FRAG_OFF_PRESENT

/*
 * 895b5c9f206e netfilter: drop bridge nf reset from nf_reset
 */
#define BPM_NF_RESET_CT_PRESENT

/*
 * 12c88d840b45 module: add support for symbol namespaces (jsc#SLE-10158).
 */
#define BPM_MODULE_IMPORT_NS_NOT_PRESENT

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4))
/*
 * 7ce2e76a0420 PCI: Move ASPM declarations to linux/pci.h
 */
#define BPM_PCI_ASPM_H_NOT_PRESENT

/*
 * 4495dfd drivers: Introduce device lookup variants by device type
 */
#define BPM_FIND_BY_DEVICE_TYPE_NOT_AVAILABLE
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,2,0))
/*
 * 8896dd968 compat_ioctl: add compat_ptr_ioctl()
 */
#define BPM_COMPAT_PTR_IOCTL_NOT_PRESENT

/*
 * 8973ea47901c driver core: platform: Introduce platform_get_irq_optional()
 */
#define BPM_PLATFORM_GET_IRQ_OPTIONAL_NOT_PRESENT

/*
 * 315cc066b8ae augmented rbtree: add new RB_DECLARE_CALLBACKS_MAX macro
 */
#define BPM_RB_DECLARE_CALLBACKS_MAX_NOT_PRESENT

/*
 * 2d15eb31b50 mm/gup: add make_dirty arg to put_user_pages_dirty_lock()
 */
#define BPM_PUT_USER_PAGES_DIRTY_LOCK_ARG_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4) || SUSE_RELEASE_VERSION_IS_GEQ(1,15,2,0)) */

#if !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,2,0) && SUSE_LOCAL_VERSION_IS_GEQ(24,46))
/*
 * 294f69e662d1
 * compiler_attributes.h: Add 'fallthrough' pseudo keyword for switch/case use
 */
#define BPM_FALLTHROUGH_API_NOT_PRESENT
#endif /* !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,2,0) && SUSE_LOCAL_VERSION_IS_GEQ(24,46)) */
#endif /* LINUX_VERSION_IS_LESS(5,4,0) */

#if LINUX_VERSION_IS_LESS(5,3,0)
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5))
/*
 * d2a8ebbf8192b kernel.h: split out container_of() and typeof_member() macros
 */
#define BPM_TYPEOF_MEMBER_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,5)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4))
/*
 * 5213d7efc8ec2 i2c: acpi: export i2c_acpi_find_adapter_by_handle
 */
#define BPM_I2C_ACPI_FIND_ADAPTER_BY_HANDLE_EXPORT_NOT_PRESENT
/*
 * 6471384af2a6 mm: security: introduce init_on_alloc=1 and init_on_free=1 boot options
 */
#define BPM_WANT_INIT_ON_ALLOC_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,3))
/* 31d6d5ce5340 vfs: Provide a mount_pseudo-replacement
 * for the new mount API
 */
#define BPM_PSEUDO_H_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,3)) */
#endif /* LINUX_VERSION_IS_LESS(5,3,0) */

#if LINUX_VERSION_IS_LESS(5,2,0)
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,2))
/*
 * ef6243acb478 genetlink: optionally validate strictly/dumps
 */
#define BPM_GENL_VALIDATE_FLAGS_PRESENT
/*
 * 3de644035446 netlink: re-add parse/validate functions in strict mode
 */
#define BPM_NLMSG_PARSE_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,2)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6))
/*
 * bf198b2b34bf mm/mmu_notifier: pass down vma and reasons why mmu notifier is happening
 */
#define BPM_MMU_NOTIFIER_RANGE_VMA_MEMBER_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4))

/* f3a09c92018a introduce fs_context methods */
#define BPM_INIT_FS_CONTEXT_NOT_PRESENT

/*
 * a49294eac27c7 Add wait_var_event_interruptible()
 */
#define BPM_WAIT_VAR_EVENT_INTERRUPTIBLE_NOT_PRESENT

/*
 * aa30f47cf66 kobject: Add support for default attribute groups to kobj_type
 */
#define BPM_DEFAULT_GROUPS_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,3))
/*
 * 54d50897d544 linux/kernel.h: split *_MAX and *_MIN macros into <linux/limits.h>
 */
#define BPM_LIMITS_H_NOT_PRESENT

/*
 * c43a113ca2c hwmon: Add convience macro to define simple static sensors
 */
#define BPM_HWMON_CHANNEL_INFO_NOT_PRESENT

/*
 * 7159dbdae3 i2c: core: improve return value handling of
 * i2c_new_device and i2c_new_dummy
 */
#define BPM_I2C_NEW_CLIENT_DEVICE_NOT_PRESENT

#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,3)) */
#endif /* LINUX_VERSION_IS_LESS(5,2,0) */

#if LINUX_VERSION_IS_LESS(5,1,0)
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,2))
/*
 * 23323289b154 netlink: reduce NLA_POLICY_NESTED{,_ARRAY} arguments
 */
#define BPM_NLA_POLICY_NESTED_ARRAY_NOT_PRESENT
#endif
#endif /* LINUX_VERSION_IS_LESS(5,1,0) */

#if LINUX_VERSION_IS_LESS(5,0,0)
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,2))
/*
 * 822b3b2ebfff net: Add max rate tx queue attribute 
 */
#define BPM_BACKPORT_DEV_OPEN_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,8)) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4))

/*
 * b33a02aadcc63 i2c: acpi: Move I2C bits from acpi.h to i2c.h
 */
#define BPM_I2C_ACPI_GET_I2C_RESOURCE_NOT_PRESENT

/*
 * 72921427d46 string.h: Add str_has_prefix() helper function
 */
#define BPM_STR_HAS_PREFIX_NOT_PRESENT
/*
 * ca79b0c211af mm: convert totalram_pages and totalhigh_pages
 * variables to atomic
 */
#define BPM_TOTALRAM_PAGES_FUNC_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,4) */
#endif /* LINUX_VERSION_IS_LESS(5,0,0) */

#if LINUX_VERSION_IS_GEQ(4,20,0)
/*
 * 7ab606d1609d genetlink: pass extended ACK report down
 */
#define BPM_CB_EXTRACK_NOT_PRESENT
#endif /* LINUX_VERSION_IS_GEQ(4,20,0) */

#if LINUX_VERSION_IS_LESS(4,20,0)
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,9))
/*
 * a3f8a30f3f00 Compiler Attributes: use feature
 * checks instead of version checks
 */
#define BPM_COMPILER_ATTRIBUTES_HEADER_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,9)) */

#if LINUX_VERSION_IN_RANGE(4,19,10, 4,20,0)
/*
 *a8305bff6852 net: Add and use skb_mark_not_on_list().
 */
#define BPM_SKB_MARK_NOT_ON_LIST_PRESENT
#endif /* LINUX_VERSION_IN_RANGE(4,19,10, 4,20,0) */

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,2))
/*
 * 8b69bd7d8a89 ppp: Remove direct skb_queue_head list pointer access
 */
#define BPM_SKB_PEEK_PRESENT
/*
 * 3e48be05f3c7 netlink: add attribute range validation to policy
 */
#define BPM_NLA_POLICY_VALIDATION_PRESENT
/*
 * 74de6960c99d rcu: Provide functions for determining if call_rcu() has been invoked
 */
#define BPM_RCU_HEAD_INIT_NOT_PRESENT
#endif /* !(REDHAT_RELEASE_VERSION_IS_GEQ(8,2)) */
#endif /* LINUX_VERSION_IS_LESS(4,20,0) */

#if LINUX_VERSION_IS_LESS(4,19,0)
/*
 * 14d32b2 Defined jiffies_delta_to_msecs() function
 */
#define BPM_JIFFIES_DELTA_TO_MSECS_NOT_PRESENT
#endif /* LINUX_VERSION_IS_LESS(4,19,0) */

#if LINUX_VERSION_IN_RANGE(3,17,0, 5,3,0)
#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,2))
/*
 * 9285ec4c8b61 timekeeping: Use proper clock specifier names in functions
 */
#define BPM_KTIME_GET_BOOT_NS_NOT_PRESENT
#endif
#endif

/***************************************************************************/
/***************************************************************************/

#if LINUX_VERSION_IS_LESS(5,0,0) && \
	!(REDHAT_RELEASE_VERSION_IS_GEQ(8,4))
/*
 * ca79b0c211af mm: convert totalram_pages and totalhigh_pages
 * variables to atomic
 */
#define BPM_TOTALRAM_PAGES_FUNC_NOT_PRESENT
#endif /*LINUX_VERSION_IS_LESS(5,0,0)*/

/*
 *  Upstream Patches not merged in any kernel yet
 */

/*
 * c1a01f290103d drm: constify sysrq_key_op
 */
#define BPM_CONST_SYSRQ_KEY_OP_NOT_PRESENT

/*
 * 9299148acf5422 VFIO - SR-IOV VF migration
 */
#define BPM_VFIO_SR_IOV_VF_MIGRATION_NOT_PRESENT

/*
 * Introduced in DII_5943
 * 00b5f7aad3d989: Post-migration driver recovery
 */
#define BPM_DRM_MM_FOR_EACH_NODE_IN_RANGE_SAFE_NOT_PRESENT

/*
 * Add macro to disable luminance range info backlight changes
 * Introduced in DII_6152
 * 7706b76ec9090b Backport couple of fixes for dpcd controlled backlight
 */
#define BPM_DRM_LUMINANCE_RANGE_INFO_NOT_PRESENT

/*
 * Add macro to disable DGLUT 24bit support for MTL+ onwards
 * Introduced in DII_6514
 * a82ae9f6b7d716 Support 24 bit DGLUT for MTL+
 */
#define BPM_DGLUT_24BIT_MTL_NOT_SUPPORTED

#if ((LINUX_VERSION_IS_GEQ(5,14,0) || IS_ENABLED(CPTCFG_BUILD_I915)) && \
	!(LINUX_VERSION_IS_GEQ(6,4,0)))
/*
 * Introduced in DII_6885
 * 55aab652a8a5 Backport DSC YUV420 patches
 */
#define BPM_DRM_DP_DSC_SINK_SUPPORTS_FORMAT_NOT_PRESENT
#endif /* (REDHAT_RELEASE_VERSION_IS_LESS(8,8)) */ 

/* Align Header path between i915-include and drm-include */
#define BPM_HEADER_PATH_ALIGN

#if (REDHAT_RELEASE_VERSION_IS_LESS(9,0) || \
	CUSTOM_KERN_1_RELEASE_VERSION_IS_GEQ(8,6656))
/* TBD : Need to check further need of ATTR Macro */
#define BPM_DEVICE_ATTR_NOT_PRESENT
#endif /* (REDHAT_RELEASE_VERSION_IS_LESS(9,0) || CUSTOM_KERN_1_RELEASE_VERSION_IS_GEQ(8,6656)) */

#if (REDHAT_RELEASE_VERSION_IS_RANGE(8,4, 9,0) || \
        SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0))
#define BPM_BP_MTD_MAGIC_NUMBER
#endif /* (REDHAT_RELEASE_VERSION_IS_RANGE(8,4, 9,0) || SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0)) */

#if (SUSE_RELEASE_VERSION_IS_LESS(1,15,4,0) || \
	REDHAT_RELEASE_VERSION_IS_LEQ(8,3) || \
	LINUX_VERSION_IN_RANGE(5,4,0, 5,5,0))
/*
 * 8117ab508f9c476 seqlock: seqcount_LOCKNAME_t: Introduce PREEMPT_RT support
 */
#define BPM_SEQCOUNT_MUTEX_INIT_NOT_PRESENT
#endif /* (SUSE_RELEASE_VERSION_IS_LESS(1,15,4,0) || REDHAT_RELEASE_VERSION_IS_LEQ(8,3)) ... */

#if (SUSE_RELEASE_VERSION_IS_LESS(1,15,3,0) || \
	REDHAT_RELEASE_VERSION_IS_LEQ(8,3) || \
	LINUX_VERSION_IN_RANGE(5,4,0, 5,5,0))
#define BPM_MMU_INTERVAL_NOTIFIER_NOTIFIER_NOT_PRESENT
#define BPM_DRM_MIPI_DSI_DISABLED
/* __kmalloc is not exported only in sp2 */
#define BPM_KMALLOC_TRACK_CALLER_NOT_EXPORTED
#endif /* (SUSE_RELEASE_VERSION_IS_LESS(1,15,3,0) || REDHAT_RELEASE_VERSION_IS_LEQ(8,3)) ... */

/*
 * SUSE
 */

#if SUSE_RELEASE_VERSION_IS_LESS(1,15,4,0)
/*SLES 15sp3 is based on MFD, it doesn't support AUX bus and
 RC6 related changes are not present in mei. */
#define BPM_RC6_DISABLED
#endif /* SUSE_RELEASE_VERSION_IS_LESS(1,15,4,0) */

#if SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0)
#define BPM_DRM_GET_PANEL_ORIENTATION_QUIRK_RENAME
#endif /* SUSE_RELEASE_VERSION_IS_GEQ(1,15,3,0) */

/*SLES15SP2 only section */
#if (SUSE_RELEASE_VERSION_IS_LESS(1,15,3,0))
/* Declaring traces are causing issue during macro expansion.
   Temporarily disable traces for SP2. */
#define BPM_DISABLE_TRACES
#endif /* (SUSE_RELEASE_VERSION_IS_LESS(1,15,3,0)) */

#if (SUSE_RELEASE_VERSION_IS_LESS(1,15,3,0) || \
    REDHAT_RELEASE_VERSION_IS_LEQ(8,3) || \
        LINUX_VERSION_IN_RANGE(5,4,0, 5,5,0))
#define BPM_DRM_MIPI_DSI_DISABLED
#endif /* (SUSE_RELEASE_VERSION_IS_LESS(1,15,3,0) || REDHAT_RELEASE_VERSION_IS_LEQ(8,3)) */

/*
 * REDHAT
 */

#if REDHAT_RELEASE_VERSION_IS_RANGE(8,4, 9,0)
#define BPM_RH_DRM_BACKPORT_MMU_NOTIFIER_WRAPPER
#ifdef BPM_RH_DRM_BACKPORT_MMU_NOTIFIER_WRAPPER
#if REDHAT_RELEASE_VERSION_IS_LEQ(8,5)
#define BPM_RH_DRM_BACKPORT_MMU_NOTIFIER_WRAPPER_1
#endif /* REDHAT_RELEASE_VERSION_IS_LEQ(8,5) */
#endif /* BPM_RH_DRM_BACKPORT_MMU_NOTIFIER_WRAPPER */
#endif /* REDHAT_RELEASE_VERSION_IS_RANGE(8,4, 9,0) */

#if (REDHAT_RELEASE_VERSION_IS_LEQ(8,3))
#define BPM_MMU_NOTIFIER_EVENT_NOT_PRESENT
#define BPM_I915_MMU_OBJECT_NOT_PRESENT
#endif /* (REDHAT_RELEASE_VERSION_IS_LEQ(8,3)) */

/*
 * MISCELLANEOUS
 */
/* TBD: Need to check if its generic or controllable with version */
#define BPM_PTRACE_MAY_ACCESS_NOT_PRESENT

#define BPM_ADLP_A0_PART_DISABLE

/*Adds backport debug prints for verification*/
#define BPM_ADD_DEBUG_PRINTS_BKPT_MOD
#define BPM_ADD_MODULE_VERSION_MACRO_IN_ALL_MOD

/* Reverts plane color and CSC features */
#define BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
#define BPM_DRM_PLANE_ATTACH_CTM_PROPERTY_API_PRESENT

/* To control trace include path for backports */
#define BPM_CHANGE_TRACE_INCLUDE_PATH

/* To control shmem_fs.h header file inclusion */
#define BPM_SHMEM_FS_H_NOT_INCLUDED

/* Remove traces */
#define BPM_REMOVE_TRACES

#define BPM_FAKE_DEVM_DRM_RELEASE_ACTION

/* To control dma-heap module */
#define BPM_DMA_HEAP_INIT_AS_MODULE_INIT

#if IS_ENABLED(CONFIG_AUXILIARY_BUS)
/*
 * Switch MEI between <linux/mei_aux.h> and  <linux/platform_device.h>
 */
#define BPM_MEI_AUX_BUS_AVAILABLE
#endif /* IS_ENABLED(CONFIG_AUXILIARY_BUS) */

/*
 * Macros which are needed only for dma/dmabuf compilation
 * Needed only for KV < 5.14
 */
#if LINUX_VERSION_IS_LESS(5,14,0)
#define BPM_LOWMEM_FOR_DG1_NOT_SUPPORTED
/* Resolve issues of dma-buf and add to compat module */
#define BPM_DMA_BUF_MOVE_FOPS_TO_DENTRY_OPS
#define BPM_INTEL_MEI_PXP_GSC_ASSUME_ALWAYS_ENABLED
#define BPM_TRACE_INCLUDE_PATH_NOT_PRESENT
#endif /* LINUX_VERSION_IS_LESS(5,14,0) */

/*
 * Enable NODRM macros
 */
#if LINUX_VERSION_IS_LESS(5,14,0) && IS_ENABLED(CPTCFG_BUILD_I915)
#define BPM_API_ARG_DRM_DRIVER_REMOVED
#define BPM_DRM_PAYLOAD_PART1_START_SLOT_NOT_PRESENT
#define BPM_EDID_HDMI_RGB444_DC_MODES_NOT_PRESENT
#endif
#ifdef CPTCFG_BUILD_I915
#define BPM_DISABLE_DRM_DMABUF
#else
/* Add backport Macro for all the symbols of dma-buf. */
#define BPM_ADD_BACKPORT_MACRO_TO_DMA_BUF_SYMBOLS
#endif /* CPTCFG_BUILD_I915 */
#endif /* _BP_LINUX_BACKPORT_MACRO_H */
