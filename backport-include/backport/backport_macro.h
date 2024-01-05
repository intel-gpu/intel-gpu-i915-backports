#ifndef _BP_LINUX_BACKPORT_MACRO_H
#define _BP_LINUX_BACKPORT_MACRO_H
#include <linux/version.h>
#include <linux/kconfig.h>
#include <backport/autoconf.h>

#if (LINUX_VERSION_IS_GEQ(6,4,5) || \
		LINUX_VERSION_IN_RANGE(6,1,42, 6,2,0) || \
		(LINUX_VERSION_IN_RANGE(6,2,16, 6,3,0) && UBUNTU_RELEASE_VERSION_IS_GEQ(36,37)) || \
		(SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0) && SUSE_LOCAL_VERSION_IS_GEQ(55,19)) || \
		REDHAT_RELEASE_VERSION_IS_GEQ(9,3))
/*
 * 104d79eb58aa
 * drm/dp_mst: Clear MSG_RDY flag before sending new message
 */
#define BPM_DRM_DP_MST_HPD_IRQ_IS_NOT_PRESENT
#endif

#if (LINUX_VERSION_IS_GEQ(6,3,0) || \
		REDHAT_RELEASE_VERSION_IS_GEQ(9,3))
/*
 * a3185f91d057 drm/ttm: merge ttm_bo_api.h and ttm_bo_driver.h v2
 */
#define BPM_TTM_BO_API_H_NOT_PRESENT

/*
 * 5e7b9a6ae8c3 swiotlb: remove swiotlb_max_segment
 */
#define BPM_SWIOTLB_MAX_SEGMENT_NOT_PRESENT

/*
 * 80ed86d4b6d7 drm/connector: Rename drm_mode_create_tv_properties
 */
#define BPM_DRM_MODE_CREATE_TV_PROP_NOT_PRESENT

/*
 * 6c80a93be62d drm/fb-helper: Initialize fb-helper's preferred BPP in prepare function
 */
#define BPM_DRM_FB_PREPARE_AND_INITIAL_CFG_NOT_PRESENT
#endif

#if (LINUX_VERSION_IS_GEQ(6,2,0) || \
		REDHAT_RELEASE_VERSION_IS_GEQ(9,3))
/*
 * e3c92eb4a84fb
 * drm/ttm: rework on ttm_resource to use size_t type
 */
#define BPM_STRUCT_TTM_RESOURCE_NUM_PAGES_NOT_PRESENT

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
 *  drm/fb_helper: Rename field fbdev to info in struct drm_fb_helper
 */
#define BPM_STRUCT_DRM_FB_HELPER_FBDEV_NOT_PRESENT
#endif

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

/*
 * 6e1ca48d0669b
 * folio-compat: remove lru_cache_add()
 */
#define BPM_LRU_CACHE_ADD_API_NOT_PRESENT
#endif

#if (LINUX_VERSION_IS_GEQ(6,1,0) || \
		REDHAT_RELEASE_VERSION_IS_GEQ(9,3))
/*
 * cce32e4e38c6
 * drm/atomic-helper: Remove _HELPER_ infix from DRM_PLANE_HELPER_NO_SCALING
 */
#define BPM_DRM_PLANE_HELPER_NO_SCALING_NOT_PRESENT
#endif

#if (LINUX_VERSION_IS_GEQ(6,1,0))
/*
 * de492c83cae prandom: remove unused functions
 */
#define BPM_GET_RANDOM_INT_NOT_PRESENT

/*
 * f683b9d61319 i915: use the VMA iterator
 */
#define BPM_STRUCT_VM_AREA_STRUCT_VM_NEXT_NOT_PRESENT
#endif

#if (LINUX_VERSION_IS_GEQ(6,1,0) || \
		(SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0) && SUSE_LOCAL_VERSION_IS_GEQ(55,7)) || \
		REDHAT_RELEASE_VERSION_IS_GEQ(9,3))
/*
 * 4d07b0bc40
 * drm/display/dp_mst: Move all payload info into the atomic state
 */
#define BPM_DRM_DP_MST_PORT_VCPI_NOT_PRESENT
#endif

#if (LINUX_VERSION_IS_LESS(6,1,0))

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(9,3))

/*
 * 3cea8d4753 lib: add find_nth{,_and,_andnot}_bit()
 */
#define BPM_FIND_NTH_BIT_PRESENT
#endif
#endif

#if (LINUX_VERSION_IS_GEQ(6,0,0) || \
		REDHAT_RELEASE_VERSION_IS_GEQ(9,3))
/*
 * e33c267ab70d
 * mm: shrinkers: provide shrinkers with names
 */
