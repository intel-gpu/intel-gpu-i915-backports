#ifndef __BACKPORT_DRM_FOURCC_H
#define __BACKPORT_DRM_FOURCC_H

#include_next <uapi/drm/drm_fourcc.h>

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * Intel modifiers for new platforms should be added using the PRELIM_ prefix
 * and the intel_prelim_fourcc_mod_code macro, while the upstreaming of the
 * platform should happen without the prefix using the fourcc_mod_code macro.
 */
#define INTEL_PRELIM_ID_FLAG         (1ULL << 55)

#define intel_prelim_fourcc_mod_code(val) \
        (fourcc_mod_code(INTEL, (val)) | INTEL_PRELIM_ID_FLAG)

/*
 * Intel Color Control Surface with Clear Color (CCS) for Gen-12 render
 * compression.
 *
 * The main surface is Y-tiled and is at plane index 0 whereas CCS is linear
 * and at index 1. The clear color is stored at index 2, and the pitch should
 * be ignored. The clear color structure is 256 bits. The first 128 bits
 * represents Raw Clear Color Red, Green, Blue and Alpha color each represented
 * by 32 bits. The raw clear color is consumed by the 3d engine and generates
 * the converted clear color of size 64 bits. The first 32 bits store the Lower
 * Converted Clear Color value and the next 32 bits store the Higher Converted
 * Clear Color value when applicable. The Converted Clear Color values are
 * consumed by the DE. The last 64 bits are used to store Color Discard Enable
 * and Depth Clear Value Valid which are ignored by the DE. A CCS cache line
 * corresponds to an area of 4x1 tiles in the main surface. The main surface
 * pitch is required to be a multiple of 4 tile widths.
 */
#define I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS_CC fourcc_mod_code(INTEL, 8)

/*
 * Intel Tile 4 layout
 *
 * This is a tiled layout using 4KB tiles in a row-major layout. It has the same
 * shape as Tile Y at two granularities: 4KB (128B x 32) and 64B (16B x 4). It
 * only differs from Tile Y at the 256B granularity in between. At this
 * granularity, Tile Y has a shape of 16B x 32 rows, but this tiling has a shape
 * of 64B x 8 rows.
 */
#define I915_FORMAT_MOD_4_TILED         fourcc_mod_code(INTEL, 9)
/*
 * Intel color control surfaces (CCS) for DG2 render compression.
 *
 * DG2 uses a new compression format for render compression. The general
 * layout is the same as I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS,
 * but a new hashing/compression algorithm is used, so a fresh modifier must
 * be associated with buffers of this type. Render compression uses 128 byte
 * compression blocks.
 */

#define I915_FORMAT_MOD_4_TILED_DG2_RC_CCS fourcc_mod_code(INTEL, 10)
#define PRELIM_I915_FORMAT_MOD_4_TILED_DG2_RC_CCS intel_prelim_fourcc_mod_code(13)

/*
 * Intel color control surfaces (CCS) for DG2 media compression.
 *
 * DG2 uses a new compression format for media compression. The general
 * layout is the same as I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS,
 * but a new hashing/compression algorithm is used, so a fresh modifier must
 * be associated with buffers of this type. Media compression uses 256 byte
 * compression blocks.
 */

#define I915_FORMAT_MOD_4_TILED_DG2_MC_CCS fourcc_mod_code(INTEL, 11)
#define PRELIM_I915_FORMAT_MOD_4_TILED_DG2_MC_CCS intel_prelim_fourcc_mod_code(14)

/*
 * Intel color control surfaces (CCS) for DG2 clear color render compression.
 *
 * DG2 uses a unified compression format for clear color render compression.
 * The general layout is a tiled layout using 4Kb tiles i.e. Tile4 layout.
 */

#define I915_FORMAT_MOD_4_TILED_DG2_RC_CCS_CC fourcc_mod_code(INTEL, 12)
#define PRELIM_I915_FORMAT_MOD_4_TILED_DG2_RC_CCS_CC intel_prelim_fourcc_mod_code(15)

/*
 * Intel color control surfaces (CCS) for display ver 14 render compression.
 *
 * The main surface is tile4 and at plane index 0, the CCS is linear and
 * at index 1. A 64B CCS cache line corresponds to an area of 4x1 tiles in
 * main surface. In other words, 4 bits in CCS map to a main surface cache
 * line pair. The main surface pitch is required to be a multiple of four
 * tile4 widths.
 */
#define PRELIM_I915_FORMAT_MOD_4_TILED_MTL_RC_CCS intel_prelim_fourcc_mod_code(16)

/*
 * Intel color control surfaces (CCS) for display ver 14 media compression
 *
 * The main surface is tile4 and at plane index 0, the CCS is linear and
 * at index 1. A 64B CCS cache line corresponds to an area of 4x1 tiles in
 * main surface. In other words, 4 bits in CCS map to a main surface cache
 * line pair. The main surface pitch is required to be a multiple of four
 * tile4 widths. For semi-planar formats like NV12, CCS planes follow the
 * Y and UV planes i.e., planes 0 and 1 are used for Y and UV surfaces,
 * planes 2 and 3 for the respective CCS.
 */
#define PRELIM_I915_FORMAT_MOD_4_TILED_MTL_MC_CCS intel_prelim_fourcc_mod_code(17)

/*
 * Intel Color Control Surface with Clear Color (CCS) for display ver 14 render
 * compression.
 *
 * The main surface is tile4 and is at plane index 0 whereas CCS is linear
 * and at index 1. The clear color is stored at index 2, and the pitch should
 * be ignored. The clear color structure is 256 bits. The first 128 bits
 * represents Raw Clear Color Red, Green, Blue and Alpha color each represented
 * by 32 bits. The raw clear color is consumed by the 3d engine and generates
 * the converted clear color of size 64 bits. The first 32 bits store the Lower
 * Converted Clear Color value and the next 32 bits store the Higher Converted
 * Clear Color value when applicable. The Converted Clear Color values are
 * consumed by the DE. The last 64 bits are used to store Color Discard Enable
 * and Depth Clear Value Valid which are ignored by the DE. A CCS cache line
 * corresponds to an area of 4x1 tiles in the main surface. The main surface
 * pitch is required to be a multiple of 4 tile widths.
 */
#define PRELIM_I915_FORMAT_MOD_4_TILED_MTL_RC_CCS_CC intel_prelim_fourcc_mod_code(18)

#if defined(__cplusplus)
}
#endif

#endif /* __BACKPORT_DRM_FOURCC_H */
