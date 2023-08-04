/* SPDX-License-Identifier: GPL-2.0 */
/* Common header for intel-gtt.ko and i915.ko */

#ifndef __BACKPORT__DRM_INTEL_GTT_H
#define __BACKPORT__DRM_INTEL_GTT_H

#include <linux/scatterlist.h>
#include_next <drm/intel-gtt.h>

#ifdef BPM_INTEL_GMCH_GTT_RENAMED
#define intel_gmch_gtt_get intel_gtt_get
#define intel_gmch_enable_gtt intel_enable_gtt
#define intel_gmch_gtt_flush intel_gtt_chipset_flush
#define intel_gmch_gtt_insert_page intel_gtt_insert_page
#define intel_gmch_gtt_insert_sg_entries intel_gtt_insert_sg_entries
#define intel_gmch_gtt_clear_range intel_gtt_clear_range
#else
struct agp_bridge_data;
struct pci_dev;
struct sg_table;

void intel_gmch_gtt_get(u64 *gtt_total,
                        phys_addr_t *mappable_base,
                        resource_size_t *mappable_end);

int intel_gmch_probe(struct pci_dev *bridge_pdev, struct pci_dev *gpu_pdev,
                     struct agp_bridge_data *bridge);
void intel_gmch_remove(void);

bool intel_gmch_enable_gtt(void);

void intel_gmch_gtt_flush(void);
void intel_gmch_gtt_insert_page(dma_addr_t addr,
                                unsigned int pg,
                                unsigned int flags);
void intel_gmch_gtt_insert_sg_entries(struct sg_table *st,
                                      unsigned int pg_start,
                                      unsigned int flags);
void intel_gmch_gtt_clear_range(unsigned int first_entry, unsigned int num_entries);

/* Special gtt memory types */
#define AGP_DCACHE_MEMORY       1
#define AGP_PHYS_MEMORY         2

/* flag for GFDT type */
#define AGP_USER_CACHED_MEMORY_GFDT (1 << 3)

#endif
#endif


