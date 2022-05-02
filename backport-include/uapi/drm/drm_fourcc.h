#ifndef __BACKPORT_DRM_FOURCC_H
#define __BACKPORT_DRM_FOURCC_H

#include_next <uapi/drm/drm_fourcc.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* FIXME: ID #8 is reserved for TGL CCS_CC */

/*
 * Intel color control surfaces (CCS) for DG2 render compression.
 *
 * DG2 uses a new compression format for render compression. The general
 * layout is the same as I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS,
 * but a new hashing/compression algorithm is used, so a fresh modifier must
 * be associated with buffers of this type. Render compression uses 128 byte
 * compression blocks.
 */
#define I915_FORMAT_MOD_F_TILED_DG2_RC_CCS fourcc_mod_code(INTEL, 9)

/*
 * Intel color control surfaces (CCS) for DG2 media compression.
 *
 * DG2 uses a new compression format for media compression. The general
 * layout is the same as I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS,
 * but a new hashing/compression algorithm is used, so a fresh modifier must
 * be associated with buffers of this type. Media compression uses 256 byte
 * compression blocks.
 */
#define I915_FORMAT_MOD_F_TILED_DG2_MC_CCS fourcc_mod_code(INTEL, 10)

/*
 * Intel color control surfaces (CCS) for DG2 clear color render compression.
 *
 * DG2 uses a unified compression format for clear color render compression.
 * The general layout is a tiled layout using 4Kb tiles i.e. Tile4 layout.
 */
#define I915_FORMAT_MOD_F_TILED_DG2_RC_CCS_CC fourcc_mod_code(INTEL, 11)

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
 * Intel F-tiling(aka Tile4) layout
 *
 * This is a tiled layout using 4Kb tiles in row-major layout.
 * Within the tile pixels are laid out in 64 byte units / sub-tiles in OWORD
 * (16 bytes) chunks column-major..
 */
#define I915_FORMAT_MOD_F_TILED         fourcc_mod_code(INTEL, 12)

/*
 * Intel color control surfaces (CCS) for DG2 render compression.
 *
 * DG2 uses a new compression format for render compression. The general
 * layout is the same as I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS,
 * but a new hashing/compression algorithm is used, so a fresh modifier must
 * be associated with buffers of this type. Render compression uses 128 byte
 * compression blocks.
 */
#define I915_FORMAT_MOD_F_TILED_DG2_RC_CCS fourcc_mod_code(INTEL, 9)
#define PRELIM_I915_FORMAT_MOD_F_TILED_DG2_RC_CCS fourcc_mod_code(INTEL, 13)

/*
 * Intel color control surfaces (CCS) for DG2 media compression.
 *
 * DG2 uses a new compression format for media compression. The general
 * layout is the same as I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS,
 * but a new hashing/compression algorithm is used, so a fresh modifier must
 * be associated with buffers of this type. Media compression uses 256 byte
 * compression blocks.
 */
#define I915_FORMAT_MOD_F_TILED_DG2_MC_CCS fourcc_mod_code(INTEL, 10)
#define PRELIM_I915_FORMAT_MOD_F_TILED_DG2_MC_CCS fourcc_mod_code(INTEL, 14)

/*
 * Intel color control surfaces (CCS) for DG2 clear color render compression.
 *
 * DG2 uses a unified compression format for clear color render compression.
 * The general layout is a tiled layout using 4Kb tiles i.e. Tile4 layout.
 */
#define I915_FORMAT_MOD_F_TILED_DG2_RC_CCS_CC fourcc_mod_code(INTEL, 11)
#define PRELIM_I915_FORMAT_MOD_F_TILED_DG2_RC_CCS_CC fourcc_mod_code(INTEL, 15)

#if defined(__cplusplus)
}
#endif

#endif /* __BACKPORT_DRM_FOURCC_H */
