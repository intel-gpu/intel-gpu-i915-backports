#ifndef _BP_LINUX_BACKPORT_MACRO_H
#define _BP_LINUX_BACKPORT_MACRO_H
#include <linux/version.h>
#include <backport/autoconf.h>

#if LINUX_VERSION_IS_GEQ(5,19,0)

/*
 * da68386d9edb1f57a drm: Rename dp/ to display/
 */
#define BPM_DRM_DP_HELPER_DIR_DISPLAY_PRESENT

#elif LINUX_VERSION_IN_RANGE(5,18,0, 5,19,0)

/*
 * 5b529e8d9c387a34 drm/dp: Move public DisplayPort headers into dp/
 */
#define BPM_DRM_DP_HELPER_DIR_DP_PRESENT

#endif /*  LINUX_VERSION_IN_RANGE(5,18,0, 5,19,0) */

#if LINUX_VERSION_IS_GEQ(5,18,0)

/*
 * 4a46e5d251a39e7c10
 * drm/edid: Rename drm_hdmi_avi_infoframe_colorspace to _colorimetry
 */
#define BPM_DRM_HDMI_AVI_INFOFRAME_COLORSPACE_NOT_PRESENT

/*
 * 7968778914e53788a
 * PCI: Remove the deprecated "pci-dma-compat.h" API
 */
#define BPM_PCI_DMA_COMPAT_H_NOT_PRESENT

/*
 * 730ff52194cdb324
 * mm: remove pointless includes from <linux/hmm.h>
 *
 */

#define BPM_MIGRATE_AND_MEMREMAP_NOT_PRESENT

/*
 * 7938f4218168ae9f
 * dma-buf-map: Rename to iosys-map
 */
#define BPM_IOSYS_MAP_PRESENT

#endif /* LINUX_VERSION_IS_GEQ(5,18,0) */


#if LINUX_VERSION_IS_GEQ(5,17,2) || \
	(LINUX_VERSION_IN_RANGE(5,17,0, 5,17,2) && UBUNTU_RELEASE_VERSION_IS_GEQ(1004,4)) || \
	LINUX_VERSION_IN_RANGE(5,15,33, 5,16,0) || \
	(LINUX_VERSION_IN_RANGE(5,14,0, 5,15,0) && UBUNTU_RELEASE_VERSION_IS_GEQ(1035,38))

/*
 * 662b372a8a72695d drm/edid: Split deep color modes between RGB and YUV444
 *
 * Introduced in 5.17.2 and backported to LTS kernel 5.15.33 as well as
 * backported in Ubuntu oem 5.17.0-1004.4 and 5.14.0-1035.38.
 */
#define EDID_HDMI_RGB444_DC_MODES_PRESENT

#endif

#if LINUX_VERSION_IS_LESS(5,18,0)
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

#if LINUX_VERSION_IS_GEQ(5,17,0)

/*
 * ec288a2cf7ca40a9 bitmap: unify find_bit operations
 */
#define BITMAP_FOR_REGION_NOT_PRESENT

/*
 * 6a2d2ddf2c345e0 drm: Move nomodeset kernel parameter to the DRM subsystem
 */
#define VGACON_TEXT_FORCE_NOT_PRESENT

/*
 * 502fee2499277c drm/i915/dp: Use the drm helpers for getting max FRL rate.
 */
#define MAX_FLR_NOT_PRESENT

/*
 * bb192ed9aa719 mm/slub: Convert most struct page to struct slab by spatch
 *
 */
#define COUNT_STRUCT_SLAB_PRESENT

/*
 * 6b41323a265a02b dma-buf: rename dma_resv_get_excl_rcu to _unlocked
 *
 */
#define DMA_RESV_EXCL_UNLOCKED_NOT_PRESENT

/*
 * d122019bf061c mm: Split slab into its own type
 *
 */
#define FOLIO_ADDRESS_PRESENT

#endif