#define BPM_REGISTER_SHRINKER_SECOND_ARG_NOT_PRESENT
#endif 

#if (LINUX_VERSION_IS_GEQ(6,0,0))

#endif
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

#endif

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
#endif

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
 * f7fd7814f34c drm/i915: Remove dma_resv_prune
 */
#define BPM_DMA_RESV_PRUNE_NOT_PRESENT

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

#endif

#if (LINUX_VERSION_IS_GEQ(5,19,0) || \
                REDHAT_RELEASE_VERSION_IS_GEQ(9,2))
/*
 * c4f135d64382 workqueue: Wrap flush_workqueue() using a macro
 */
#define BPM_FLUSH_WQ_WITH_WARN_WRAPPER_PRESENT

#endif

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

#endif

#if (LINUX_VERSION_IS_GEQ(5,18,0) || \
		REDHAT_RELEASE_VERSION_IS_GEQ(9,1) || \
		SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0))

/*
 * 4a46e5d251a39e7c10
 * drm/edid: Rename drm_hdmi_avi_infoframe_colorspace to _colorimetry
 */
#define BPM_DRM_HDMI_AVI_INFOFRAME_COLORSPACE_NOT_PRESENT

#endif

#if LINUX_VERSION_IS_GEQ(5,18,0)
/*
 * 7968778914e53788a
 * PCI: Remove the deprecated "pci-dma-compat.h" API
 */
#define BPM_PCI_DMA_COMPAT_H_NOT_PRESENT

#endif

#if LINUX_VERSION_IS_GEQ(5,18,0) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0)
/*
 * 730ff52194cdb324
 * mm: remove pointless includes from <linux/hmm.h>
 *
 */

#define BPM_MIGRATE_AND_MEMREMAP_NOT_PRESENT

#endif

#if (LINUX_VERSION_IS_GEQ(5,18,0) || \
		REDHAT_RELEASE_VERSION_IS_GEQ(9,1) || \
		SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0))
/*
 * 7938f4218168ae9f
 * dma-buf-map: Rename to iosys-map
 */
#define BPM_IOSYS_MAP_PRESENT

#endif /* LINUX_VERSION_IS_GEQ(5,18,0) */

#if (LINUX_VERSION_IN_RANGE(5,17,0, 5,19,0) || \
		REDHAT_RELEASE_VERSION_IS_EQL(9,1))

/*
 * 6b41323a265a02b dma-buf: rename dma_resv_get_excl_rcu to _unlocked
 */
#define DMA_RESV_EXCL_UNLOCKED_NOT_PRESENT

#endif

#if (LINUX_VERSION_IS_GEQ(5,17,2) || \
	(LINUX_VERSION_IN_RANGE(5,17,0, 5,17,2) && UBUNTU_RELEASE_VERSION_IS_GEQ(1004,4)) || \
	LINUX_VERSION_IN_RANGE(5,15,33, 5,16,0) || \
	(LINUX_VERSION_IN_RANGE(5,14,0, 5,15,0) && UBUNTU_RELEASE_VERSION_IS_GEQ(1035,38)) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,1) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0))

/*
 * 662b372a8a72695d drm/edid: Split deep color modes between RGB and YUV444
 *
 * Introduced in 5.17.2 and backported to LTS kernel 5.15.33 as well as
 * backported in Ubuntu oem 5.17.0-1004.4 and 5.14.0-1035.38.
 */
#define EDID_HDMI_RGB444_DC_MODES_PRESENT

#endif

#if (LINUX_VERSION_IS_LESS(5,18,0) && \
		!(REDHAT_RELEASE_VERSION_IS_GEQ(9,1) || \
		SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0)))
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
#define BPM_IOSYS_MAP_RENAME_APIS
#endif

#if (LINUX_VERSION_IS_GEQ(5,17,0) || \
		REDHAT_RELEASE_VERSION_IS_GEQ(9,1) || \
		SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0))
/*
 * 6a2d2ddf2c345e0 drm: Move nomodeset kernel parameter to the DRM subsystem
 */
#define VGACON_TEXT_FORCE_NOT_PRESENT
#endif

#if (LINUX_VERSION_IS_GEQ(5,17,0) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,2))
/*
 * d122019bf061c mm: Split slab into its own type
 *
 */
#define FOLIO_ADDRESS_PRESENT

/*
 * bb192ed9aa719 mm/slub: Convert most struct page to struct slab by spatch
 *
 */
#define COUNT_STRUCT_SLAB_PRESENT

/*
 * ec288a2cf7ca40a9 bitmap: unify find_bit operations
 */
#define BITMAP_FOR_REGION_NOT_PRESENT

#endif

