// SPDX-License-Identifier: MIT
/*
 * Copyright © 2012-2023 Intel Corporation
 */

#include <linux/hugetlb.h>
#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/mmu_context.h>
#include <linux/swap.h>
#include <linux/sched/mm.h>
#ifdef BPM_MMAP_WRITE_LOCK_NOT_PRESENT
#include <linux/mmap_lock.h>
#endif

#include "gt/intel_gt.h"

#include "i915_drv.h"
#include "i915_gem_ioctls.h"
#include "i915_gem_object.h"
#include "i915_gem_region.h"
#include "i915_scatterlist.h"
#include "i915_sw_fence_work.h"
#include "i915_tbb.h"

#ifdef BPM_KTHREAD_USE_MM_NOT_PRESENT
#define kthread_use_mm use_mm
#define kthread_unuse_mm unuse_mm
#endif

static void
i915_gem_userptr_init__mm(struct drm_i915_gem_object *obj)
{
	obj->userptr.mm = current->mm;
	mmgrab(current->mm);
}

static void
i915_gem_userptr_release(struct drm_i915_gem_object *obj)
{
	i915_gem_object_release_memory_region(obj);
	mmdrop(obj->userptr.mm);
}

struct userptr_work {
	struct dma_fence_work base;
	struct drm_i915_gem_object *obj;
	struct mempolicy *policy;
	struct scatterlist *pages;
};

struct userptr_chunk {
	struct i915_tbb tbb;
	struct mm_struct *mm;
	struct mempolicy *policy;
	struct i915_sw_fence *fence;
	unsigned long addr;
	unsigned int count;
};

#if IS_ENABLED(CONFIG_NUMA)
#define set_mempolicy(tsk, pol) smp_store_mb((tsk)->mempolicy, pol)
#define get_mempolicy(tsk) ((tsk)->mempolicy)
#else
#define set_mempolicy(tsk, pol)
#define get_mempolicy(tsk) NULL
#endif

struct follow_page_context {
	unsigned int page_size;
};

static struct page *__try_get_compound_page(struct page *page)
{
	struct page *head = compound_head(page);

	if (unlikely(!page_cache_get_speculative(head)))
		return NULL;

	if (unlikely(compound_head(page) != head)) {
		put_page(head);
		return NULL;
	}

	return page;
}

#if IS_ENABLED(CONFIG_ARCH_HAS_PTE_SPECIAL)

static struct page *follow_page_pte(unsigned long address,
				    pmd_t *pmd,
				    unsigned int flags,
				    struct follow_page_context *ctx)
{
	struct page *page = NULL;
	pte_t *ptep, pte;

	if (pmd_bad(*pmd))
		return NULL;

	ptep = pte_offset_map(pmd, address);
	if (unlikely(!ptep))
		return NULL;

	pte = ptep_get_lockless(ptep);
	if (!pte_present(pte))
		goto out;

	if (unlikely(pte_special(pte)))
		goto out;
	if (unlikely(pte_devmap(pte)))
		goto out;

	if (flags & FOLL_WRITE && !pte_write(pte))
		goto out;

	page = __try_get_compound_page(pte_page(pte));
	if (unlikely(!page)) {
		page = ERR_PTR(-EAGAIN);
		goto out;
	}

	if (unlikely(pte_val(pte) != pte_val(*ptep))) {
		put_page(page);
		page = ERR_PTR(-EAGAIN);
		goto out;
	}

	ctx->page_size = SZ_4K;
out:
	pte_unmap(ptep);
	return page;
}

static struct page *follow_page_pmd(pmd_t orig, pmd_t *pmd, unsigned long flags,
				    struct follow_page_context *ctx)
{
	struct page *page;

	if (flags & FOLL_WRITE && !pmd_write(orig))
		return NULL;

	page = __try_get_compound_page(pmd_page(orig));
	if (unlikely(!page))
		return ERR_PTR(-EAGAIN);

	if (unlikely(pmd_val(orig) != pmd_val(*pmd))) {
		put_page(page);
		return ERR_PTR(-EAGAIN);
	}

	ctx->page_size = SZ_2M;
	return page;
}

