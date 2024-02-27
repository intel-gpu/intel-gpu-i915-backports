/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2016 Intel Corporation
 */

#include <linux/kmemleak.h>
#include <linux/slab.h>

#include <drm/drm_mm.h>

#include "i915_buddy.h"
#include "i915_scatterlist.h"

void i915_sg_trim(struct sg_table *sgt)
{
	struct scatterlist *sg;
	unsigned int n, end;

	GEM_BUG_ON(sgt->nents > sgt->orig_nents);
	if (sgt->nents == sgt->orig_nents)
		return;

	n = 0;
	end = 0;
	sg = sgt->sgl;
	do {
		struct scatterlist *chain;

		if (sgt->orig_nents - n <= SG_MAX_SINGLE_ALLOC)
			break;

		if (end == 0 && n + SG_MAX_SINGLE_ALLOC >= sgt->nents)
			end = n + SG_MAX_SINGLE_ALLOC;

		chain = sg_chain_ptr(sg + I915_MAX_CHAIN_ALLOC);
		if (n >= sgt->nents) {
			kmemleak_free(sg);
			free_page((unsigned long)sg);
		}

		n += I915_MAX_CHAIN_ALLOC;
		if (sgt->nents == n + 1) {
			sg[I915_MAX_CHAIN_ALLOC] = *chain;
			GEM_BUG_ON(!sg_is_last(sg + I915_MAX_CHAIN_ALLOC));
			GEM_BUG_ON(end != sgt->nents);
		}

		sg = chain;
	} while (1);
	if (!end)
		return;

	if (n >= sgt->nents) {
		if (sgt->orig_nents - n == SG_MAX_SINGLE_ALLOC) {
			kmemleak_free(sg);
			free_page((unsigned long)sg);
		} else {
			kfree(sg);
		}
	}

	sgt->orig_nents = end;
}

unsigned long i915_sg_compact(struct sg_table *st, unsigned long max)
{
	struct scatterlist *sg, *cur = NULL;
	unsigned long sizes = 0;
	unsigned long pfn = -1;

	GEM_BUG_ON(!IS_ALIGNED(max, PAGE_SIZE));
	if (GEM_WARN_ON(!st->orig_nents))
		return 0;

	st->nents = 0;
	for (sg = st->sgl; sg; sg = __sg_next(sg)) {
		if (!sg->length)
			continue;

		if (page_to_pfn(sg_page(sg)) == pfn && cur->length < max) {
			cur->length += PAGE_SIZE;
		} else {
			if (cur) {
				sizes |= cur->length;
				cur = __sg_next(cur);
			} else {
				cur = st->sgl;
			}
			sg_set_page(cur, sg_page(sg), sg->length, 0);
			sg_dma_address(cur) = sg_dma_address(sg);
			sg_dma_len(cur) = sg_dma_len(sg);
			st->nents++;

			pfn = page_to_pfn(sg_page(sg));
		}
		pfn++;
	}
	if (unlikely(!cur))
		cur = memset(st->sgl, 0, sizeof(*cur));
	sizes |= cur->length;
	sg_mark_end(cur);

	i915_sg_trim(st);
	return sizes;
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftests/scatterlist.c"
#endif