#if LINUX_VERSION_IS_LESS(5,17,0)
/*
 * 2d8b5b3b9e40f7 drm/i915/dp: use new link training delay helpers
 * 
 * Required DRM changes are not present in KV < 5.17 so modified code
 * to follow previous implementation.
 */
#define DP_LINK_TRAINING_CR_DELAY_PRESENT

/*
 * 781b2ba6eb5f2 SLUB: Out-of-memory diagnostics
 *
 */
#define COUNT_STRUCT_PAGE_PRESENT

/*
 * f58a435311672 drm/dp, drm/i915: Add support for VESA backlights using PWM for brightness control
 *
 */
#define DRM_EDP_BACKLIGHT_NOT_PRESENT
#endif

#if LINUX_VERSION_IS_GEQ(5,16,0)

/*
 * 16b0314aa746be dma-buf: move dma-buf symbols into the DMA_BUF module namespace
 */ 
#define MODULE_IMPORT_NS_SUPPORT

/*
 * d6c6a76f80a1c drm: Update MST First Link Slot Information Based on Encoding Format
 */ 
#define DRM_PAYLOAD_PART1_START_SLOT_PRESENT

/*
 * ab09243aa95a7 mm/migrate.c: remove MIGRATE_PFN_LOCKED
 */
#define MIGRATE_PFN_LOCKED_REMOVED

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

#if LINUX_VERSION_IS_GEQ(5,15,0)

/* 
 * ac1723c16b drm/i915: Track IRQ state in local device state.
 */
#define DRM_DEVICE_IRQ_ENABLED_INSIDE_LEGACY_ADDED

/*
 * 279cc2e9543eb drm: Define DRM_FORMAT_MAX_PLANES
 *
 * Required DRM changes are not present in KV < 5.15.
 * Maintaining header drm_fourcc.h to compat,So add this feature to support KV >5.15.
 */
#define DRM_FORMAT_MAX_PLANES_ADDED

#endif

#if LINUX_VERSION_IS_LESS(5,15,0)

/* 
 * bf44e8cecc03 vgaarb: don't pass a cookie to vga_client_register
 * f6b1772b2555 vgaarb: remove the unused irq_set_state argument to vga_client_register
 */
#define VGA_SET_DECODE_ARG_PCI_DEV_NOT_PRESENT

/*
 * b8779475869a vgaarb: provide a vga_client_unregister wrapper
 */
#define VGA_CLIENT_UNREGISTER_NOT_PRESENT

#if !(SUSE_RELEASE_VERSION_IS_GEQ(1,15,4,0))
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

/*
 *6f2beb268a5d swiotlb: Update is_swiotlb_active to add a struct device argument
 *
 */
#define IS_SWIOTLB_ACTIVE_ARG_DEV_NOT_PRESENT

/*
 * 90e7a6de62781c lib/scatterlist: Provide a dedicated function to support table append
 *
 */
#define SG_ALLOC_TABLE_FROM_PAGES_SEGMENT_NOT_PRESENT

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
 * DII changes outside i915 yet to upstream. 
 * as on DII tag 5899
 */

/*
 * 64fa30f9ffc0ed Backport and fix intel-gtt split
 */

#define INTEL_GMCH_GTT_RENAMED

/*
 * Introduced in DII_5943
 * 00b5f7aad3d989: Post-migration driver recovery
 */

#define DRM_MM_FOR_EACH_NODE_IN_RANGE_SAFE_NOT_PRESENT

/*
 * Add macro to disable HDMI21 features
 * Introduced in DII_6023
 * 623878a1e7da2c Add support for HDMI21 FRL link training
 * b00ac558fad656 Add support for CVTEM packets for HDMI21 DSC
 */
#define NATIVE_HDMI21_FEATURES_NOT_SUPPORTED

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
 * Add macro to disable HDMI2.1 VRR support
 * Introduced in DII_6556
 * 64ccfe30b7e258 Enable support for HDMI2.1 VRR
 */
#define VRR_FEATURE_NOT_SUPPORTED

#endif /* _BP_LINUX_BACKPORT_MACRO_H */
