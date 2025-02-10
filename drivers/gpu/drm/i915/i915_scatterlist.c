/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2016 Intel Corporation
 */

#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/iova.h>
#include <linux/kmemleak.h>
#include <linux/mempool.h>
#include <linux/slab.h>

#include <drm/drm_mm.h>

#include "i915_buddy.h"
#include "i915_scatterlist.h"

#define SG_MEMPOOL_NR		ARRAY_SIZE(sg_pools)
#define SG_MEMPOOL_SIZE		64
#define SG_MEMPOOL_MIN		4

struct sg_pool {
	union {
		const char		*name;
		struct kmem_cache	*kc;
	};
};

#define SP(x) {{ .name = "i915-sg:" __stringify(x) }}
#if (SG_CHUNK_SIZE < 32)
#error SG_CHUNK_SIZE is too small (must be 32 or greater)
#endif
static struct sg_pool sg_pools[] = {
	SP(SG_MEMPOOL_MIN),
	SP(8),
	SP(16),
#if (SG_CHUNK_SIZE > 32)
	SP(32),
#if (SG_CHUNK_SIZE > 64)
	SP(64),
#if (SG_CHUNK_SIZE > 128)
	SP(128),
#if (SG_CHUNK_SIZE > 256)
#error SG_CHUNK_SIZE is too large (256 MAX)
#endif
#endif
#endif
#endif
	SP(SG_CHUNK_SIZE)
};
#undef SP

static inline struct sg_pool *sg_pool_index(unsigned short nents)
{
	unsigned int index = 0;

	GEM_BUG_ON(nents > SG_CHUNK_SIZE);
	if (nents > SG_MEMPOOL_MIN)
		index = get_count_order(nents) - ilog2(SG_MEMPOOL_MIN);

	return sg_pools + index;
}

static void sg_pool_free(struct scatterlist *sgl, unsigned int nents)
{
	struct sg_pool *sgp = sg_pool_index(nents);

	kmem_cache_free(sgp->kc, sgl);
}

struct scatterlist *sg_pool_alloc(unsigned int nents, gfp_t gfp_mask)
{
	struct sg_pool *sgp = sg_pool_index(nents);

	return kmem_cache_alloc(sgp->kc, gfp_mask);
}

static void init_sg_table_inline(struct scatterlist *sg)
{
	sg_init_table(sg, SG_NUM_INLINE);
	sg_init_capacity(sg);
}

struct scatterlist *__sg_table_inline_create(gfp_t gfp)
{
	BUILD_BUG_ON(sizeof(__as_sg_table_inline(0)->tbl[0]) != sizeof(struct scatterlist));

	return sg_pool_alloc(SG_NUM_INLINE, gfp);
}

struct scatterlist *sg_table_inline_create(gfp_t gfp)
{
	struct scatterlist *sg;

	sg = __sg_table_inline_create(gfp);
	if (unlikely(!sg))
		return NULL;

	init_sg_table_inline(sg);
	return sg;
}

int sg_table_inline_alloc(struct scatterlist *sgt, unsigned int nents, gfp_t gfp)
{
	struct scatterlist *sg;
	unsigned int n;
	int ret = 0;

	if (sg_capacity(sgt) >= nents)
		return 0;

	for (n = sg_capacity(sgt) - 1, sg = sgt; n < nents;) {
		struct scatterlist *chain;
		unsigned int x;

		x = min_t(unsigned int, nents - n, SG_MAX_SINGLE_ALLOC);
		chain = sg_pool_alloc(x, gfp);
		if (unlikely(!chain)) {
			ret = -ENOMEM;
			n++;
			break;
		}

		sg_init_table(chain, x);
		__sg_chain(sg + min_t(unsigned int, n, I915_MAX_CHAIN_ALLOC), chain);
		sg = chain;

		n += x;
		n -= n < nents;
	}

	sg_capacity(sgt) = n;
	return ret;
}

static void free_excess(struct scatterlist *sg)
{
	if (sg_capacity(sg) > SG_NUM_INLINE)
		__sg_free_table(&sg_table(sg),
				SG_CHUNK_SIZE, SG_NUM_INLINE,
				sg_pool_free, sg_capacity(sg));
}

void sg_table_inline_free(struct scatterlist *sg)
{
	free_excess(sg);
	sg_pool_free(sg, SG_NUM_INLINE);
}

void i915_sg_free_excess(struct scatterlist *sg)
{
	free_excess(sg);
	init_sg_table_inline(sg);
}

__maybe_unused
static unsigned int i915_sg_count(struct scatterlist *sg)
{
	unsigned int count = 0;

	while (sg)
		count++, sg = __sg_next(sg);

	return count;
}

