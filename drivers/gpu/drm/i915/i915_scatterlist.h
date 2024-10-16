/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2016 Intel Corporation
 */

#ifndef I915_SCATTERLIST_H
#define I915_SCATTERLIST_H

#include <linux/pfn.h>
#include <linux/scatterlist.h>
#include <linux/swiotlb.h>

#include "i915_gem.h"

#define I915_MAX_CHAIN_ALLOC (SG_MAX_SINGLE_ALLOC - 1)

enum {
	SG_CAPACITY = 0,
	SG_COUNT,
	SG_PAGE_SIZES,
	__SG_NUM_INLINE,
	SG_NUM_INLINE = roundup_pow_of_two(__SG_NUM_INLINE)
};

struct sg_table_inline {
	struct {
		/* scatterlist */
		unsigned long	page_link;
		unsigned int	offset;
		unsigned int	length;
		dma_addr_t	dma_address;
#ifdef CONFIG_NEED_SG_DMA_LENGTH
		unsigned int	dma_length;
#endif
		/* sg_table */
		unsigned int pack;
	} tbl[SG_NUM_INLINE];
};

static inline struct sg_table_inline *as_sg_table_inline(struct scatterlist *sg)
{
	struct sg_table_inline *sgt = (struct sg_table_inline *)sg;

#ifdef CPTCFG_DRM_I915_DEBUG
	BUG_ON(!sgt->tbl[SG_CAPACITY].pack);
#endif

	return sgt;
}

static inline struct scatterlist *to_scatterlist(struct sg_table_inline *sgt)
{
	return (struct scatterlist *)sgt;
}

#define sg_capacity(sg)		(as_sg_table_inline(sg)->tbl[SG_CAPACITY].pack)
#define sg_count(sg)		(as_sg_table_inline(sg)->tbl[SG_COUNT].pack)
#define sg_page_sizes(sg)	(as_sg_table_inline(sg)->tbl[SG_PAGE_SIZES].pack)
#define sg_table(sg)		((struct sg_table){ .orig_nents = sg_capacity(sg), .nents = sg_count(sg), .sgl = sg })

#define __as_sg_table_inline(sg)	((struct sg_table_inline *)(sg))
#define __sg_set_capacity(sg, x)	(__as_sg_table_inline(sg)->tbl[SG_CAPACITY].pack = (x))
#define sg_init_capacity(sg)		__sg_set_capacity(sg, SG_NUM_INLINE)
#define sg_init_count(sg)		(__as_sg_table_inline(sg)->tbl[SG_COUNT].pack = 0)
#define sg_init_page_sizes(sg)		(__as_sg_table_inline(sg)->tbl[SG_PAGE_SIZES].pack = 0)
#define sg_init_inline(sg) ({ \
	struct sg_table_inline *sgt__ = __as_sg_table_inline(sg);	\
	sgt__->tbl[SG_CAPACITY].pack = SG_NUM_INLINE;			\
	sgt__->tbl[SG_COUNT].pack = 0;					\
	sgt__->tbl[SG_PAGE_SIZES].pack = 0;				\
})

/*
 * Optimised SGL iterator for GEM objects
 */
static __always_inline struct sgt_iter {
	struct scatterlist *sgp;
	union {
		unsigned long pfn;
		dma_addr_t dma;
	};
	unsigned int curr;
	unsigned int max;
} __sgt_iter(struct scatterlist *sgl, bool dma) {
	struct sgt_iter s = { .sgp = sgl };

	if (s.sgp) {
		if (dma) {
			s.dma = sg_dma_address(s.sgp);
			s.max = sg_dma_len(s.sgp);
		} else {
			s.pfn = page_to_pfn(sg_page(s.sgp));
			s.max = s.sgp->length;
		}
		if (!s.max)
			s.sgp = NULL;
	}

	return s;
}

static inline int __sg_page_count(const struct scatterlist *sg)
{
	return sg->length >> PAGE_SHIFT;
}