static struct page *follow_pmd_mask(unsigned long address,
				    pud_t *pudp,
				    unsigned int flags,
				    struct follow_page_context *ctx)
{
	pmd_t *pmd = pmd_offset(pudp, address);
	pmd_t val = READ_ONCE(*pmd);

	if (pmd_none(val) || unlikely(!pmd_present(val)))
		return NULL;
	if (unlikely(is_hugepd(__hugepd(pmd_val(val)))))
		return NULL;
	if (unlikely(pmd_devmap(val)))
		return NULL;

	if (!pmd_trans_huge(val))
		return follow_page_pte(address, pmd, flags, ctx);
	else
		return follow_page_pmd(val, pmd, flags, ctx);
}

static struct page *follow_pud_mask(unsigned long address,
				    p4d_t *p4dp,
				    unsigned int flags,
				    struct follow_page_context *ctx)
{
	pud_t *pud = pud_offset(p4dp, address);

	if (pud_none(*pud) || unlikely(pud_bad(*pud)))
		return NULL;
	if (is_hugepd(__hugepd(pud_val(*pud))))
		return NULL;
	if (unlikely(pud_devmap(*pud)))
		return NULL;

	return follow_pmd_mask(address, pud, flags, ctx);
}

static struct page *follow_p4d_mask(unsigned long address,
				    pgd_t *pgdp,
				    unsigned int flags,
				    struct follow_page_context *ctx)
{
	p4d_t *p4d = p4d_offset(pgdp, address);

	if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d)))
		return NULL;
	if (is_hugepd(__hugepd(p4d_val(*p4d))))
		return NULL;

	return follow_pud_mask(address, p4d, flags, ctx);
}

static struct page *follow_page_mask(struct mm_struct *mm,
				     unsigned long address,
				     unsigned int flags,
				     struct follow_page_context *ctx)
{
	pgd_t *pgd = pgd_offset(mm, address);

	if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
		return NULL;
	if (pgd_huge(*pgd) || unlikely(is_hugepd(__hugepd(pgd_val(*pgd)))))
		return NULL;

	return follow_p4d_mask(address, pgd, flags, ctx);
}

#else
static struct page *follow_page_mask(struct mm_struct *mm,
				     unsigned long address,
				     unsigned int flags,
				     struct follow_page_context *ctx)
{
	return NULL;
}
#endif

static int __userptr_chunk(struct scatterlist *sg,
			   unsigned long start,
			   unsigned long max,
			   unsigned long flags,
			   int ret)
{
	unsigned long count = 0;

	do {
		unsigned long addr = start + (count << PAGE_SHIFT);
		struct page **pages = (struct page **)sg;
		unsigned int n, i;

		GEM_BUG_ON(count >= max);

		i = max - count;
		if (i > SG_MAX_SINGLE_ALLOC)
			i = I915_MAX_CHAIN_ALLOC;

		n = ret ?: pin_user_pages_fast(addr, i, flags, pages);
		if (unlikely(n != i)) {
			if (n < i)
				unpin_user_pages(pages, n);
			memset(sg, 0, i * sizeof(*sg));
			ret = -EFAULT;
		} else while (n--) {
			sg[n].page_link = (unsigned long)pages[n];
			sg[n].length = PAGE_SIZE;
			sg[n].offset = 0;
		}

		count += i;
		GEM_BUG_ON(count > max);
		if (count == max)
			break;

		sg = sg_chain_ptr(sg + I915_MAX_CHAIN_ALLOC);
	} while (1);

	return ret;
}

static void userptr_local_chunk(struct i915_tbb *tbb)
{
	struct userptr_chunk *chunk = container_of(tbb, typeof(*chunk), tbb);
	struct i915_sw_fence *fence = chunk->fence;
	int err;

	err = __userptr_chunk((struct scatterlist *)chunk,
			      chunk->addr & PAGE_MASK,
			      chunk->count,
			      chunk->addr & ~PAGE_MASK,
			      READ_ONCE(fence->error));
	i915_sw_fence_set_error_once(fence, err);

	i915_sw_fence_complete(fence);
}

static void userptr_remote_chunk(struct i915_tbb *tbb)
{
	struct userptr_chunk *chunk = container_of(tbb, typeof(*chunk), tbb);
	struct mm_struct *mm = chunk->mm;

	GEM_BUG_ON(get_mempolicy(current));

	kthread_use_mm(mm);
	set_mempolicy(current, chunk->policy);

	userptr_local_chunk(tbb);

	set_mempolicy(current, NULL);
	kthread_unuse_mm(mm);
}

