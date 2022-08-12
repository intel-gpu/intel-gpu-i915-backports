/* SPDX-License-Identifier: GPL-2.0 */
/* Common header for intel-gtt.ko and i915.ko */

#ifndef __BACKPORT__DRM_INTEL_GTT_H
#define	__BACKPORT__DRM_INTEL_GTT_H

#include_next <drm/intel-gtt.h>

#ifdef INTEL_GMCH_GTT_RENAMED

#define intel_gmch_gtt_get intel_gtt_get
#define intel_gmch_enable_gtt intel_enable_gtt
#define intel_gmch_gtt_flush intel_gtt_chipset_flush
#define intel_gmch_gtt_insert_page intel_gtt_insert_page
#define intel_gmch_gtt_insert_sg_entries intel_gtt_insert_sg_entries
#define intel_gmch_gtt_clear_range intel_gtt_clear_range
#endif

#endif /* __BACKPORT__DRM_INTEL_GTT_H */