void i915_sg_trim(struct scatterlist *sgt)
{
	const unsigned int capacity = sg_capacity(sgt);
	const unsigned int count = sg_count(sgt);
	unsigned int max_ents = SG_NUM_INLINE;
	struct scatterlist *sg = sgt;
	unsigned int n, end;

	GEM_BUG_ON(count > capacity);
	if (count == capacity)
		return;

	n = 0;
	end = 0;
	do {
		struct scatterlist *chain;

		if (n + max_ents >= capacity)
			return;

		if (n + max_ents >= count)
			end = n + max_ents;

		n += max_ents - 1;
		chain = sg_chain_ptr(sg + max_ents - 1);
		if (count == n + 1) {
			sg[max_ents - 1] = *chain;
			GEM_BUG_ON(!sg_is_last(sg + max_ents - 1));
			GEM_BUG_ON(end != count);
		}

		max_ents = SG_MAX_SINGLE_ALLOC;
		sg = chain;
	} while (!end);
	GEM_BUG_ON(end > capacity);

	while (n + SG_MAX_SINGLE_ALLOC < capacity) {
		struct scatterlist *chain = sg_chain_ptr(sg + I915_MAX_CHAIN_ALLOC);

		sg_pool_free(sg, SG_MAX_SINGLE_ALLOC);

		n += I915_MAX_CHAIN_ALLOC;
		sg = chain;
	}
	if (n < capacity)
		sg_pool_free(sg, capacity - n);

	sg_capacity(sgt) = end;
	GEM_BUG_ON(sg_count(sgt) > sg_capacity(sgt));
	GEM_BUG_ON(sg_count(sgt) != i915_sg_count(sgt));
}

static size_t __i915_iommu_pgsize(const struct iommu_domain *domain,
				  unsigned long iova, phys_addr_t paddr, size_t size,
				  size_t *count)
{
	unsigned int pgsize_idx, pgsize_idx_next;
	unsigned long addr_merge = paddr | iova;
	size_t offset, pgsize, pgsize_next;
	unsigned long pgsizes;

	/* Page sizes supported by the hardware and small enough for @size */
	pgsizes = domain->pgsize_bitmap & GENMASK(__fls(size), 0);

	/* Constrain the page sizes further based on the maximum alignment */
	if (likely(addr_merge))
		pgsizes &= GENMASK(__ffs(addr_merge), 0);

	/* Make sure we have at least one suitable page size */
	GEM_BUG_ON(!pgsizes);

	/* Pick the biggest page size remaining */
	pgsize_idx = __fls(pgsizes);
	pgsize = BIT(pgsize_idx);
	if (!count)
		return pgsize;

	/* Find the next biggest support page size, if it exists */
	pgsizes = domain->pgsize_bitmap & ~GENMASK(pgsize_idx, 0);
	if (!pgsizes)
		goto out_set_count;

	pgsize_idx_next = __ffs(pgsizes);
	pgsize_next = BIT(pgsize_idx_next);

	/*
	 * There's no point trying a bigger page size unless the virtual
	 * and physical addresses are similarly offset within the larger page.
	 */
	if ((iova ^ paddr) & (pgsize_next - 1))
		goto out_set_count;

	/* Calculate the offset to the next page size alignment boundary */
	offset = pgsize_next - (addr_merge & (pgsize_next - 1));

	/*
	 * If size is big enough to accommodate the larger page, reduce
	 * the number of smaller pages.
	 */
	if (offset + pgsize_next <= size)
		size = offset;

out_set_count:
	*count = size >> pgsize_idx;
	return pgsize;
}

int __i915_iommu_map(struct iommu_domain *domain,
		     unsigned long iova, phys_addr_t paddr, size_t size,
		     int prot, gfp_t gfp, size_t *mapped)
{
	int ret;

	GEM_BUG_ON(!(domain->type & __IOMMU_DOMAIN_PAGING));
	GEM_BUG_ON(!IS_ALIGNED(iova | paddr | size, 1 << __ffs(domain->pgsize_bitmap)));

	while (size) {
		size_t pgsz, count, sz;

		pgsz = __i915_iommu_pgsize(domain, iova, paddr, size, &count);
#ifdef BPM_IOMMU_OPS_MAP_PAGES_NOT_PRESENT
		sz = count << __ffs(pgsz);
		ret = domain->ops->map(domain, iova, paddr, sz, prot, gfp);
#else
		ret = domain->ops->map_pages(domain, iova, paddr, pgsz, count, prot, gfp, &sz);
#endif
		if (ret)
			return ret;

		iova += sz;
		paddr += sz;
		*mapped += sz;

		size -= sz;
	}

	return 0;
}

static inline struct iova_domain *i915_iovad(struct iommu_domain *domain)
{
	struct {
		enum {
			IOVA_COOKIE,
		} type;
		struct iova_domain iovad;
	} *cookie = (void *)domain->iova_cookie;

	return &cookie->iovad;
}

void __i915_iommu_free(unsigned long iova, unsigned long total, unsigned long mapped, struct iommu_domain *domain)
{
	struct iova_domain *iovad = i915_iovad(domain);
	int shift = iova_shift(iovad);

	iommu_unmap(domain, iova, mapped);
	free_iova_fast(iovad, iova >> shift, total >> shift);
}