static void
userptr_queue(struct userptr_chunk *chunk, struct i915_tbb_node *tbb, struct list_head *tasks)
{
	chunk->tbb.fn = userptr_remote_chunk;

	i915_tbb_lock(tbb);
	list_add_tail(&chunk->tbb.local, tasks);
	if (IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_PARALLEL_USERPTR))
		i915_tbb_add_task_locked(tbb, &chunk->tbb);
	else
		INIT_LIST_HEAD(&chunk->tbb.link);
	i915_tbb_unlock(tbb);
}

static void unpin_sg(struct scatterlist *sg, struct device *dev)
{
	for (; sg; sg = __sg_next(sg)) {
		struct page *page;

		page = sg_page(sg);
		if (unlikely(!page))
			continue;

		unpin_user_page_range_dirty_lock(page, sg->length >> PAGE_SHIFT, false);
	}
}

static int userptr_work(struct dma_fence_work *base)
{
	/* Spread the pagefaulting across the cores (~4MiB per core) */
	const unsigned int spread = max_t(unsigned int, SG_MAX_SINGLE_ALLOC, SZ_8M >> PAGE_SHIFT);
	struct userptr_work *wrk = container_of(base, typeof(*wrk), base);
	struct drm_i915_gem_object *obj = wrk->obj;
	/* user memory is likely closer to this processor than the device */
	struct i915_tbb_node *tbb = i915_tbb_node(numa_node_id());
	const unsigned int num_pages = obj->base.size >> PAGE_SHIFT;
	struct scatterlist *sgt = wrk->pages;
	struct scatterlist *sg = sgt, *tail;
	struct userptr_chunk *chunk = NULL;
	struct i915_sw_fence fence;
	struct device *dma = NULL;
	unsigned long addr;
	LIST_HEAD(tasks);
	unsigned int n;
	int cpu;

	BUILD_BUG_ON(sizeof(*chunk) > SG_NUM_INLINE * sizeof(*sg));

	addr = obj->userptr.ptr | FOLL_FORCE;
	if (!i915_gem_object_is_readonly(obj))
		addr |= FOLL_WRITE;
	BUILD_BUG_ON((FOLL_WRITE | FOLL_FORCE) & PAGE_MASK);

	if (!mmget_not_zero(obj->userptr.mm))
		return -EFAULT;

	GEM_BUG_ON(get_mempolicy(current));

	cpu = i915_tbb_suspend_local();
	kthread_use_mm(obj->userptr.mm);
	set_mempolicy(current, wrk->policy);

	i915_sw_fence_init_onstack(&fence);

	n = num_pages;
	if (n > sg_capacity(sgt))
		n = sg_capacity(sgt) - 1;
	fence.error =
		__userptr_chunk(sg, addr & PAGE_MASK, n, addr & ~PAGE_MASK, 0);

	tail = sg + n - 1;
	while (!READ_ONCE(fence.error) && n < num_pages) {
		struct scatterlist *chain;
		unsigned int x;

		/* PMD-split locks (2M), try to minimise lock contention */
		x = min_t(unsigned int, num_pages - n, SG_MAX_SINGLE_ALLOC);
		chain = sg_pool_alloc(x, I915_GFP_ALLOW_FAIL);
		if (unlikely(!chain)) {
			i915_sw_fence_set_error_once(&fence, -ENOMEM);
			break;
		}

		__sg_chain(sg + min_t(unsigned int, n, I915_MAX_CHAIN_ALLOC), chain);
		sg = chain;

		if (chunk && n + chunk->count > spread) {
			chunk->count += n;
			userptr_queue(chunk, tbb, &tasks);
			cond_resched();
			chunk = NULL;
		}

		if (chunk == NULL) {
			chunk = (struct userptr_chunk *)sg;
			chunk->fence = &fence;
			chunk->addr = addr + ((unsigned long)n << PAGE_SHIFT);
			chunk->count = -n;
			chunk->mm = obj->userptr.mm;
			chunk->policy = wrk->policy;
			i915_sw_fence_await(&fence);
		}

		n += x;
		n -= n < num_pages;
		tail = chain + x - 1;
	}
	i915_sw_fence_commit(&fence);
	n += n < num_pages; /* clear final entry (absent chain) on error */
	GEM_BUG_ON(n > num_pages);

	/* Leaving the missing chunk for ourselves */
	if (chunk) {
		chunk->count += n;
		userptr_queue(chunk, tbb, &tasks);
		i915_tbb_run_local(tbb, &tasks, userptr_local_chunk);
		i915_sw_fence_wait(&fence);
	}
	GEM_BUG_ON(!list_empty(&tasks));

	i915_sw_fence_fini(&fence);
	__sg_set_capacity(sgt, n); /* maybe cleared upon error */
	sg_mark_end(tail);

	if (unlikely(fence.error))
		goto err;

	GEM_BUG_ON(sg_capacity(sgt) != num_pages);
	GEM_BUG_ON(__sg_total_length(sgt, false) != obj->base.size);
	if (i915_gem_object_can_bypass_llc(obj))
		drm_clflush_sg(&sg_table(sgt));

	dma = obj->base.dev->dev;
	fence.error = i915_sg_map(sgt, obj->base.size, i915_gem_sg_segment_size(obj), dma);
	if (unlikely(fence.error)) {
err:		unpin_sg(sgt, dma);
		i915_sg_free_excess(sgt);
	}
	if (IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GEM) && !fence.error) {
		GEM_BUG_ON(__sg_total_length(sgt, false) != obj->base.size);
		GEM_BUG_ON(__sg_total_length(sgt, true) != obj->base.size);
	}

	set_mempolicy(current, NULL);
	kthread_unuse_mm(obj->userptr.mm);
	i915_tbb_resume_local(cpu);
	mmput(obj->userptr.mm);
	return fence.error;
}

