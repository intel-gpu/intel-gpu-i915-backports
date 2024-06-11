// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dynamic DMA mapping support.
 *
 * This implementation is a fallback for platforms that do not support
 * I/O TLBs (aka DMA address translation hardware).
 * Copyright (C) 2000 Asit Mallick <Asit.K.Mallick@intel.com>
 * Copyright (C) 2000 Goutham Rao <goutham.rao@intel.com>
 * Copyright (C) 2000, 2003 Hewlett-Packard Co
 *      David Mosberger-Tang <davidm@hpl.hp.com>
 *
 * 03/05/07 davidm      Switch from PCI-DMA to generic device DMA API.
 * 00/12/13 davidm      Rename to swiotlb.c and add mark_clean() to avoid
 *                      unnecessary i-cache flushing.
 * 04/07/.. ak          Better overflow handling. Assorted fixes.
 * 05/09/10 linville    Add support for syncing ranges, support syncing for
 *                      DMA_BIDIRECTIONAL mappings, miscellaneous cleanup.
 * 08/12/11 beckyb      Add highmem support
 */

#include <linux/swiotlb.h>

#ifdef BPM_SWIOTLB_MAX_SEGMENT_NOT_PRESENT
struct io_tlb_mem io_tlb_default_mem;
static unsigned int max_segment;

unsigned int swiotlb_max_segment(void)
{
        return io_tlb_default_mem.nslabs ? max_segment : 0;
}
EXPORT_SYMBOL_GPL(swiotlb_max_segment);
#endif

#ifdef BPM_IS_SWIOTLB_ACTIVE_PRESENT
bool is_swiotlb_active(struct device *dev)
{
        struct io_tlb_mem *mem = dev->dma_io_tlb_mem;

        return mem && mem->nslabs;
}
EXPORT_SYMBOL_GPL(is_swiotlb_active);
#endif