#if LINUX_VERSION_IS_GEQ(5,17,0)

/*
 * 502fee2499277c drm/i915/dp: Use the drm helpers for getting max FRL rate.
 */
#define MAX_FLR_NOT_PRESENT

#endif

#if LINUX_VERSION_IS_LESS(5,17,0)
/*
 * 2d8b5b3b9e40f7 drm/i915/dp: use new link training delay helpers
 * 
 * Required DRM changes are not present in KV < 5.17 so modified code
 * to follow previous implementation.
 */
#define DP_LINK_TRAINING_CR_DELAY_PRESENT

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(9,2))
/*
 * 781b2ba6eb5f2 SLUB: Out-of-memory diagnostics
 *
 */
#define COUNT_STRUCT_PAGE_PRESENT

#endif

/*
 * f58a435311672 drm/dp, drm/i915: Add support for VESA backlights using PWM for brightness control
 *
 */
#define DRM_EDP_BACKLIGHT_NOT_PRESENT

#if !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,4,0) || \
		REDHAT_RELEASE_VERSION_IS_GEQ(9,1))

/*
 * 365481e42a8a driver core: auxiliary bus: Add driver data helpers
 */
#define BPM_AUXILIARY_BUS_HELPERS_NOT_PRESENT
#endif
#endif

#if LINUX_VERSION_IS_GEQ(5,16,0) || REDHAT_RELEASE_VERSION_IS_GEQ(9,0) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0)

/*
 * 16b0314aa746be dma-buf: move dma-buf symbols into the DMA_BUF module namespace
 */ 
#define MODULE_IMPORT_NS_SUPPORT
#endif

#if (LINUX_VERSION_IS_GEQ(5,16,0) || \
		REDHAT_RELEASE_VERSION_IS_GEQ(9,1) || \
		SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0))
/*
 * d6c6a76f80a1c drm: Update MST First Link Slot Information Based on Encoding Format
 */ 
#define DRM_PAYLOAD_PART1_START_SLOT_PRESENT

#endif

#if (LINUX_VERSION_IS_GEQ(5,16,0) || \
		REDHAT_RELEASE_VERSION_IS_GEQ(9,2) || \
		SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0))
/*
 * ab09243aa95a7 mm/migrate.c: remove MIGRATE_PFN_LOCKED
 */
#define MIGRATE_PFN_LOCKED_REMOVED

#endif

#if LINUX_VERSION_IS_LESS(5,15,46)
#if !((SUSE_RELEASE_VERSION_IS_GEQ(1,15,4,0) && \
	!(SUSE_LOCAL_VERSION_IS_LESS(24,11))) || \
        UBUNTU_RELEASE_VERSION_IS_GEQ(20,04) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(9,3))
/*
 * 0425473037db list: introduce list_is_head() helper and re-use it in list.h
 */
#define BPM_LIST_IS_HEAD_NOT_PRESENT
#endif
#endif

#if LINUX_VERSION_IS_LESS(5,16,0)

/*
 * c78b4a85721f3 drm/dp: add helper for extracting adjust 128b/132b TX FFE preset
 *
 */

#define DRM_DP_GET_ADJUST_NOT_PRESENT

/*
 * 103c7044be5b207 drm/i915/edp: use MSO pixel overlap from DisplayID data
 *
 */
#define MSO_PIXEL_OVERLAP_DISPLAY_NOT_PRESENT

#endif

#if (LINUX_VERSION_IS_GEQ(5,15,0) || \
		REDHAT_RELEASE_VERSION_IS_GEQ(9,1) || \
		SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0))

/* 
 * ac1723c16b drm/i915: Track IRQ state in local device state.
 */
#define DRM_DEVICE_IRQ_ENABLED_INSIDE_LEGACY_ADDED

#endif

#if (LINUX_VERSION_IS_GEQ(5,15,0) || REDHAT_RELEASE_VERSION_IS_GEQ(8,7) || \
		SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0))
/*
 * 279cc2e9543eb drm: Define DRM_FORMAT_MAX_PLANES
 *
 * Required DRM changes are not present in KV < 5.15.
 * Maintaining header drm_fourcc.h to compat,So add this feature to support KV >5.15.
 */
#define DRM_FORMAT_MAX_PLANES_ADDED

#endif

#if LINUX_VERSION_IS_LESS(5,15,0)

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(9,1) || \
		SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0))

/* 
 * bf44e8cecc03 vgaarb: don't pass a cookie to vga_client_register
 * f6b1772b2555 vgaarb: remove the unused irq_set_state argument to vga_client_register
 */
#define VGA_SET_DECODE_ARG_PCI_DEV_NOT_PRESENT