static void put_page_range(struct page *page, long length, long step)
{
	long x;

	x = page_to_phys(page) & (step - 1);
	page -= x >> PAGE_SHIFT;
	length += x;

	for (x = 0 ; x < length; x += step)
		put_page(nth_page(page, x >> PAGE_SHIFT));
}

static int userptr_imm(struct drm_i915_gem_object *obj, struct scatterlist *sgt)
{
	struct scatterlist *sg = sgt, *chain = sgt + SG_NUM_INLINE - 1;
	struct device *dev = obj->base.dev->dev;
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct mm_struct *mm = obj->userptr.mm;
	unsigned long addr = obj->userptr.ptr;
	unsigned long end = addr + obj->base.size;
	struct follow_page_context ctx = {};
	struct scatterlist *map = NULL;
	unsigned long phys = -1, flags;
	unsigned long iova, mapped;

	sg_init_inline(sgt);
	sgt->length = 0;

	if (!IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_UPTR_IMM_2M))
		return -ERESTARTSYS;

	flags = FOLL_FORCE;
	if (!i915_gem_object_is_readonly(obj))
		flags |= FOLL_WRITE;

	do {
		unsigned long p_phys;
		unsigned long len;
		struct page *page;

		rcu_read_lock();
		do
			page = follow_page_mask(mm, addr, flags, &ctx);
		while (unlikely(page == ERR_PTR(-EAGAIN)));
		rcu_read_unlock();
		if (!page)
			break;

		if (!map && domain) {
			iova = __i915_iommu_alloc(obj->base.size, i915_dma_limit(dev), domain);
			if (IS_ERR_VALUE(iova)) {
				put_page(page);
				return iova;
			}

			map = sgt;
			sg_dma_address(map) = iova;
			sg_dma_len(map) = 0;
			mapped = 0;
		}

		len = addr & (ctx.page_size - 1);
		page += len >> PAGE_SHIFT;

		len = min_t(unsigned long, ctx.page_size - len, end - addr);
		sg_page_sizes(sgt) |= len;

		/* Hopefully we can combine together 64K pages */
		p_phys = page_to_phys(page);
		if (phys != p_phys || ctx.page_size != sg->offset || sg->length >= SZ_2G) {
			if (sg->length) {
				GEM_BUG_ON(!sg_page(sg));
				if (!domain) {
					sg_dma_address(sg) = __sg_phys(sg);
					sg_dma_len(sg) = sg->length;
				} else {
					if (sg_dma_len(map) > UINT_MAX - sg->length) {
						map = __sg_next(map);
						sg_dma_address(map) = iova + mapped;
						sg_dma_len(map) = 0;
					}

					if (__i915_iommu_map(domain, iova + mapped,
							     __sg_phys(sg), sg->length,
							     IOMMU_READ | IOMMU_WRITE, GFP_KERNEL,
							     &mapped))
						break;

					sg_dma_len(map) += sg->length;
				}

				if (sg == chain) {
					unsigned int x;

					x = min_t(unsigned int,
						  ((end - addr - len) >> PAGE_SHIFT) + 2,
						  SG_MAX_SINGLE_ALLOC);
					chain = sg_pool_alloc(x, GFP_NOWAIT | __GFP_NOWARN);
					if (unlikely(!chain))
						break;

					__sg_chain(sg, memcpy(chain, sg, sizeof(*sg)));
					GEM_BUG_ON(sg_chain_ptr(sg) != chain);

					sg = chain;
					chain += x - 1;
					sg_capacity(sgt) += x - 1;
				}
				GEM_BUG_ON(sg_is_last(sg));
				GEM_BUG_ON(sg_is_chain(sg));
				sg++;
			}

			sg->page_link = (unsigned long)page;
			sg->offset = ctx.page_size;
			sg->length = 0;
			sg_count(sgt)++;
			GEM_BUG_ON(sg_count(sgt) > sg_capacity(sgt));

			phys = p_phys;
		}
		sg->length += len;
		phys += len;
		addr += len;
		if (addr == end) {
			if (!domain) {
				sg_dma_address(sg) = __sg_phys(sg);
				sg_dma_len(sg) = sg->length;
			} else {
				if (sg_dma_len(map) > UINT_MAX - sg->length) {
					map = __sg_next(map);
					sg_dma_address(map) = iova + mapped;
					sg_dma_len(map) = 0;
				}

				if (__i915_iommu_map(domain, iova + mapped,
						     __sg_phys(sg), sg->length,
						     IOMMU_READ | IOMMU_WRITE, GFP_KERNEL,
						     &mapped))
					break;

				GEM_BUG_ON(mapped != obj->base.size);
				sg_dma_len(map) += sg->length;
				if (map != sg)
					sg_dma_len(__sg_next(map)) = 0; /* iommu terminator */
			}
			sg_mark_end(sg);

			if (domain && domain->ops->iotlb_sync_map)
				domain->ops->iotlb_sync_map(domain, iova, mapped);

			GEM_BUG_ON(__sg_total_length(sgt, false) != obj->base.size);
			GEM_BUG_ON(__sg_total_length(sgt, true) != obj->base.size);
			__set_bit(I915_BO_FAST_GUP_BIT, &obj->flags);
			return 0;
		}
	} while(1);

	if (unlikely(sg_count(sgt))) {
		sg_mark_end(sg);

		if (map)
			__i915_iommu_free(iova, obj->base.size, mapped, domain);

		for (sg = sgt; sg; sg = __sg_next(sg))
			put_page_range(sg_page(sg), sg->length, sg->offset);

		i915_sg_free_excess(sgt);
		sg_dma_len(sgt) = 0;
	}

	return -ERESTARTSYS; /* retry from kworker */
}

