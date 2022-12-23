#ifndef _BP_LINUX_BACKPORT_MACRO_H
#define _BP_LINUX_BACKPORT_MACRO_H
#include <linux/version.h>

#if LINUX_VERSION_IS_GEQ(5,17,2) || \
       (LINUX_VERSION_IN_RANGE(5,17,0, 5,17,2) && \
       UBUNTU_BACKPORT_RELEASE_CODE >= UBUNTU_BACKPORT_RELEASE_VERSION(1004,4))\
       || LINUX_VERSION_IN_RANGE(5,15,33, 5,16,0) \
       || (LINUX_VERSION_IN_RANGE(5,14,0, 5,15,0) && \
       UBUNTU_BACKPORT_RELEASE_CODE >= UBUNTU_BACKPORT_RELEASE_VERSION(1035,38))

/* 
 * 662b372a8a72695d drm/edid: Split deep color modes between RGB and YUV444
 *
 * Introduced in 5.17.2 and backported to LTS kernel 5.15.33 as well as
 * backported in Ubuntu oem 5.17.0-1004.4 and 5.14.0-1035.38.
 */
#define EDID_HDMI_RGB444_DC_MODES_PRESENT

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
 * 348332e00069 mm: don't include <linux/blk-cgroup.h> in <linux/writeback.h>
 *
 * Took partial i915 patch from KV 5.17.0
 */
#define LINUX_SCHED_CLOCK_H_ADDED

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

#if LINUX_VERSION_IS_GEQ(5,15,0)

/* 
 * ac1723c16b drm/i915: Track IRQ state in local device state.
 */
#define DRM_DEVICE_IRQ_ENABLED_INSIDE_LEGACY_ADDED

#endif

#if LINUX_VERSION_IS_LESS(5,15,0)

/*
 * 90e7a6de62781c lib/scatterlist: Provide a dedicated function to support table append
 * 
 * Required DRM changes are not present in KV < 5.15 so modified code
 * to follow previous implementation.
 */
#define SG_ALLOC_TABLE_FROM_PAGES_SEGMENT_NOT_PRESENT

/* 
 *6f2beb268a5d swiotlb: Update is_swiotlb_active to add a struct device argument
 */
#define IS_SWIOTLB_ACTIVE_ARG_DEV_NOT_PRESENT

/* 
 * bf44e8cecc03 vgaarb: don't pass a cookie to vga_client_register
 * f6b1772b2555 vgaarb: remove the unused irq_set_state argument to vga_client_register
 */
#define VGA_SET_DECODE_ARG_PCI_DEV_NOT_PRESENT

/*
 * b8779475869a vgaarb: provide a vga_client_unregister wrapper
 */
#define VGA_CLIENT_UNREGISTER_NOT_PRESENT

/* 
 * 97c9bfe3f660 drm/aperture: Pass DRM driver structure instead of driver name
 */
#define API_ARG_DRM_DRIVER_REMOVED

/*
 * 440d0f12b52a dma-buf: add dma_fence_chain_alloc/free v3
 *
 * Took partial i915 patch from KV 5.15.0
 */
#define DMA_FENCE_CHAIN_ALLOC_NOT_PRESENT

/*
 * 103c7044be5b207 drm/i915/edp: use MSO pixel overlap from DisplayID data
 *
 * Required DRM changes are not present in KV < 5.15 so modified code
 * to follow previous implementation.
 */ 
#define MSO_PIXEL_OVERLAP_DISPLAY_NOT_PRESENT

#endif

#if LINUX_VERSION_IN_RANGE(5,15,0, 5,16,0)

#define MSO_PIXEL_OVERLAP_DISPLAY_NOT_PRESENT

#endif

#if LINUX_VERSION_IS_GEQ(5,14,0)

/* 
 * bd99b4fd9164267 drm/i915/gt: Flush GT interrupt handler before changing interrupt state
 * Update to utilize intel_synchronize_irq instead of old synchronize_hardirq API
 * VLK-32720
 */
#define SYNC_HRQ_NOT_PRESENT
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

#endif /* _BP_LINUX_BACKPORT_MACRO_H */
