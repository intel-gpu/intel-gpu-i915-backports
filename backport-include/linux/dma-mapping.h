#ifndef __BACKPORT_DMA_MAPPING_H
#define __BACKPORT_DMA_MAPPING_H
#include <linux/version.h>
#include_next <linux/dma-mapping.h>

#ifdef BPM_DMA_MAP_UNMAP_SGTABLE_NOT_PRESENT

/**
 * dma_map_sgtable - Map the given buffer for DMA
 * @dev:        The device for which to perform the DMA operation
 * @sgt:        The sg_table object describing the buffer
 * @dir:        DMA direction
 * @attrs:      Optional DMA attributes for the map operation
 *
 * Maps a buffer described by a scatterlist stored in the given sg_table
 * object for the @dir DMA operation by the @dev device. After success the
 * ownership for the buffer is transferred to the DMA domain.  One has to
 * call dma_sync_sgtable_for_cpu() or dma_unmap_sgtable() to move the
 * ownership of the buffer back to the CPU domain before touching the
 * buffer by the CPU.
 *
 * Returns 0 on success or -EINVAL on error during mapping the buffer.
 */
static inline int dma_map_sgtable(struct device *dev, struct sg_table *sgt,
                enum dma_data_direction dir, unsigned long attrs)
{
        int nents;

        nents = dma_map_sg_attrs(dev, sgt->sgl, sgt->orig_nents, dir, attrs);
        if (nents <= 0)
                return -EINVAL;
        sgt->nents = nents;
        return 0;
}

/**
 * dma_unmap_sgtable - Unmap the given buffer for DMA
 * @dev:        The device for which to perform the DMA operation
 * @sgt:        The sg_table object describing the buffer
 * @dir:        DMA direction
 * @attrs:      Optional DMA attributes for the unmap operation
 *
 * Unmaps a buffer described by a scatterlist stored in the given sg_table
 * object for the @dir DMA operation by the @dev device. After this function
 * the ownership of the buffer is transferred back to the CPU domain.
 */
static inline void dma_unmap_sgtable(struct device *dev, struct sg_table *sgt,
                enum dma_data_direction dir, unsigned long attrs)
{
        dma_unmap_sg_attrs(dev, sgt->sgl, sgt->orig_nents, dir, attrs);
}
#endif

#endif /* __BACKPORT_DMA_MAPPING_H */