static const struct dma_fence_work_ops userptr_ops = {
	.name = "userptr",
	.work = userptr_work,
};

#ifndef BPM_VM_FLAGS_IS_READ_ONLY_FLAG
#define vm_flags_set(vma, flags) (vma)->vm_flags |= (flags)
#endif

static int
probe_range(struct mm_struct *mm, unsigned long addr, unsigned long len)
{
#ifndef BPM_STRUCT_VM_AREA_STRUCT_VM_NEXT_NOT_PRESENT
#define VMA_ITERATOR(it_, mm_, va_) unsigned long it_ = (va_)
#define for_each_vma_range(it_, vma_, end_) \
	for (vma_ = find_vma(mm, it_); vma_; vma_ = (vma_)->vm_next)
#endif
	const unsigned long end = addr + len;
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, addr);
	int ret = -EFAULT;

	mmap_read_lock(mm);
	for_each_vma_range(vmi, vma, end) {
		if (vma->vm_start > addr)
			break;

		if (vma->vm_flags & (VM_IO | VM_PFNMAP))
			break;

		if (no_init_on_alloc &&
		    round_down(min(vma->vm_end, end), SZ_1M) > round_up(addr, SZ_1M))
			vm_flags_set(vma, VM_HUGEPAGE);

		if (vma->vm_end >= end) {
			ret = 0;
			break;
		}

		addr = vma->vm_end;
	}
	mmap_read_unlock(mm);

	return ret;
}

