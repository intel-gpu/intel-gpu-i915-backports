/*
 * Copyright _ 2019 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#ifndef __BACKPORT_SCATTERLIST_H
#define __BACKPORT_SCATTERLIST_H
#include_next <linux/scatterlist.h>

#if LINUX_VERSION_IS_LESS(3,7,0)
int sg_nents(struct scatterlist *sg);
#endif

#if LINUX_VERSION_IS_LESS(3, 9, 0)

/*
 * sg page iterator
 *
 * Iterates over sg entries page-by-page.  On each successful iteration,
 * @piter->page points to the current page, @piter->sg to the sg holding this
 * page and @piter->sg_pgoffset to the page's page offset within the sg. The
 * iteration will stop either when a maximum number of sg entries was reached
 * or a terminating sg (sg_last(sg) == true) was reached.
 */
struct sg_page_iter {
	struct page             *page;          /* current page */
	struct scatterlist      *sg;            /* sg holding the page */
	unsigned int            sg_pgoffset;    /* page offset within the sg */

	/* these are internal states, keep away */
	unsigned int            __nents;        /* remaining sg entries */
	int                     __pg_advance;   /* nr pages to advance at the
						 * next step */
};

struct backport_sg_mapping_iter {
	/* the following three fields can be accessed directly */
	struct page		*page;		/* currently mapped page */
	void			*addr;		/* pointer to the mapped area */
	size_t			length;		/* length of the mapped area */
	size_t			consumed;	/* number of consumed bytes */
	struct sg_page_iter	piter;		/* page iterator */

	/* these are internal states, keep away */
	unsigned int		__offset;	/* offset within page */
	unsigned int		__remaining;	/* remaining bytes on page */
	unsigned int		__flags;
};
#define sg_mapping_iter LINUX_I915_BACKPORT(sg_mapping_iter)

/**
 * sg_page_iter_page - get the current page held by the page iterator
 * @piter:	page iterator holding the page
 */
static inline struct page *sg_page_iter_page(struct sg_page_iter *piter)
{
	return nth_page(sg_page(piter->sg), piter->sg_pgoffset);
}

bool __sg_page_iter_next(struct sg_page_iter *piter);
void __sg_page_iter_start(struct sg_page_iter *piter,
			  struct scatterlist *sglist, unsigned int nents,
			  unsigned long pgoffset);

void backport_sg_miter_start(struct sg_mapping_iter *miter, struct scatterlist *sgl,
		    unsigned int nents, unsigned int flags);
bool backport_sg_miter_next(struct sg_mapping_iter *miter);
void backport_sg_miter_stop(struct sg_mapping_iter *miter);
#define sg_miter_start LINUX_I915_BACKPORT(sg_miter_start)
#define sg_miter_next LINUX_I915_BACKPORT(sg_miter_next)
#define sg_miter_stop LINUX_I915_BACKPORT(sg_miter_stop)

/**
 * for_each_sg_page - iterate over the pages of the given sg list
 * @sglist:    sglist to iterate over
 * @piter:     page iterator to hold current page, sg, sg_pgoffset
 * @nents:     maximum number of sg entries to iterate over
 * @pgoffset:  starting page offset
 */
#define for_each_sg_page(sglist, piter, nents, pgoffset)		\
	for (__sg_page_iter_start((piter), (sglist), (nents), (pgoffset)); \
	     __sg_page_iter_next(piter);)

#endif /* LINUX_VERSION_IS_LESS(3, 9, 0) */

#if LINUX_VERSION_IS_LESS(3, 11, 0)
size_t sg_copy_buffer(struct scatterlist *sgl, unsigned int nents, void *buf,
		      size_t buflen, off_t skip, bool to_buffer);

#define sg_pcopy_to_buffer LINUX_I915_BACKPORT(sg_pcopy_to_buffer)

static inline
size_t sg_pcopy_to_buffer(struct scatterlist *sgl, unsigned int nents,
			  void *buf, size_t buflen, off_t skip)
{
	return sg_copy_buffer(sgl, nents, buf, buflen, skip, true);
}

#define sg_pcopy_from_buffer LINUX_I915_BACKPORT(sg_pcopy_from_buffer)

static inline
size_t sg_pcopy_from_buffer(struct scatterlist *sgl, unsigned int nents,
			    void *buf, size_t buflen, off_t skip)
{
	return sg_copy_buffer(sgl, nents, buf, buflen, skip, false);
}

#endif /* LINUX_VERSION_IS_LESS(3, 11, 0) */

#if LINUX_VERSION_IS_LESS(4, 17, 0)

#define sg_init_marker LINUX_I915_BACKPORT(sg_init_marker)
/**
 * sg_init_marker - Initialize markers in sg table
 * @sgl:	   The SG table
 * @nents:	   Number of entries in table
 *
 **/
static inline void sg_init_marker(struct scatterlist *sgl,
				  unsigned int nents)
{
#ifdef CONFIG_DEBUG_SG
	unsigned int i;

	for (i = 0; i < nents; i++)
		sgl[i].sg_magic = SG_MAGIC;
#endif
	sg_mark_end(&sgl[nents - 1]);
}

#endif /* LINUX_VERSION_IS_LESS(4, 17, 0) */

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

#ifdef BPM_SG_CHAIN_NOT_PRESENT
static inline void __sg_chain(struct scatterlist *chain_sg,
                              struct scatterlist *sgl)
{
        /*
         * offset and length are unused for chain entry. Clear them.
         */
        chain_sg->offset = 0;
        chain_sg->length = 0;

        /*
         * Set lowest bit to indicate a link pointer, and make sure to clear
         * the termination bit if it happens to be set.
         */
        chain_sg->page_link = ((unsigned long) sgl | SG_CHAIN) & ~SG_END;
}
#endif

#endif /* __BACKPORT_SCATTERLIST_H */