unsigned long __i915_iommu_alloc(unsigned long total, u64 dma_limit, struct iommu_domain *domain)
{
	struct iova_domain *iovad = i915_iovad(domain);
	int shift = iova_shift(iovad);
	unsigned long iova;

	if (domain->geometry.force_aperture)
		dma_limit = min_t(u64, dma_limit, domain->geometry.aperture_end);

	iova = alloc_iova_fast(iovad, total >> shift, dma_limit >> shift, true);
	if (!iova)
		return -ENOMEM;

	return iova << shift;
}

int i915_sg_map(struct scatterlist *sgt, unsigned long total, unsigned long max, struct device *dev)
{
	struct iommu_domain *domain = get_iommu_domain(dev);
	struct scatterlist *sg, *cur = NULL, *map;
	unsigned long iova, mapped;
	unsigned long end = -1;
	int err = 0;

	GEM_BUG_ON(!IS_ALIGNED(max, PAGE_SIZE));

	if (domain) {
		iova = __i915_iommu_alloc(total, i915_dma_limit(dev), domain);
		if (IS_ERR_VALUE(iova))
			return iova;

		map = sgt;
		sg_dma_address(map) = iova;
		sg_dma_len(map) = 0;
		mapped = 0;
	}

	sg_count(sgt) = 0;
	sg_page_sizes(sgt) = 0;
	for (sg = sgt; sg; sg = __sg_next(sg)) {
		unsigned int len = sg->length;
		unsigned long phys;

		if (unlikely(!len))
			continue;

		GEM_BUG_ON(sg->offset);
		phys = page_to_phys(sg_page(sg));
		if (phys == end && cur->length < max) {
			cur->length += len;
		} else {
			if (cur) {
				if (!domain) {
					sg_dma_address(cur) = __sg_phys(cur);
					sg_dma_len(cur) = cur->length;
				} else if (!err)  {
					if (sg_dma_len(map) > UINT_MAX - cur->length) {
						map = __sg_next(map);
						sg_dma_address(map) = iova + mapped;
						sg_dma_len(map) = 0;
					}

					err = __i915_iommu_map(domain, iova + mapped,
							       __sg_phys(cur), cur->length,
							       IOMMU_READ | IOMMU_WRITE, GFP_KERNEL,
							       &mapped);
					GEM_BUG_ON(mapped > total);
					sg_dma_len(map) += cur->length;
				}

				sg_page_sizes(sgt) |= cur->length;
				cur = __sg_next(cur);
			} else {
				cur = sgt;
			}
			sg_set_page(cur, sg_page(sg), len, 0);
			sg_count(sgt)++;

			end = phys;
		}
		end += len;
	}
	GEM_BUG_ON(!cur);

	if (!domain) {
		sg_dma_address(cur) = __sg_phys(cur);
		sg_dma_len(cur) = cur->length;
	} else if (!err) {
		if (sg_dma_len(map) > UINT_MAX - cur->length) {
			map = __sg_next(map);
			sg_dma_address(map) = iova + mapped;
			sg_dma_len(map) = 0;
		}

		err = __i915_iommu_map(domain, iova + mapped,
				       __sg_phys(cur), cur->length,
				       IOMMU_READ | IOMMU_WRITE, GFP_KERNEL,
				       &mapped);
		GEM_BUG_ON(mapped > total);
		sg_dma_len(map) += cur->length;
		if (map != cur)
			sg_dma_len(__sg_next(map)) = 0; /* iommu terminator */
	}

	sg_page_sizes(sgt) |= cur->length;
	sg_mark_end(cur);

	if (domain) {
		if (!err) {
			if (domain->ops->iotlb_sync_map)
#ifdef BPM_IOTLB_SYNC_MAP_ARGS_IOVA_SIZE_NOT_PRESENT
				domain->ops->iotlb_sync_map(domain);
#else
				domain->ops->iotlb_sync_map(domain, iova, mapped);
#endif
		} else {
			__i915_iommu_free(iova, total, mapped, domain);
			sg_dma_len(sgt) = 0;
		}
	}

	i915_sg_trim(sgt);
	return err;
}

int __init i915_scatterlist_module_init(void)
{
	int size, i;

	for (i = 0, size = SG_MEMPOOL_MIN * sizeof(struct scatterlist); i < SG_MEMPOOL_NR; i++, size <<= 1) {
		struct sg_pool *sgp = sg_pools + i;
		const char *name = sgp->name;

		sgp->kc = kmem_cache_create(name, size, 0, 0, NULL);
		if (!sgp->kc) {
			pr_err("SG_POOL: can't init sg slab %s\n", name);
			goto cleanup_sdb;
		}
	}

	return 0;

cleanup_sdb:
	while (i--) {
		struct sg_pool *sgp = sg_pools + i;

		kmem_cache_destroy(sgp->kc);
	}
	memset(sg_pools, 0, sizeof(*sg_pools));

	return -ENOMEM;
}

void i915_scatterlist_module_exit(void)
{
	int i;

	for (i = 0; i < SG_MEMPOOL_NR; i++) {
		struct sg_pool *sgp = sg_pools + i;

		if (!sgp->kc)
			break;

		kmem_cache_destroy(sgp->kc);
	}
}