static int i915_gem_userptr_get_pages(struct drm_i915_gem_object *obj)
{
	struct userptr_work *wrk;
	struct scatterlist *sg;
	unsigned int num_pages; /* limited by sg_alloc_table */
	int err;

	if (!safe_conversion(&num_pages, obj->base.size >> PAGE_SHIFT))
		return -E2BIG;

	sg = __sg_table_inline_create(I915_GFP_ALLOW_FAIL);
	if (unlikely(!sg))
		return -ENOMEM;

	atomic64_sub(obj->base.size, &obj->mm.region.mem->avail);
	if (!userptr_imm(obj, sg))
		goto out;

	wrk = kmalloc(sizeof(*wrk), GFP_KERNEL);
	if (!wrk) {
		err = -ENOMEM;
		goto err_sg;
	}
	dma_fence_work_init(&wrk->base, &userptr_ops,
			    to_i915(obj->base.dev)->mm.sched);
	wrk->obj = obj;
	wrk->pages = sg;
	wrk->policy = get_mempolicy(current);
	wrk->base.cpu = raw_smp_processor_id();

	i915_gem_object_migrate_prepare(obj, &wrk->base.rq.fence);
	dma_fence_work_commit(&wrk->base);
	set_tsk_need_resched(current);
out:
	__i915_gem_object_set_pages(obj, sg);
	return 0;

err_sg:
	atomic64_add(obj->base.size, &obj->mm.region.mem->avail);
	sg_table_inline_free(sg);
	return err;
}

static int
i915_gem_userptr_put_pages(struct drm_i915_gem_object *obj,
			   struct scatterlist *pages)
{
	struct iommu_domain *domain;
	struct scatterlist *sg;
	bool dirty, gup;

	i915_gem_object_migrate_finish(obj);
	if (!sg_count(pages))
		goto out;

	intel_tlb_sync(to_i915(obj->base.dev), obj->mm.tlb);

	/*
	 * We always mark objects as dirty when they are used by the GPU,
	 * just in case. However, if we set the vma as being read-only we know
	 * that the object will never have been written to.
	 */
	dirty = !i915_gem_object_is_readonly(obj);

	domain = iommu_get_domain_for_dev(obj->base.dev->dev);
	if (domain && sg_dma_len(pages))
		__i915_iommu_free(sg_dma_address(pages), obj->base.size, obj->base.size, domain);

	gup = __test_and_clear_bit(I915_BO_FAST_GUP_BIT, &obj->flags);
	for (sg = pages; sg; sg = __sg_next(sg)) {
		struct page *page;

		page = sg_page(sg);
		if (unlikely(!page))
			break;

		GEM_BUG_ON(!sg->length);

		if (gup) {
			if (dirty && !PageDirty(page))
				set_page_dirty_lock(page);
			put_page_range(page, sg->length, sg->offset);
		} else {
			unpin_user_page_range_dirty_lock(page, sg->length >> PAGE_SHIFT, dirty);
		}
	}

out:
	atomic64_add(obj->base.size, &obj->mm.region.mem->avail);
	sg_table_inline_free(pages);
	return 0;
}

static int
i915_gem_userptr_dmabuf_export(struct drm_i915_gem_object *obj)
{
	drm_dbg(obj->base.dev, "Exporting userptr no longer allowed\n");

	return -EINVAL;
}

static const struct drm_i915_gem_object_ops i915_gem_userptr_ops = {
	.name = "i915_gem_object_userptr",
	.flags = I915_GEM_OBJECT_HAS_STRUCT_PAGE |
		 I915_GEM_OBJECT_NO_MMAP,
	.get_pages = i915_gem_userptr_get_pages,
	.put_pages = i915_gem_userptr_put_pages,
	.dmabuf_export = i915_gem_userptr_dmabuf_export,
	.release = i915_gem_userptr_release,
};