/*
 * b8779475869a vgaarb: provide a vga_client_unregister wrapper
 */
#define VGA_CLIENT_UNREGISTER_NOT_PRESENT

#endif

#if !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,4,0))

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(9,1))
/* 
 * 97c9bfe3f660 drm/aperture: Pass DRM driver structure instead of driver name
 *
 */
#define API_ARG_DRM_DRIVER_REMOVED

/*
 * 440d0f12b52a dma-buf: add dma_fence_chain_alloc/free v3
 *
 * Took partial i915 patch from KV 5.15.0
 *
 */
#define DMA_FENCE_CHAIN_ALLOC_NOT_PRESENT
#endif

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(9,2))
/*
 * fc7a620 bus: Make remove callback return void
 *
 * In file bus.h in bus_type sturct, return type of remove function
 * changed from int to void
 */
#define BPM_BUS_REMOVE_FUNCTION_RETURN_TYPE_CHANGED

#endif

#if !(REDHAT_RELEASE_VERSION_IS_GEQ(8,6))

/*
 *6f2beb268a5d swiotlb: Update is_swiotlb_active to add a struct device argument
 *
 */
#define IS_SWIOTLB_ACTIVE_ARG_DEV_NOT_PRESENT

#endif

/*
 * f0ab00174eb7 PCI: Make saved capability state private to core
 */
#define PCI_INTERFACES_NOT_PRESENT

#endif


#if (SUSE_RELEASE_VERSION_IS_GEQ(1,15,4,0))
/*
 * 867cf9cd73c3d drm/dp: Extract i915's eDP backlight code into DRM helpers
 */
#define DRM_EDP_BACKLIGHT_SUPPORT_PRESENT
#endif

#if !((LINUX_VERSION_IN_RANGE(5,14,0, 5,15,0) && UBUNTU_RELEASE_VERSION_IS_GEQ(1011,0)) || \
	REDHAT_RELEASE_VERSION_IS_GEQ(8,7) || \
	SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0))
/*
 * 59dc33252ee7 PCI: VMD: ACPI: Make ACPI companion lookup work for VMD bus
 */
#define BPM_PCI_FIND_HOST_BRIDGE_NOT_EXPORTED
#endif
#endif

#if LINUX_VERSION_IS_GEQ(5,14,0)

/* 
 * bd99b4fd9164267 drm/i915/gt: Flush GT interrupt handler before changing interrupt state
 * Update to utilize intel_synchronize_irq instead of old synchronize_hardirq API
 */
#define SYNC_HRQ_NOT_PRESENT

/* TBD: Need to check if its generic or controllable with version */
#define BPM_PTRACE_MAY_ACCESS_NOT_PRESENT
#endif

/*
 *  Upstream Patches not merged in any kernel yet
 */

/*
 * Introduced in DII_5943
 * 00b5f7aad3d989: Post-migration driver recovery
 */

#define DRM_MM_FOR_EACH_NODE_IN_RANGE_SAFE_NOT_PRESENT

/*
 * Add macro to disable luminance range info backlight changes
 * Introduced in DII_6152
 * 7706b76ec9090b Backport couple of fixes for dpcd controlled backlight
 */
#define DRM_LUMINANCE_RANGE_INFO_NOT_PRESENT

/*
 * Add macro to disable DGLUT 24bit support for MTL+ onwards 
 * Introduced in DII_6514
 * a82ae9f6b7d716 Support 24 bit DGLUT for MTL+
 */
#define BPM_DGLUT_24BIT_MTL_NOT_SUPPORTED

/*
 * Introduced in DII_6042
 * 9299148acf5422 VFIO - SR-IOV VF migration
 */
#define BPM_VFIO_SR_IOV_VF_MIGRATION_NOT_PRESENT

/*
 * Introduced in DII_6885
 * 55aab652a8a5 Backport DSC YUV420 patches
 */
#define BPM_DRM_DP_DSC_SINK_SUPPORTS_FORMAT_NOT_PRESENT

#if IS_ENABLED(CONFIG_AUXILIARY_BUS)
/* 
 * Added macro for MEI to switch between <linux/mei_aux.h> and  
 * <linux/platform_device.h> depending on which one is available 
 */
#define BPM_MEI_AUX_BUS_AVAILABLE
#endif

/* SLES15SP5 section only */
#if SUSE_RELEASE_VERSION_IS_GEQ(1,15,5,0)
/*
 * 4dea97f8636d
 * lib/bitmap: change type of bitmap_weight to unsigned long
 */
#define BPM_BITMAP_WEIGHT_RETURN_TYPE_CHANGED
#endif

#endif /* _BP_LINUX_BACKPORT_MACRO_H */