static inline int __sg_dma_page_count(const struct scatterlist *sg)
{
	return sg_dma_len(sg) >> PAGE_SHIFT;
}

static inline struct scatterlist *____sg_next(struct scatterlist *sg)
{
	++sg;
	if (unlikely(sg_is_chain(sg)))
		sg = sg_chain_ptr(sg);
	return sg;
}

/**
 * __sg_next - return the next scatterlist entry in a list
 * @sg:		The current sg entry
 *
 * Description:
 *   If the entry is the last, return NULL; otherwise, step to the next
 *   element in the array (@sg@+1). If that's a chain pointer, follow it;
 *   otherwise just return the pointer to the current element.
 **/
static inline struct scatterlist *__sg_next(struct scatterlist *sg)
{
	return sg_is_last(sg) ? NULL : ____sg_next(sg);
}

/**
 * __for_each_sgt_daddr - iterate over the device addresses of the given sg_table
 * @__dp:	Device address (output)
 * @__iter:	'struct sgt_iter' (iterator state, internal)
 * @__sg:	sg_table to iterate over (input)
 * @__step:	step size
 */
#define __for_each_sgt_daddr(__dp, __iter, __sg, __step)		\
	for ((__iter) = __sgt_iter((__sg), true);			\
	     ((__dp) = (__iter).dma + (__iter).curr), (__iter).sgp;	\
	     (((__iter).curr += (__step)) >= (__iter).max) ?		\
	     (__iter) = __sgt_iter(__sg_next((__iter).sgp), true), 0 : 0)

/**
 * for_each_sgt_page - iterate over the pages of the given sg_table
 * @__pp:	page pointer (output)
 * @__iter:	'struct sgt_iter' (iterator state, internal)
 * @__sg:	sg_table to iterate over (input)
 */
#define for_each_sgt_page(__pp, __iter, __sg)				\
	for ((__iter) = __sgt_iter((__sg), false);		\
	     ((__pp) = (__iter).pfn == 0 ? NULL :			\
	      pfn_to_page((__iter).pfn + ((__iter).curr >> PAGE_SHIFT))); \
	     (((__iter).curr += PAGE_SIZE) >= (__iter).max) ?		\
	     (__iter) = __sgt_iter(__sg_next((__iter).sgp), false), 0 : 0)

/**
 * __for_each_daddr - iterates over the device addresses with pre-initialized
 * iterator.
 */
#define __for_each_daddr(__dp, __iter, __step)			\
	for (; ((__dp) = (__iter).dma + (__iter).curr), (__iter).sgp;	\
	     (((__iter).curr += (__step)) >= (__iter).max) ?		\
	     (__iter) = __sgt_iter(__sg_next((__iter).sgp), true), 0 : 0)

static inline unsigned int i915_sg_segment_size(void)
{
	unsigned int size = swiotlb_max_segment();

	if (size == 0)
		size = UINT_MAX;

	size = rounddown(size, PAGE_SIZE);
	/* swiotlb_max_segment_size can return 1 byte when it means one page. */
	if (size < PAGE_SIZE)
		size = PAGE_SIZE;

	return size;
}

struct scatterlist *sg_pool_alloc(unsigned int nents, gfp_t gfp_mask);
void i915_sg_free_excess(struct scatterlist *sg);
void i915_sg_trim(struct scatterlist *sg);
int i915_sg_map(struct scatterlist *sg, unsigned long max, struct device *dev);

struct scatterlist *__sg_table_inline_create(gfp_t gfp);
struct scatterlist *sg_table_inline_create(gfp_t gfp);
int sg_table_inline_alloc(struct scatterlist *st, unsigned int nents, gfp_t gfp);
void sg_table_inline_free(struct scatterlist *st);

int i915_scatterlist_module_init(void);
void i915_scatterlist_module_exit(void);

static inline u64 __sg_total_length(struct scatterlist *sg, bool dma)
{
	u64 total = 0;

	for (; sg; sg = __sg_next(sg))
		total += dma ? sg_dma_len(sg) : sg->length;

	return total;
}

#endif