/*
 * Creates a new mm object that wraps some normal memory from the process
 * context - user memory.
 *
 * We impose several restrictions upon the memory being mapped
 * into the GPU.
 * 1. It must be page aligned (both start/end addresses, i.e ptr and size).
 * 2. It must be normal system memory, not a pointer into another map of IO
 *    space (e.g. it must not be a GTT mmapping of another object).
 * 3. We only allow a bo as large as we could in theory map into the GTT,
 *    that is we limit the size to the total size of the GTT.
 * 4. The bo is marked as being snoopable. The backing pages are left
 *    accessible directly by the CPU, but reads and writes by the GPU may
 *    incur the cost of a snoop (unless you have an LLC architecture).
 *
 * Synchronisation between multiple users and the GPU is left to userspace
 * through the normal set-domain-ioctl. The kernel will enforce that the
 * GPU relinquishes the VMA before it is returned back to the system
 * i.e. upon free(), munmap() or process termination. However, the userspace
 * malloc() library may not immediately relinquish the VMA after free() and
 * instead reuse it whilst the GPU is still reading and writing to the VMA.
 * Caveat emptor.
 *
 * Also note, that the object created here is not currently a "first class"
 * object, in that several ioctls are banned. These are the CPU access
 * ioctls: mmap(), pwrite and pread. In practice, you are expected to use
 * direct access via your pointer rather than use those ioctls. Another
 * restriction is that we do not allow userptr surfaces to be pinned to the
 * hardware and so we reject any attempt to create a framebuffer out of a
 * userptr.
 *
 * If you think this is a good interface to use to pass GPU memory between
 * drivers, please use dma-buf instead. In fact, wherever possible use
 * dma-buf instead.
 */
int
i915_gem_userptr_ioctl(struct drm_device *dev,
		       void *data,
		       struct drm_file *file)
{
	struct drm_i915_private *i915 = to_i915(dev);
	struct drm_i915_gem_userptr *args = data;
	struct drm_i915_gem_object *obj;
	u32 handle;
	int ret;

	if (!HAS_LLC(i915) && !HAS_SNOOP(i915)) {
		/* We cannot support coherent userptr objects on hw without
		 * LLC and broken snooping.
		 */
		return -ENODEV;
	}

	if (args->flags & ~(I915_USERPTR_READ_ONLY |
			    I915_USERPTR_UNSYNCHRONIZED))
		return -EINVAL;

	if (i915_gem_object_size_2big(args->user_size))
		return -E2BIG;

	if (!args->user_size ||
	    offset_in_page(args->user_ptr | args->user_size))
		return -EINVAL;

	if (!access_ok(u64_to_user_ptr(args->user_ptr), args->user_size))
		return -EFAULT;

	if (args->flags & I915_USERPTR_UNSYNCHRONIZED)
		return -ENODEV;

	if (args->flags & I915_USERPTR_READ_ONLY) {
		/*
		 * On almost all of the older hw, we cannot tell the GPU that
		 * a page is readonly.
		 */
		if (!to_gt(i915)->vm->has_read_only)
			return -ENODEV;
	}

	ret = probe_range(current->mm, args->user_ptr, args->user_size);
	if (unlikely(ret))
		return ret;

	obj = i915_gem_object_alloc();
	if (obj == NULL)
		return -ENOMEM;

	drm_gem_private_object_init(dev, &obj->base, args->user_size);
	i915_gem_object_init(obj, &i915_gem_userptr_ops, I915_BO_ALLOC_USER);
	i915_gem_object_set_cache_coherency(obj, I915_CACHE_LLC);

	obj->userptr.ptr = args->user_ptr;
	if (args->flags & I915_USERPTR_READ_ONLY)
		i915_gem_object_set_readonly(obj);

	i915_gem_userptr_init__mm(obj);
	i915_gem_object_init_memory_region(obj,
					   i915->mm.regions[INTEL_REGION_SMEM]);

	/* drop reference from allocate - handle holds it now */
	ret = drm_gem_handle_create(file, &obj->base, &handle);
	i915_gem_object_put(obj);
	if (ret)
		return ret;

	args->handle = handle;
	return 0;
}
