#ifndef __BACKPORT_SCATTERLIST_H
#define __BACKPORT_SCATTERLIST_H
#include_next <linux/scatterlist.h>

#ifdef BPM_FOR_EACH_SGTABLE_PAGE_NOT_PRESENT
/*
 * for_each_sgtable_page - iterate over all pages in the sg_table object
 * @sgt:        sg_table object to iterate over
 * @piter:      page iterator to hold current page
 * @pgoffset:   starting page offset (in pages)
 *
 * Iterates over the all memory pages in the buffer described by
 * a scatterlist stored in the given sg_table object.
 * See also for_each_sg_page(). In each loop it operates on PAGE_SIZE unit.
 */
#define for_each_sgtable_page(sgt, piter, pgoffset)    \
       for_each_sg_page((sgt)->sgl, piter, (sgt)->orig_nents, pgoffset)


/*
 * for_each_sgtable_dma_page - iterate over the DMA mapped sg_table object
 * @sgt:       sg_table object to iterate over
 * @dma_iter:   DMA page iterator to hold current page
 * @pgoffset:   starting page offset (in pages)
 *
 * Iterates over the all DMA mapped pages in the buffer described by
 * a scatterlist stored in the given sg_table object.
 * See also for_each_sg_dma_page(). In each loop it operates on PAGE_SIZE
 * unit.
 */
#define for_each_sgtable_dma_page(sgt, dma_iter, pgoffset)     \
       for_each_sg_dma_page((sgt)->sgl, dma_iter, (sgt)->nents, pgoffset)

/*
 * Loop over each sg element in the given *DMA mapped* sg_table object.
 * Please use sg_dma_address(sg) and sg_dma_len(sg) to extract DMA addresses
 * of the each element.
 */

#define for_each_sgtable_dma_sg(sgt, sg, i)     \
                for_each_sg(sgt->sgl, sg, sgt->nents, i)

#endif

#endif /* __BACKPORT_SCATTERLIST_H */

