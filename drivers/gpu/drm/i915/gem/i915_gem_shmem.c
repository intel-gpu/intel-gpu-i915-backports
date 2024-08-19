/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2014-2016 Intel Corporation
 */

#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/mm.h>
#include <linux/numa.h>
#include <linux/pagevec.h>
#include <linux/shmem_fs.h>
#include <linux/swap.h>

#include <asm-generic/getorder.h>

#include "gt/intel_context.h"
#include "gt/intel_gt.h"
#include "gt/intel_tlb.h"

#include "dma_tx.h"
#include "i915_drv.h"
#include "i915_gem_object.h"
#include "i915_gem_region.h"
#include "i915_gem_shmem.h"
#include "i915_gemfs.h"
#include "i915_memcpy.h"
#include "i915_scatterlist.h"
#include "i915_sw_fence_work.h"
#include "i915_trace.h"

#define MAX_PAGE SZ_2M

static struct kmem_cache *slab_clear;

static inline struct shmem_private {
	struct clear_pages {
		spinlock_t lock;
		struct list_head clean;
		struct list_head dirty;
	} clear[MAX_ORDER];
	atomic_t clear_pages;
	int max_clear_pages;

	struct ras_errors {
		unsigned int max;
		struct ras_error {
			struct dev_ext_attribute attr;
			unsigned long count;
			char *name;
		} errors[];
	} *errors;
} *to_shmem_private(const struct intel_memory_region *mem) { return mem->region_private; }

struct i915_dma_engine {
	struct rb_node node;
	struct dma_chan *dma;
	struct page *zero;
	dma_addr_t zero_dma;
	int cpu;
};

static struct rb_root i915_dma_engines;
static DEFINE_SPINLOCK(i915_dma_lock);

static int __local_cpu(int nid)
{
	return nid == NUMA_NO_NODE ? 0 : cpumask_first(cpumask_of_node(nid));
}

static int local_cpu(struct device *dev)
{
	return __local_cpu(dev_to_node(dev));
}

static int mem_cpu(struct intel_memory_region *mem)
{
	return local_cpu(mem->i915->drm.dev);
}

static int __local_node(int nid)
{
	return nid == NUMA_NO_NODE ? 0 : nid;
}

static int local_node(struct device *dev)
{
	return __local_node(dev_to_node(dev));
}

static int mem_node(struct intel_memory_region *mem)
{
	return local_node(mem->i915->drm.dev);
}

static bool channel_filter(struct dma_chan *chan, void *param)
{
	return local_cpu(chan->device->dev) == (long)param;
}

static struct dma_chan *get_dma_channel(long cpu)
{
	dma_cap_mask_t dma_mask;
	struct dma_chan *chan;

	dma_cap_zero(dma_mask);
	dma_cap_set(DMA_INTERRUPT, dma_mask);
	dma_cap_set(DMA_MEMSET, dma_mask);
	chan = dma_request_channel(dma_mask, channel_filter, (void *)cpu);
	if (!IS_ERR_OR_NULL(chan))
		return chan;

	dma_cap_clear(DMA_MEMSET, dma_mask);
	dma_cap_set(DMA_MEMCPY, dma_mask);
	chan = dma_request_channel(dma_mask, channel_filter, (void *)cpu);
	if (!IS_ERR_OR_NULL(chan))
		return chan;

	return NULL;
}

static int __i915_dma_engine_cmp(int cpu, const struct rb_node *node)
{
	return cpu - rb_entry(node, struct i915_dma_engine, node)->cpu;
}

static int i915_dma_engine_cmp(const void *key, const struct rb_node *node)
{
	return __i915_dma_engine_cmp((long)key, node);
}

static int i915_dma_engine_add(struct rb_node *key, const struct rb_node *node)
{
	return __i915_dma_engine_cmp(rb_entry(key, struct i915_dma_engine, node)->cpu, node);
}

static struct i915_dma_engine *lookup_dma_engine(long cpu)
{
	return rb_entry(rb_find((void *)cpu, &i915_dma_engines, i915_dma_engine_cmp), struct i915_dma_engine, node);
}

static struct i915_dma_engine *get_dma_engine(long cpu)
{
	struct i915_dma_engine *new, *old;
	struct dma_chan *chan;

	if (!IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_SMEM_DMA))
		return NULL;

	do {
		old = lookup_dma_engine(cpu);
		if (likely(old))
			return old;

		chan = get_dma_channel(cpu);
		if (!chan && cpu) {
			cpu = 0;
			continue;
		}
		if (!chan)
			goto err;

		new = kzalloc(sizeof(*new), GFP_KERNEL);
		if (!new)
			goto err_chan;

		new->cpu = local_cpu(chan->device->dev);
		new->dma = chan;

		if (!dma_has_cap(DMA_MEMSET, chan->device->cap_mask)) {
			new->zero = alloc_pages_node(dev_to_node(chan->device->dev),
						     GFP_KERNEL | __GFP_THISNODE | __GFP_ZERO,
						     get_order(MAX_PAGE));
			if (!new->zero)
				goto err_engine;

			new->zero_dma = dma_map_page(chan->device->dev,
						     new->zero, 0, MAX_PAGE,
						     DMA_TO_DEVICE);
			if (!new->zero_dma)
				goto err_page;
		}

		spin_lock(&i915_dma_lock);
		old = rb_entry(rb_find_add(&new->node, &i915_dma_engines, i915_dma_engine_add), typeof(*old), node);
		spin_unlock(&i915_dma_lock);
		if (old)
			goto err_dma;

		if (new->cpu == cpu)
			return new;
	} while (1);

err_dma:
	if (new->zero_dma)
		dma_unmap_page_attrs(new->dma->device->dev,
				     new->zero_dma, MAX_PAGE,
				     DMA_TO_DEVICE,
				     DMA_ATTR_SKIP_CPU_SYNC);
err_page:
	if (new->zero)
		__free_pages(new->zero, get_order(MAX_PAGE));
err_engine:
	kfree(new);
err_chan:
	dma_release_channel(chan);
err:
	return lookup_dma_engine(cpu);
}

static struct dma_fence *dma_clear(struct i915_dma_engine *de, dma_addr_t addr, int length)
{
	if (de->zero_dma)
		return dma_async_tx_memcpy(de->dma, de->zero_dma, addr, length);
	else
		return dma_async_tx_memset(de->dma, addr, 0, length);
}

struct shmem_work;

struct shmem_error {
	struct dma_fence_work base;
	struct i915_sw_dma_fence_cb cb;
	struct i915_dependency dep;
	struct shmem_work *work;
};

static void fence_chain(struct i915_request *rq,
			struct dma_fence *f,
			struct i915_sw_dma_fence_cb *cb,
			struct i915_dependency *dep)
{
	GEM_BUG_ON(i915_sw_fence_done(&rq->submit));

	if (IS_ERR_OR_NULL(f)) {
		i915_sw_fence_set_error_once(&rq->submit, PTR_ERR(f));
		return;
	}

	if (!__i915_sw_fence_await_dma_fence(&rq->submit, f, cb))
		return;

	if (!dma_fence_is_i915(f))
		return;

	if (dep)
		__i915_sched_node_add_dependency(&rq->sched,
						 &to_request(f)->sched,
						 dep,
						 0);
}

static void error_inject(struct shmem_error *e, struct dma_fence *f)
{
	fence_chain(&e->base.rq, f, &e->cb, &e->dep);
}

static void add_clear_fences(struct i915_request *rq, struct list_head *fences)
{
	struct clear_page *cp;

	if (list_empty(fences))
		return;

	rcu_read_lock();
	list_for_each_entry(cp, fences, link)
		fence_chain(rq, rcu_dereference(cp->active.fence),
			    &cp->cb, NULL /* XXX &cp->dep */);
	rcu_read_unlock();
}

struct shmem_work {
	struct dma_fence_work base;
	struct drm_i915_gem_object *obj;
	struct mempolicy *policy;
	struct sg_table *pages;
	struct shmem_error *error;
};

struct shmem_chunk {
	struct scatterlist *sg;
	struct work_struct work;
	struct intel_memory_region *mem;
	struct address_space *mapping;
	struct i915_sw_fence *fence;
	struct mempolicy *policy;
	unsigned long idx, end;
	unsigned long flags;
#define SHMEM_CLEAR	BIT(0)
#define SHMEM_CLFLUSH	BIT(1)
#define SHMEM_CACHE	BIT(2)
};

#if IS_ENABLED(CONFIG_NUMA)
#define swap_mempolicy(tsk, pol) ({ \
	struct mempolicy *old__ = (tsk)->mempolicy; \
	(tsk)->mempolicy = (pol); \
	(pol) = old__; \
})
#define get_mempolicy(tsk) ((tsk)->mempolicy)
#else
#define swap_mempolicy(tsk, pol)
#define get_mempolicy(tsk) NULL
#endif

static struct page *
shmem_get_page(struct intel_memory_region *mem,
	       struct address_space *mapping,
	       unsigned long idx)
{
	struct page *page;
	gfp_t gfp;

	/*
	 * Our bo are always dirty and so we require
	 * kswapd to reclaim our pages (direct reclaim
	 * does not effectively begin pageout of our
	 * buffers on its own). However, direct reclaim
	 * only waits for kswapd when under allocation
	 * congestion. So as a result __GFP_RECLAIM is
	 * unreliable and fails to actually reclaim our
	 * dirty pages -- unless you try over and over
	 * again with !__GFP_NORETRY. However, we still
	 * want to fail this allocation rather than
	 * trigger the out-of-memory killer and for
	 * this we want __GFP_RETRY_MAYFAIL.
	 */
	gfp = mapping_gfp_constraint(mapping, ~__GFP_RECLAIM);
	page = shmem_read_mapping_page_gfp(mapping, idx, gfp);
	if (!IS_ERR(page))
		return page;

	/* Preferentially reap our own buffer objects before swapping */
	intel_memory_region_evict(mem, NULL, SZ_2M, jiffies - HZ, PAGE_SIZE);

	/*
	 * We've tried hard to allocate the memory by reaping
	 * our own buffer, now let the real VM do its job and
	 * go down in flames if truly OOM.
	 *
	 * However, since graphics tend to be disposable,
	 * defer the oom here by reporting the ENOMEM back
	 * to userspace.
	 */
	gfp = mapping_gfp_constraint(mapping, ~__GFP_RETRY_MAYFAIL);
	return shmem_read_mapping_page_gfp(mapping, idx, gfp);
}

static bool is_clear_page(struct page *page)
{
	struct clear_page *cp = to_clear_page(page);
	struct dma_fence *f;
	bool ret = false;

	f = i915_active_fence_get_or_error(&cp->active);
	if (!f)
		return true;

	if (!IS_ERR(f)) {
		if (dma_fence_wait(f, false) == 0)
			ret = f->error == 0;
		dma_fence_put(f);
	}

	return ret;
}

static void mark_clear(struct page *page)
{
	RCU_INIT_POINTER(to_clear_page(page)->active.fence, NULL);
}

static int __shmem_chunk(struct scatterlist *sg,
			 struct intel_memory_region *mem,
			 struct address_space *mapping,
			 struct mempolicy *mempolicy,
			 unsigned long idx,
			 unsigned long end,
			 unsigned long flags,
			 int *error)
{
	int err = 0;

	GEM_BUG_ON(idx >= end);

	swap_mempolicy(current, mempolicy);
	do {
		struct page *page = sg_page(sg);
		bool clear = false;

		if (!page) {
			/* try to backoff quickly if any of our threads fail */
			err = READ_ONCE(*error);
			if (err)
				break;

			GEM_BUG_ON(!mapping);
			page = shmem_get_page(mem, mapping, idx);
			if (IS_ERR(page)) {
				err = PTR_ERR(page);
				break;
			}

			sg_set_page(sg, page, PAGE_SIZE, 0);
		} else {
			if (is_clear_page(page))
				goto skip;

			clear = true;
		}

		if (flags) {
			void *ptr = kmap_atomic(page);

			if (flags & SHMEM_CLEAR) {
				if (flags & SHMEM_CACHE || !i915_memclear_nocache(ptr, sg->length))
					memset(ptr, 0, sg->length);
				if (clear)
					mark_clear(page);
			}
			if (flags & SHMEM_CLFLUSH)
				clflush_cache_range(ptr, sg->length);

			kunmap_atomic(ptr);
		}

skip:
		if (++idx == end)
			break;

		sg = __sg_next(sg);
		GEM_BUG_ON(!sg);
	} while (1);
	swap_mempolicy(current, mempolicy);

	return err;
}

static void shmem_chunk(struct work_struct *wrk)
{
	struct shmem_chunk *chunk = container_of(wrk, typeof(*chunk), work);
	struct intel_memory_region *mem = chunk->mem;
	struct address_space *mapping = chunk->mapping;
	struct i915_sw_fence *fence = chunk->fence;
	struct mempolicy *policy = chunk->policy;
	struct scatterlist *sg = chunk->sg;
	unsigned long flags = chunk->flags;
	unsigned long idx = chunk->idx;
	unsigned long end = chunk->end;

	if (sg == (void *)chunk)
		memset(chunk, 0, sizeof(*chunk));
	else
		kunmap(sg_page(sg));

	if (!READ_ONCE(fence->error)) {
		int err;

		err = __shmem_chunk(sg, mem, mapping, policy,
				    idx, end, flags,
				    &fence->error);
		i915_sw_fence_set_error_once(fence, err);
	}

	i915_sw_fence_complete(fence);
}

static void
shmem_queue(struct shmem_chunk *chunk, struct drm_i915_private *i915)
{
	if (IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_PARALLEL_SHMEMFS))
		queue_work_on(i915_next_online_cpu(i915),
			      system_unbound_wq,
			      &chunk->work);
	else
		shmem_chunk(&chunk->work);
}

static int preferred_node(const struct drm_i915_gem_object *obj)
{
	int nid = NUMA_NO_NODE;

	if (!IS_ENABLED(CONFIG_NUMA))
		return NUMA_NO_NODE;

	if (obj->mempol == I915_GEM_CREATE_MPOL_LOCAL) {
	} else if (!obj->maxnode) {
		nid = dev_to_node(obj->base.dev->dev);
	} else {
		nid = find_first_bit(get_obj_nodes(obj), obj->maxnode);
		if (nid == obj->maxnode)
			nid = NUMA_NO_NODE;
	}

	if (nid == NUMA_NO_NODE)
		nid = numa_node_id();

	return nid;
}

static void ras_error(struct drm_i915_gem_object *obj)
{
	struct shmem_private *mp = to_shmem_private(obj->mm.region.mem);
	struct ras_errors *e = mp->errors;
	int nid = preferred_node(obj);

	if (!e || nid >= e->max)
		return;

	WRITE_ONCE(e->errors[nid].count, READ_ONCE(e->errors[nid].count) + 1);
}

static struct page *
alloc_pages_for_object(struct drm_i915_gem_object *obj,
		       int *interleave,
		       gfp_t gfp,
		       int order)
{
	struct page *page = NULL;

	if (obj->mempol == I915_GEM_CREATE_MPOL_LOCAL)
		return alloc_pages_node(numa_node_id(), gfp | __GFP_THISNODE, order);

	if (obj->mempol && obj->maxnode) {
		const unsigned long *nodes = get_obj_nodes(obj);
		int max = READ_ONCE(*interleave);
		int nid = max;

		for_each_set_bit_from(nid, nodes, obj->maxnode) {
			page = alloc_pages_node(nid, gfp | __GFP_THISNODE, order);
			if (page) {
				if (obj->mempol == I915_GEM_CREATE_MPOL_INTERLEAVED)
					WRITE_ONCE(*interleave, nid + 1);
				return page;
			}
		}

		for_each_set_bit(nid, nodes, max) {
			page = alloc_pages_node(nid, gfp | __GFP_THISNODE, order);
			if (page) {
				if (obj->mempol == I915_GEM_CREATE_MPOL_INTERLEAVED)
					WRITE_ONCE(*interleave, nid + 1);
				return page;
			}
		}

		if (!(gfp & __GFP_DIRECT_RECLAIM))
			return page; /* Try again with a smaller pagesize */
	}

	if (obj->mempol != I915_GEM_CREATE_MPOL_BIND)
		page = alloc_pages_node(dev_to_node(obj->base.dev->dev), gfp, order);

	return page;
}

static unsigned long
shmem_create_mode(const struct drm_i915_gem_object *obj, bool movntda)
{
	unsigned long flags = 0;

	if ((obj->flags & (I915_BO_ALLOC_USER | I915_BO_CPU_CLEAR)) &&
	    !(obj->flags & I915_BO_SKIP_CLEAR))
		flags |= SHMEM_CLEAR;

	if (i915_gem_object_can_bypass_llc(obj) ||
	    !(obj->flags & I915_BO_CACHE_COHERENT_FOR_WRITE)) {
		if (!(flags & SHMEM_CLEAR && movntda))
			flags |= SHMEM_CLFLUSH;
	}

	if (obj->flags & I915_BO_SYNC_HINT)
		flags |= SHMEM_CACHE;

	return flags;
}

static struct intel_context *
prefer_blt(struct drm_i915_gem_object *obj, unsigned int flags)
{
	struct intel_gt *gt = obj->mm.region.mem->gt;
	struct intel_context *ce;
	int num_threads;
	u64 limit;

	if (!IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_SMEM_BLT))
		return NULL;

	if (!(flags & SHMEM_CLEAR))
		return NULL;

	num_threads = min_t(u32, gt->i915->sched->num_cpus, 1 + (obj->base.size >> ilog2(SZ_4M)));
	num_threads = 1 + ilog2(num_threads);

	limit = num_threads << ilog2(SZ_4M);
	if (obj->flags & I915_BO_SYNC_HINT)
		limit = max_t(u64, limit, SZ_16M);

	if (obj->base.size <= limit)
		return NULL;

	ce = i915_gem_get_active_smem_context(gt);
	if (ce && !intel_context_throttle(ce, 0))
		return ce;

	return NULL;
}

static int __fence_started(struct i915_active_fence *ref)
{
	struct dma_fence *f;
	int ret;

	rcu_read_lock();
	f = rcu_dereference(ref->fence);
	if (IS_ERR_OR_NULL(f))
		ret = 1;
	else if (dma_fence_is_i915(f) &&
		 __i915_request_has_started(to_request(f)))
		ret = 0;
	else
		ret = -1;
	rcu_read_unlock();

	return ret;
}

static struct page *
get_clear_page(struct intel_memory_region *mem, int order,
	       int maxnode, const unsigned long *nodes,
	       bool use_blt, unsigned int flags,
	       struct list_head *fences)
{
	const int local = mem_node(mem);
	struct shmem_private *mp = to_shmem_private(mem);
	struct clear_pages *pages = &mp->clear[order];
	struct list_head *lists[] = {
		&pages->dirty,
		&pages->clean,
	};
	struct clear_page *cp, *cn;
	struct page *page = NULL;
	int i, nid;

	GEM_BUG_ON(order >= ARRAY_SIZE(mp->clear));
	if (flags & SHMEM_CLEAR)
		swap(lists[0], lists[1]);

	spin_lock(&pages->lock);
	for (i = 0; i < ARRAY_SIZE(lists); i++) {
		list_for_each_entry_safe(cp, cn, lists[i], link) {
			if (!cp->page)
				continue;

			if (flags & SHMEM_CLEAR && i915_active_fence_has_error(&cp->active)) {
				list_move(&cp->link, &pages->dirty);
				continue;
			}

			nid = page_to_nid(cp->page);
			if (maxnode && (nid >= maxnode || !test_bit(nid, nodes)))
				continue;
			if (!maxnode && nid != local)
				continue;

			/* Keep searching for a short while for an idle page */
			if (__fence_started(&cp->active) < 0 && !use_blt)
				break;

			__list_del_entry(&cp->link);
			if (i915_active_fence_isset(&cp->active))
				list_add_tail(&cp->link, fences);

			page = cp->page;
			goto out;
		}

		flags = 0;
	}
out:
	spin_unlock(&pages->lock);

	return page;
}

static void __add_clear_page(struct shmem_private *mp, struct clear_page *cp, struct clear_pages *pages)
{
	struct dma_fence *f = rcu_access_pointer(cp->active.fence);
	struct list_head *head;

	GEM_BUG_ON(!PagePrivate(cp->page));
	GEM_BUG_ON(to_clear_page(cp->page) != cp);

	head = &pages->clean;
	if (IS_ERR(f))
		head = &pages->dirty;
	if (f)
		head = head->prev;
	list_add(&cp->link, head);
	mark_page_accessed(cp->page);
}

static void add_clear_page(struct intel_memory_region *mem, struct clear_page *cp, int order)
{
	struct shmem_private *mp = to_shmem_private(mem);
	struct clear_pages *pages = &mp->clear[order];

	spin_lock(&pages->lock);
	__add_clear_page(mp, cp, pages);
	spin_unlock(&pages->lock);
}

static void keep_sg(struct intel_memory_region *mem,
		    struct scatterlist *sg,
		    struct drm_i915_gem_object *obj)
{
	struct shmem_private *mp = to_shmem_private(mem);
	struct clear_pages *pages;
	unsigned int length = 0;
	spinlock_t *lock = NULL;

	for (; sg; sg = __sg_next(sg)) {
		struct clear_page *cp;
		struct page *page;

		page = sg_page(sg);
		if (!page)
			break;

		if (sg->length != length) {
			if (lock)
				spin_unlock(lock);

			length = sg->length;
			GEM_BUG_ON(!is_power_of_2(length));

			pages = &mp->clear[get_order(length)];
			lock = &pages->lock;
			spin_lock(lock);
		}

		cp = to_clear_page(page);
		__add_clear_page(mp, cp, pages);
		if (obj)
			memcpy(cp->tlb, obj->mm.tlb, sizeof(obj->mm.tlb));
	}

	if (lock)
		spin_unlock(lock);
}

static void release_clear_page(struct intel_memory_region *mem, struct page *page, int order, u32 *tlb)
{
	struct clear_page *cp = to_clear_page(page);

	might_sleep();

	i915_active_fence_fini(&cp->active);

	intel_tlb_sync(mem->i915, tlb ?: cp->tlb);
	dma_unmap_page_attrs(mem->i915->drm.dev,
			     cp->dma[0], BIT(order + PAGE_SHIFT),
			     DMA_BIDIRECTIONAL,
			     DMA_ATTR_SKIP_CPU_SYNC);
	if (cp->dma[1])
		dma_unmap_page_attrs(cp->engine->dma->device->dev,
				     cp->dma[1], BIT(order + PAGE_SHIFT),
				     DMA_FROM_DEVICE,
				     DMA_ATTR_SKIP_CPU_SYNC);

	kmem_cache_free(slab_clear, cp);

	ClearPagePrivate(page);
	page->private = 0;

	atomic_sub(BIT(order), &to_shmem_private(mem)->clear_pages);
}

unsigned long i915_gem_reap_clear_smem(struct intel_memory_region *mem, int order, unsigned long target)
{
	struct clear_page bookmark = {};
	struct shmem_private *mp;
	size_t count = 0;

	mp = to_shmem_private(mem);
	if (unlikely(!mp))
		return 0;

	for (; count < target && order < ARRAY_SIZE(mp->clear); order++) {
		struct clear_pages *pages = &mp->clear[order];
		struct list_head *lists[] = {
			&pages->dirty,
			&pages->clean,
		};
		struct clear_page *cp;
		int i;

		spin_lock(&pages->lock);
		for (i = 0; count < target && i < ARRAY_SIZE(lists); i++) {
			if (list_empty(lists[i]))
				continue;

			list_for_each_entry(cp, lists[i], link) {
				struct dma_fence *f;
				struct page *page;
				bool idle;

				page = cp->page;
				if (!page)
					continue;

				idle = true;
				list_replace(&cp->link, &bookmark.link);
				f = i915_active_fence_get(&cp->active);
				spin_unlock(&pages->lock);

				if (f) {
					if (target == -1)  /* enforce cleanup */
						dma_fence_wait(f, false);

					idle = dma_fence_is_signaled(f);
					dma_fence_put(f);
				}

				if (idle) {
					release_clear_page(mem, page, order, NULL);
					__free_pages(page, order);
					count += BIT(order + PAGE_SHIFT);
				}

				spin_lock(&pages->lock);
				if (idle) {
					__list_del_entry(&bookmark.link);
					cp = &bookmark;
				} else {
					list_replace(&bookmark.link, &cp->link);
					if (count)
						target = 0;
				}

				if (count >= target)
					break;
			}
		}
		spin_unlock(&pages->lock);
	}

	return count;
}

static int shmem_create(struct shmem_work *wrk)
{
	const unsigned int limit = SZ_4M;
	const uint32_t max_segment = i915_sg_segment_size();
	struct drm_i915_gem_object *obj = wrk->obj;
	const unsigned long flags = shmem_create_mode(obj, i915_memclear_nocache(NULL, 0));
	struct intel_memory_region *mem = obj->mm.region.mem;
	struct i915_dma_engine *de = get_dma_engine(mem_cpu(mem));
	struct intel_context *blt = prefer_blt(obj, flags);
	struct sg_table *sgt = wrk->pages;
	struct scatterlist *need_blt = NULL;
	struct shmem_chunk *chunk = NULL;
	u64 remain = obj->base.size, dirty = 0;
	int min_order = min(MAX_ORDER - 1, get_order(MAX_PAGE));
	int last_node = mem_node(mem);
	struct i915_sw_fence fence;
	struct scatterlist *sg;
	LIST_HEAD(fences);
	unsigned long n;
	gfp_t gfp;
	int err;

	gfp = GFP_HIGHUSER | __GFP_RECLAIMABLE;
	gfp |= __GFP_RETRY_MAYFAIL | __GFP_NOWARN;
	gfp &= ~__GFP_RECLAIM;

	i915_sw_fence_init_onstack(&fence);

	n = 0;
	sg = sgt->sgl;
	obj->mm.page_sizes = 0;
	do {
		struct clear_page *cp;
		struct page *page;
		int order, nid;

restart:
		order = ilog2(min_t(u64, remain, max_segment)) - PAGE_SHIFT;
		order = min(order, min_order);
		do {
			page = get_clear_page(mem, order,
					      obj->maxnode,
					      get_obj_nodes(obj),
					      blt || gfp & __GFP_RECLAIM, flags,
					      &fences);
			if (page)
				break;

			page = alloc_pages_for_object(obj, &mem->interleave, gfp, order);
			if (page || gfp & __GFP_DIRECT_RECLAIM)
				break;

			/* Split higher order pages? */
			if (i915_gem_reap_clear_smem(mem, order + 1, remain)) {
				min_order = min(MAX_ORDER - 1, get_order(MAX_PAGE));
				goto restart;
			}

			if (order > get_order(SZ_2M))
				order = get_order(SZ_2M);
			else if (order > get_order(SZ_64K))
				order = get_order(SZ_64K);
			else
				order = 0;

			if (order <= PAGE_ALLOC_COSTLY_ORDER)
				gfp |= __GFP_KSWAPD_RECLAIM;
			if (order == 0) {
				/* XXX eviction does not consider node equivalence */
				intel_memory_region_evict(mem, NULL, SZ_2M, jiffies - HZ, PAGE_SIZE);
				gfp |= __GFP_DIRECT_RECLAIM;
			}

			min_order = min(min_order, order);
		} while (1);
		if (!page) {
			i915_sw_fence_set_error_once(&fence, -ENOMEM);
			ras_error(obj);
			break;
		}

		nid = page_to_nid(page);
		if (obj->maxnode && (nid >= obj->maxnode || !test_bit(nid, get_obj_nodes(obj))))
			ras_error(obj);

		if (!PagePrivate(page)) {
			dma_addr_t dma;

			cp = kmem_cache_alloc(slab_clear, GFP_KERNEL);
			if (!cp) {
				i915_sw_fence_set_error_once(&fence, -ENOMEM);
				break;
			}

			dma = dma_map_page_attrs(obj->base.dev->dev,
						 page, 0, BIT(order + PAGE_SHIFT),
						 DMA_BIDIRECTIONAL,
						 DMA_ATTR_SKIP_CPU_SYNC |
						 DMA_ATTR_NO_KERNEL_MAPPING |
						 DMA_ATTR_NO_WARN);
			if (dma_mapping_error(obj->base.dev->dev, dma)) {
				i915_sw_fence_set_error_once(&fence, -ENOMEM);
				kmem_cache_free(slab_clear, cp);
				break;
			}

			__i915_active_fence_init(&cp->active,
						 want_init_on_alloc(0) ? NULL : ERR_PTR(-ENODEV),
						 NULL);
			cp->page = page;
			cp->dma[0] = dma;

			if (de && nid != last_node) {
				de = get_dma_engine(__local_cpu(nid));
				last_node = nid;
			}

			dma = 0;
			if (de)
				dma = dma_map_page_attrs(de->dma->device->dev,
							 page, 0, BIT(order + PAGE_SHIFT),
							 DMA_FROM_DEVICE,
							 DMA_ATTR_SKIP_CPU_SYNC |
							 DMA_ATTR_NO_KERNEL_MAPPING |
							 DMA_ATTR_NO_WARN);
			cp->dma[1] = dma;
			cp->engine = de;

			page->private = (unsigned long)cp;
			GEM_BUG_ON(PagePrivate(page));
			SetPagePrivate(page);

			atomic_add(BIT(order), &to_shmem_private(mem)->clear_pages);
		}

		cp = to_clear_page(page);
		sg_set_page(sg, page, BIT(order + PAGE_SHIFT), 0);
		sg_dma_address(sg) = cp->dma[0];
		sg_dma_len(sg) = sg->length;
		obj->mm.page_sizes |= sg->length;

		if (flags && i915_active_fence_has_error(&cp->active)) {
			if (dirty >= limit) {
				chunk->end = n;
				GEM_BUG_ON(need_blt);
				shmem_queue(chunk, to_i915(obj->base.dev));
				chunk = NULL;
				dirty = 0;
			}

			if (!need_blt && chunk == NULL) {
				struct dma_fence *f = NULL;

				if (atomic_read(&fence.pending) > 1 && 2 * remain > limit) {
					if (cp->dma[1] && !cp->engine->zero_dma) {
						f = dma_clear(cp->engine, cp->dma[1], sg->length);
						if (f) {
							__i915_active_fence_set(&cp->active, f);
							fence_chain(&wrk->error->base.rq, f, &cp->cb, NULL);
							dma_fence_put(f);
						}
					}

					if (!f && blt) {
						need_blt = sg;
						f = ERR_PTR(-1);
					}
				}

				if (!f) {
					chunk = kmap(page);
					if (!chunk) {
						i915_sw_fence_set_error_once(&fence, -ENOMEM);
						break;
					}

					i915_sw_fence_await(&fence);
					chunk->sg = sg;
					chunk->fence = &fence;
					chunk->idx = n;
					chunk->flags = flags;
					INIT_WORK(&chunk->work, shmem_chunk);
				}
			}

			if (chunk)
				dirty += sg->length;
		}
		n++;

		GEM_BUG_ON(sg->length > remain);
		remain -= sg->length;
		if (!remain)
			break;

		if (sg_is_last(sg)) {
			struct scatterlist *chain;

			chain = (void *)__get_free_page(I915_GFP_ALLOW_FAIL);
			if (!chain) {
				i915_sw_fence_set_error_once(&fence, -ENOMEM);
				break;
			}
			sg_init_table(chain, SG_MAX_SINGLE_ALLOC);

			sg_unmark_end(memcpy(chain, sg, sizeof(*sg)));
			__sg_chain(sg, chain);
			sgt->orig_nents += I915_MAX_CHAIN_ALLOC;

			if (chunk && chunk->sg == sg)
				chunk->sg = chain;
			if (need_blt == sg)
				need_blt = chain;

			GEM_BUG_ON(sg_chain_ptr(sg) != chain);
			sg = chain;

			cond_resched();
		}
		GEM_BUG_ON(sg_is_last(sg));
		sg++;
	} while (1);
	i915_sw_fence_commit(&fence);

	sg_mark_end(sg);
	sgt->nents = n;
	GEM_BUG_ON(sgt->nents > sgt->orig_nents);

	if (!READ_ONCE(fence.error) && need_blt) {
		struct i915_request *rq = NULL;

		err = i915_gem_clear_smem(blt, need_blt, 0, &rq);
		if (rq) {
			if (!err)
				error_inject(wrk->error, &rq->fence);
			i915_sw_fence_complete(&rq->submit);
			i915_request_put(rq);
		}
		if (err)
			i915_sw_fence_set_error_once(&fence, err);

		set_bit(INTEL_MEMORY_CLEAR_FREE, &mem->flags);
	}

	/* Leaving the last chunk for ourselves */
	if (chunk) {
		chunk->end = n;
		GEM_BUG_ON(need_blt);
		shmem_chunk(&chunk->work);
	}

	i915_sw_fence_wait(&fence);
	err = fence.error;

	i915_sw_fence_fini(&fence);
	if (err)
		goto err;

	add_clear_fences(&wrk->error->base.rq, &fences);

	//i915_request_set_priority(&wrk->error->base.rq, I915_PRIORITY_MAX);
	return 0;

err:
	keep_sg(mem, sgt->sgl, NULL);
	sg_mark_end(sgt->sgl);
	sgt->nents = 0;
	return err;
}

static int shmem_swapin(struct shmem_work *wrk)
{
	struct drm_i915_gem_object *obj = wrk->obj;
	const unsigned int num_pages = obj->base.size >> PAGE_SHIFT;
	const unsigned long flags = shmem_create_mode(obj, false);
	struct address_space *mapping = obj->base.filp->f_mapping;
	struct sg_table *sgt = wrk->pages;
	struct shmem_chunk *chunk = NULL;
	struct i915_sw_fence fence;
	struct scatterlist *sg;
	unsigned long spread;
	unsigned long n;
	int err;

	spread = div_u64(obj->base.size, to_i915(obj->base.dev)->mm.sched->num_cpus + 1);
	spread = max_t(unsigned long, roundup_pow_of_two(spread), SZ_2M);

	err = 0;
	i915_sw_fence_init_onstack(&fence);
	mapping_set_unevictable(mapping);
	for (n = 0, sg = sgt->sgl; n + SG_MAX_SINGLE_ALLOC < num_pages;) {
		struct scatterlist *chain;

		if (chunk == NULL) {
			chunk = memset(sg, 0, sizeof(*chunk));

			i915_sw_fence_await(&fence);
			chunk->sg = sg;
			chunk->fence = &fence;
			chunk->mem = obj->mm.region.mem;
			chunk->mapping = mapping;
			chunk->policy = wrk->policy;
			chunk->idx = n;
			chunk->flags = flags;
			INIT_WORK(&chunk->work, shmem_chunk);
		}

		sg += I915_MAX_CHAIN_ALLOC;
		GEM_BUG_ON(!sg_is_last(sg));

		chain = (void *)__get_free_page(I915_GFP_ALLOW_FAIL);
		if (!chain) {
			i915_sw_fence_set_error_once(&fence, -ENOMEM);
			break;
		}
		sg_init_table(chain, SG_MAX_SINGLE_ALLOC);

		__sg_chain(sg, chain);
		sgt->orig_nents += I915_MAX_CHAIN_ALLOC;
		sg = chain;

		n += I915_MAX_CHAIN_ALLOC;
		if (n - chunk->idx > spread >> PAGE_SHIFT) {
			chunk->end = n;
			shmem_queue(chunk, to_i915(obj->base.dev));
			chunk = NULL;
		}

		cond_resched();
	}
	i915_sw_fence_commit(&fence);

	/* Leaving the last chunk for ourselves */
	if (chunk) {
		chunk->end = num_pages;
		shmem_chunk(&chunk->work);
	} else {
		err = __shmem_chunk(sg, obj->mm.region.mem,
				    mapping, wrk->policy,
				    n, num_pages, flags,
				    &fence.error);
		i915_sw_fence_set_error_once(&fence, err);
	}

	if (n) {
		i915_sw_fence_wait(&fence);
		err = fence.error;
	}

	i915_sw_fence_fini(&fence);
	if (err)
		goto err;

	sg_mark_end(&sg[num_pages - n - 1]);
	obj->mm.page_sizes =
		i915_sg_compact(sgt, i915_gem_sg_segment_size(obj));

	err = i915_gem_gtt_prepare_pages(obj, sgt);
	if (err)
		goto err;

	return 0;

err:
	mapping_clear_unevictable(mapping);
	for (sg = sgt->sgl; sg; sg = __sg_next(sg)) {
		unsigned long pfn, end;
		struct page *page;

		page = sg_page(sg);
		if (!page)
			continue;

		pfn = 0;
		end = sg->length >> PAGE_SHIFT;
		do {
			put_page(nth_page(page, pfn));
		} while (++pfn < end);

		sg_set_page(sg, NULL, 0, 0);
	}
	sg_mark_end(sgt->sgl);
	sgt->nents = 0;

	/*
	 * shmemfs first checks if there is enough memory to allocate the page
	 * and reports ENOSPC should there be insufficient, along with the usual
	 * ENOMEM for a genuine allocation failure.
	 *
	 * We use ENOSPC in our driver to mean that we have run out of aperture
	 * space and so want to translate the error from shmemfs back to our
	 * usual understanding of ENOMEM.
	 */
	if (err == -ENOSPC)
		err = -ENOMEM;

	return err;
}

static int shmem_work(struct dma_fence_work *base)
{
	struct shmem_work *wrk = container_of(base, typeof(*wrk), base);
	int err;

	if (!wrk->obj->base.filp)
		err = shmem_create(wrk);
	else
		err = shmem_swapin(wrk);
	if (err && test_bit(DMA_FENCE_WORK_IMM, &wrk->base.rq.fence.flags))
		err = -ERESTARTSYS; /* retry from kworker */

	return err;
}

static void shmem_work_release(struct dma_fence_work *base)
{
	struct shmem_work *wrk = container_of(base, typeof(*wrk), base);

	i915_gem_object_put(wrk->obj);
}

static const struct dma_fence_work_ops shmem_ops = {
	.name = "shmem",
	.work = shmem_work,
	.release = shmem_work_release,
};

static int shmem_error(struct dma_fence_work *base)
{
	struct shmem_error *e = container_of(base, typeof(*e), base);
	struct shmem_work *wrk = e->work;
	unsigned long flags;

	if (!e->base.rq.submit.error)
		return 0;

	if (!e->work->pages->nents)
		return e->base.rq.submit.error;

	if (wrk->obj->base.filp)
		return e->base.rq.submit.error;

	flags = shmem_create_mode(wrk->obj, i915_memclear_nocache(NULL, 0));
	if (!flags)
		return 0;

	if (test_bit(DMA_FENCE_WORK_IMM, &base->rq.fence.flags))
		return -ERESTARTSYS; /* only run from kworker */

	/* Ignore any blt errors and redo the work */
	return __shmem_chunk(e->work->pages->sgl,
			     NULL, NULL, NULL,
			     0, e->work->pages->nents,
			     flags, NULL);
}

static const struct dma_fence_work_ops error_ops = {
	.name = "shmem-error",
	.work = shmem_error,
	.no_error_propagation = true,
};

static struct shmem_error *
error_create(struct i915_sched_engine *se, struct shmem_work *wrk)
{
	struct shmem_error *e;

	e = kmalloc(sizeof(*e), I915_GFP_ALLOW_FAIL);
	if (!e)
		return e;

	dma_fence_work_init(&e->base, &error_ops, se);
	__set_bit(DMA_FENCE_WORK_IMM, &e->base.rq.fence.flags);
	e->work = wrk;

	return e;
}

static int shmem_get_pages(struct drm_i915_gem_object *obj)
{
	struct intel_memory_region *mem = obj->mm.region.mem;
	struct shmem_work *wrk;
	unsigned int num_pages;
	struct sg_table *st;
	int err;

	if (!safe_conversion(&num_pages, obj->base.size >> PAGE_SHIFT))
		return -E2BIG;

	/*
	 * If there's no chance of allocating enough pages for the whole
	 * object, bail early.
	 */
	if (num_pages > totalram_pages())
		return -E2BIG;

	st = kmalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	err = sg_alloc_table(st,
			     min_t(unsigned int, num_pages, SG_MAX_SINGLE_ALLOC),
			     I915_GFP_ALLOW_FAIL);
	if (err)
		goto err_free;

	wrk = kmalloc(sizeof(*wrk), GFP_KERNEL);
	if (!wrk) {
		err = -ENOMEM;
		goto err_sg;
	}
	dma_fence_work_init(&wrk->base, &shmem_ops,
			    to_i915(obj->base.dev)->mm.sched);
	wrk->obj = i915_gem_object_get(obj);
	wrk->pages = st;
	wrk->policy = get_mempolicy(current);
	wrk->error = error_create(to_i915(obj->base.dev)->mm.sched, wrk);
	if (!wrk->error) {
		kfree(wrk);
		err = -ENOMEM;
		goto err_sg;
	}

	/* Install a watcher to hide any blt errors */
	i915_gem_object_migrate_prepare(obj, &wrk->error->base.rq.fence);
	dma_fence_work_chain(&wrk->error->base, &wrk->base.rq.fence);
	dma_fence_work_commit(&wrk->error->base);

	atomic64_sub(obj->base.size, &mem->avail);

	__i915_gem_object_set_pages(obj, st, PAGE_SIZE); /* placeholder */

	dma_fence_work_commit_imm_if(&wrk->base,
				     obj->flags & I915_BO_SYNC_HINT ||
				     obj->base.size <= SZ_64K ||
				     !obj->base.filp);
	return 0;

err_sg:
	sg_free_table(st);
err_free:
	kfree(st);
	return err;
}

static void
shmem_truncate(struct drm_i915_gem_object *obj)
{
	/*
	 * Our goal here is to return as much of the memory as
	 * is possible back to the system as we are called from OOM.
	 * To do this we must instruct the shmfs to drop all of its
	 * backing pages, *now*.
	 */
	if (obj->base.filp)
		shmem_truncate_range(file_inode(obj->base.filp), 0, (loff_t)-1);
}

static void check_release_pagevec(struct pagevec *pvec)
{
	check_move_unevictable_pages(pvec);
	__pagevec_release(pvec);
	cond_resched();
}

static void page_release(struct page *page, struct pagevec *pvec)
{
	if (!pagevec_add(pvec, page))
		check_release_pagevec(pvec);
}

static bool need_swap(const struct drm_i915_gem_object *obj)
{
	GEM_BUG_ON(obj->base.filp && mapping_mapped(obj->base.filp->f_mapping));

	if (i915_gem_object_migrate_has_error(obj))
		return false;

	if (kref_read(&obj->base.refcount) == 0)
		return false;

	if (i915_gem_object_is_purgeable(obj))
		return false;

	if (obj->flags & I915_BO_ALLOC_USER && !i915_gem_object_inuse(obj))
		return false;

	return true;
}

#ifdef BPM_DELETE_FROM_PAGE_CACHE_NOT_PRESENT
#define i915_delete_from_page_cache delete_from_page_cache
#else
/* inlined delete_from_page_cache() to aide porting to different kernels */
static void i915_delete_from_page_cache(struct page *page)
{
	struct address_space *mapping = page_mapping(page);
	XA_STATE(xas, &mapping->i_pages, page->index);

	GEM_BUG_ON(!PageLocked(page));
	xas_lock_irq(&xas);

#ifdef BPM_INC_DEC_LRUVEC_PAGE_STATE_PRESENT
{
	struct folio *old = page_folio(page);
	__lruvec_stat_mod_folio(old, NR_FILE_PAGES, -1);
	__lruvec_stat_mod_folio(old, NR_SHMEM, -1);
}
#else
	__dec_lruvec_page_state(page, NR_FILE_PAGES);
	__dec_lruvec_page_state(page, NR_SHMEM);
#endif

	xas_set_order(&xas, page->index, 0);
	xas_store(&xas, NULL);
	xas_init_marks(&xas);

	/* Leave page->index set: truncation lookup relies upon it */
	page->mapping = NULL;
	mapping->nrpages--;

	xas_unlock_irq(&xas);
	put_page(page);
}
#endif

#ifdef BPM_ADD_PAGE_CACHE_LOCKED_NOT_PRESENT
#define i915_add_to_page_cache_locked add_to_page_cache_locked
#else
/* inlined add_to_page_cache_locked() to aide porting to different kernels */
static int i915_add_to_page_cache_locked(struct page *page,
					 struct address_space *mapping,
					 pgoff_t offset,
					 gfp_t gfp)
{
	XA_STATE(xas, &mapping->i_pages, offset);
	int error;

	get_page(page);
	page->mapping = mapping;
	page->index = offset;

	do {
		unsigned int order = xa_get_order(xas.xa, xas.xa_index);
		void *entry, *old = NULL;

		if (order > thp_order(page))
			xas_split_alloc(&xas, xa_load(xas.xa, xas.xa_index),
					order, gfp);
		xas_lock_irq(&xas);
		xas_for_each_conflict(&xas, entry) {
			old = entry;
			if (!xa_is_value(entry)) {
				xas_set_err(&xas, -EEXIST);
				goto unlock;
			}
		}

		if (old) {
			/* entry may have been split before we acquired lock */
			order = xa_get_order(xas.xa, xas.xa_index);
			if (order > thp_order(page)) {
				xas_split(&xas, old, order);
				xas_reset(&xas);
			}
		}

		xas_store(&xas, page);
		if (xas_error(&xas))
			goto unlock;

		mapping->nrpages++;

#ifdef BPM_INC_DEC_LRUVEC_PAGE_STATE_PRESENT
{
		struct folio *fobj = page_folio(page);
		__lruvec_stat_mod_folio(fobj, NR_FILE_PAGES, 1);
}
#else
		__inc_lruvec_page_state(page, NR_FILE_PAGES);
#endif
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, gfp));

	if (xas_error(&xas)) {
		error = xas_error(&xas);
		goto error;
	}

	return 0;
error:
	/* Leave page->index set: truncation relies upon it */
	page->mapping = NULL;
	put_page(page);
	return error;
}
#endif

static int __create_shmem(struct drm_i915_gem_object *obj)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	resource_size_t size = obj->base.size;
	unsigned long flags = VM_NORESERVE;
	struct address_space *mapping;
	struct file *filp;
	gfp_t mask;

	if (i915->mm.gemfs)
		filp = shmem_file_setup_with_mnt(i915->mm.gemfs, "i915", size,
						 flags);
	else
		filp = shmem_file_setup("i915", size, flags);
	if (IS_ERR(filp))
		return PTR_ERR(filp);

	mask = GFP_HIGHUSER | __GFP_RECLAIMABLE;
	mask |= __GFP_RETRY_MAYFAIL | __GFP_NOWARN;

	mapping = filp->f_mapping;
	mapping_set_gfp_mask(mapping, mask);
	GEM_BUG_ON(!(mapping_gfp_mask(mapping) & __GFP_RECLAIM));

	i_size_write(filp->f_inode, size);
	obj->base.filp = filp;
	return 0;
}

static int
shmem_put_pages(struct drm_i915_gem_object *obj, struct sg_table *pages)
{
	struct intel_memory_region *mem = obj->mm.region.mem;
	bool clflush = shmem_create_mode(obj, false) & SHMEM_CLFLUSH;
	bool do_swap = need_swap(obj);
	struct pagevec pvec;

	i915_gem_object_migrate_finish(obj);
	if (!pages->nents)
		goto empty;

	pagevec_init(&pvec);
	if (obj->base.filp) {
		struct address_space *mapping = obj->base.filp->f_mapping;
		struct sgt_iter sgt_iter;
		struct page *page;

		i915_gem_gtt_finish_pages(obj, pages);
		mapping_clear_unevictable(mapping);

		for_each_sgt_page(page, sgt_iter, pages) {
			GEM_BUG_ON(!page);

			if (clflush) {
				void *ptr = kmap_atomic(page);
				clflush_cache_range(ptr, PAGE_SIZE);
				kunmap_atomic(ptr);
			}

			if (do_swap) {
				set_page_dirty(page);
				mark_page_accessed(page);
			} else {
				cancel_dirty_page(page);
			}

			page_release(page, &pvec);
		}
	} else if (do_swap) { /* instantiate shmemfs backing store for swap */
		struct address_space *mapping;
		struct scatterlist *sg;
		struct inode *inode;
		long idx = 0;
		int err;

		err = __create_shmem(obj);
		if (err)
			return err;

		inode = file_inode(obj->base.filp);
		GEM_BUG_ON(!inode);
		mapping = obj->base.filp->f_mapping;

		for (sg = pages->sgl; sg; sg = __sg_next(sg)) {
			int order = get_order(sg->length);
			struct page *page = sg_page(sg);
			int i;

			if (clflush) {
				void *ptr = kmap_atomic(page);
				clflush_cache_range(ptr, sg->length);
				kunmap_atomic(ptr);
			}

			if (PagePrivate(page)) {
				release_clear_page(mem, page, order, obj->mm.tlb);
				if (order)
					split_page(page, order);
			}

			GEM_BUG_ON(PagePrivate(page));
			for (i = 0; i < BIT(order); i++) {
				struct page *p = nth_page(page, i);

				lock_page(p);

				SetPageUptodate(p);
				set_page_dirty(p);
				mark_page_accessed(p);

				if (i915_add_to_page_cache_locked(p, mapping, idx, I915_GFP_ALLOW_FAIL)) {
					unlock_page(p);

					if (pagevec_count(&pvec))
						check_release_pagevec(&pvec);

					mapping_set_unevictable(mapping);

					while (idx--) {
						p = find_lock_page(mapping, idx);
						GEM_BUG_ON(!p);

						cancel_dirty_page(page);
						i915_delete_from_page_cache(p);
						unlock_page(p);
					}

					GEM_BUG_ON(mapping->nrpages);
					return -ENOMEM;
				}

				if (!PageLRU(p)) {
					__SetPageSwapBacked(p);
					lru_cache_add(p);
				}

#ifdef BPM_INC_DEC_LRUVEC_PAGE_STATE_PRESENT
{
				struct folio *fobj = page_folio(page);
				__lruvec_stat_mod_folio(fobj, NR_FILE_PAGES, 1);
}
#else
				inc_lruvec_page_state(p, NR_SHMEM);
#endif
				unlock_page(p);
				idx++;

				page_release(p, &pvec);
			}
		}

		SHMEM_I(inode)->alloced = mapping->nrpages;
		inode->i_blocks = mapping->nrpages * (PAGE_SIZE / 512);
	} else if (mem->gt->suspend || current->flags & PF_MEMALLOC) { /* inside the shrinker, reclaim immediately */
		struct scatterlist *sg;

		for (sg = pages->sgl; sg; sg = __sg_next(sg)) {
			int order = get_order(sg->length);
			struct page *page = sg_page(sg);

			release_clear_page(mem, page, order, obj->mm.tlb);
			__free_pages(page, order);
		}
	} else { /* device-local host pages, keep for future use */
		struct scatterlist *sg = pages->sgl;

		if (IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_SMEM_FREE) &&
		    obj->flags & I915_BO_ALLOC_USER) {
			for (; sg; sg = __sg_next(sg)) {
				struct clear_page *cp = to_clear_page(sg_page(sg));
				struct dma_fence *f;

				if (i915_active_fence_isset(&cp->active))
					continue;

				f = NULL;
				if (cp->dma[1])
					f = dma_clear(cp->engine, cp->dma[1], sg->length);
				if (!f)
					break;

				__i915_active_fence_set(&cp->active, f);
				dma_fence_put(f);
			}

			if (sg && test_bit(INTEL_MEMORY_CLEAR_FREE, &mem->flags)) {
				struct intel_gt *gt = mem->gt;
				intel_wakeref_t wf;

				with_intel_gt_pm_if_awake(gt, wf) {
					struct intel_context *ce;
					struct i915_request *rq;

					ce = i915_gem_get_free_smem_context(gt);
					if (unlikely(!ce))
						continue;

					if (intel_context_throttle(ce, 0))
						continue;

					rq = NULL;
					if (i915_gem_clear_smem(ce, sg, SPLIT_CLEARS, &rq) == 0)
						sg = NULL;
					if (rq) {
						dma_fence_enable_sw_signaling(&rq->fence);
						i915_sw_fence_complete(&rq->submit);
						i915_request_put(rq);
					}
				}
			}
		}

		for (; sg; sg = __sg_next(sg))
			RCU_INIT_POINTER(to_clear_page(sg_page(sg))->active.fence,
					 ERR_PTR(-ENODEV));

		keep_sg(mem, pages->sgl, obj);
	}
	if (pagevec_count(&pvec))
		check_release_pagevec(&pvec);

empty:
	atomic64_add(obj->base.size, &mem->avail);

	sg_free_table(pages);
	kfree(pages);
	return 0;
}

static void shmem_release(struct drm_i915_gem_object *obj)
{
	i915_gem_object_release_memory_region(obj);
	if (obj->base.filp)
		fput(obj->base.filp);
}

const struct drm_i915_gem_object_ops i915_gem_shmem_ops = {
	.name = "i915_gem_object_shmem",
	.flags = I915_GEM_OBJECT_HAS_STRUCT_PAGE,

	.get_pages = shmem_get_pages,
	.put_pages = shmem_put_pages,
	.truncate = shmem_truncate,

	.release = shmem_release,
};

/* See sum_zone_node_page_state() */
static unsigned long sum_node_pages(int nid, enum zone_stat_item item)
{
	struct zone *zones = NODE_DATA(nid)->node_zones;
	unsigned long count = 0;
	int i;

	for (i = 0; i < MAX_NR_ZONES; i++)
		count += zone_page_state(zones + i, item);

	return count;
}

static bool can_mpol_bind(struct drm_i915_gem_object *obj, resource_size_t sz)
{
	const unsigned long *nodes = get_obj_nodes(obj);
	unsigned long nr_free;
	unsigned long nid;

	if (bitmap_weight(nodes, obj->maxnode) > 1)
		return true;

	nid = find_first_bit(nodes, obj->maxnode);
	nr_free = sum_node_pages(nid, NR_FREE_PAGES);

	return (sz >> PAGE_SHIFT) <= nr_free;
}

static int shmem_object_init(struct intel_memory_region *mem,
			     struct drm_i915_gem_object *obj,
			     resource_size_t size,
			     unsigned int flags)
{
	struct drm_i915_private *i915 = mem->i915;
	unsigned int cache_level;

	/*
	 * If the user requests to use only a specific domain, check there
	 * is sufficient space up front. In return, we will try to keep
	 * the object resident during memory pressure.
	 */
	if (obj->mempol == I915_GEM_CREATE_MPOL_BIND &&
	    !can_mpol_bind(obj, size))
		return -ENOMEM;

	drm_gem_private_object_init(&i915->drm, &obj->base, size);
	i915_gem_object_init(obj, &i915_gem_shmem_ops, flags);

	/*
	 * Soft-pinned buffers need to be 1-way coherent from MTL onward
	 * because GPU is no longer snooping CPU cache by default. Make it
	 * default setting and let others to modify as needed later
	 */
	if (IS_DGFX(i915) || HAS_LLC(i915) || GRAPHICS_VER_FULL(i915) >= IP_VER(12, 70))
		/*
		 * On some devices, we can have the GPU use the LLC (the CPU
		 * cache) for about a 10% performance improvement
		 * compared to uncached.  Graphics requests other than
		 * display scanout are coherent with the CPU in
		 * accessing this cache.  This means in this mode we
		 * don't need to clflush on the CPU side, and on the
		 * GPU side we only need to flush internal caches to
		 * get data visible to the CPU.
		 *
		 * However, we maintain the display planes as UC, and so
		 * need to rebind when first used as such.
		 */
		cache_level = I915_CACHE_LLC;
	else
		cache_level = I915_CACHE_NONE;
	if (i915_run_as_guest())
		cache_level = I915_CACHE_NONE;

	i915_gem_object_set_cache_coherency(obj, cache_level);

	i915_gem_object_init_memory_region(obj, mem);

	return 0;
}

struct drm_i915_gem_object *
i915_gem_object_create_shmem(struct drm_i915_private *i915,
			     resource_size_t size)
{
	return i915_gem_object_create_region(i915->mm.regions[INTEL_REGION_SMEM],
					     size, 0);
}

/* Allocate a new GEM object and fill it with the supplied data */
struct drm_i915_gem_object *
i915_gem_object_create_shmem_from_data(struct drm_i915_private *dev_priv,
				       const void *data, resource_size_t size)
{
	struct drm_i915_gem_object *obj;
	resource_size_t offset;
	struct file *file;
	int err;

	obj = i915_gem_object_create_shmem(dev_priv, round_up(size, PAGE_SIZE));
	if (IS_ERR(obj))
		return obj;

	err = __create_shmem(obj);
	if (err)
		goto fail;

	file = obj->base.filp;
	offset = 0;
	do {
		unsigned int len = min_t(typeof(size), size, PAGE_SIZE);
		struct page *page;
		void *pgdata, *vaddr;

		err = pagecache_write_begin(file, file->f_mapping,
					    offset, len, 0,
					    &page, &pgdata);
		if (err < 0)
			goto fail;

		vaddr = kmap(page);
		memcpy(vaddr, data, len);
		kunmap(page);

		err = pagecache_write_end(file, file->f_mapping,
					  offset, len, len,
					  page, pgdata);
		if (err < 0)
			goto fail;

		size -= len;
		data += len;
		offset += len;
	} while (size);

	return obj;

fail:
	i915_gem_object_put(obj);
	return ERR_PTR(err);
}

static struct page *
get_dirty_page(struct intel_memory_region *mem, int *order)
{
	struct shmem_private *mp = to_shmem_private(mem);
	struct page *page = NULL;
	int nid = mem_node(mem);
	struct clear_page *cp;
	dma_addr_t dma;
	int nr_free;

	for (*order = 0; *order < ARRAY_SIZE(mp->clear); ++*order) {
		struct clear_pages *pages = &mp->clear[*order];

		if (list_empty(&pages->dirty))
			continue;

		spin_lock(&pages->lock);
		list_for_each_entry(cp, &pages->dirty, link) {
			if (!cp->page)
				continue;

			list_del(&cp->link);
			page = cp->page;
			break;
		}
		spin_unlock(&pages->lock);
		if (page)
			return page;
	}

	if (atomic_read(&mp->clear_pages) > mp->max_clear_pages)
		return NULL;

	nr_free = sum_node_pages(nid, NR_FREE_PAGES);
	if (nr_free < 128 * MAX_PAGE >> PAGE_SHIFT)
		return NULL;

	*order = get_order(MAX_PAGE);
	page = alloc_pages_node(nid,
				GFP_NOWAIT | __GFP_THISNODE | __GFP_NORETRY | __GFP_NOWARN,
				*order);
	if (!page)
		return NULL;

	cp = kmem_cache_alloc(slab_clear, GFP_KERNEL);
	if (!cp) {
		__free_pages(page, *order);
		return NULL;
	}

	dma = dma_map_page_attrs(mem->i915->drm.dev,
				 page, 0, MAX_PAGE,
				 DMA_BIDIRECTIONAL,
				 DMA_ATTR_SKIP_CPU_SYNC |
				 DMA_ATTR_NO_KERNEL_MAPPING |
				 DMA_ATTR_NO_WARN);
	if (!dma) {
		kmem_cache_free(slab_clear, cp);
		__free_pages(page, *order);
		return NULL;
	}

	__i915_active_fence_init(&cp->active,
				 want_init_on_alloc(0) ? NULL : ERR_PTR(-ENODEV),
				 NULL);
	cp->page = page;
	cp->dma[0] = dma;

	dma = 0;
	cp->engine = get_dma_engine(mem_cpu(mem));
	if (cp->engine)
		dma = dma_map_page_attrs(cp->engine->dma->device->dev,
					 page, 0, MAX_PAGE,
					 DMA_FROM_DEVICE,
					 DMA_ATTR_SKIP_CPU_SYNC |
					 DMA_ATTR_NO_KERNEL_MAPPING |
					 DMA_ATTR_NO_WARN);
	cp->dma[1] = dma;

	page->private = (unsigned long)cp;
	GEM_BUG_ON(PagePrivate(page));
	SetPagePrivate(page);

	atomic_add(BIT(*order), &mp->clear_pages);

	return page;
}

bool i915_gem_shmem_park(struct intel_memory_region *mem)
{
	struct scatterlist *sg, *end = NULL;
	struct i915_request *rq = NULL;
	struct intel_migrate_window *w;
	struct intel_context *ce;
	struct sg_table sgt;
	struct page *page;
	int order;

	if (!IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_SMEM_IDLE))
		return false;

	ce = i915_gem_get_free_smem_context(mem->gt);
	if (!ce || !ce->private)
		return false;

	page = get_dirty_page(mem, &order);
	if (!page)
		return false;

	__intel_wakeref_defer_park(&mem->gt->wakeref);
	mutex_unlock(&mem->gt->wakeref.mutex);

	w = ce->private;
	if (sg_alloc_table(&sgt, w->swap_chunk >> ilog2(MAX_PAGE), GFP_NOWAIT | __GFP_NOWARN)) {
		release_clear_page(mem, page, order, NULL);
		__free_pages(page, order);
		goto out;
	}

	sg = sgt.sgl;
	do {
		struct clear_page *cp = to_clear_page(page);

		if (!i915_active_fence_has_error(&cp->active)) {
			add_clear_page(mem, cp, order);
			continue;
		}

		sg_set_page(sg, page, BIT(order + PAGE_SHIFT), 0);
		sg_dma_address(sg) = cp->dma[0];
		sg_dma_len(sg) = sg->length;
		end = sg;

		sg = __sg_next(sg);
		if (!sg)
			break;

		if (atomic_read(&mem->gt->wakeref.count) > 1)
			break;
	} while ((page = get_dirty_page(mem, &order)));

	if (end) {
		sg_mark_end(end);
		i915_gem_clear_smem(ce, sgt.sgl, 0, &rq);
		if (rq) {
			dma_fence_enable_sw_signaling(&rq->fence); /* fast retire */
			i915_sw_fence_complete(&rq->submit);
			i915_request_put(rq);
		}
		keep_sg(mem, sgt.sgl, NULL);
	}

	sg_free_table(&sgt);

out:
	mutex_lock(&mem->gt->wakeref.mutex);
	return __intel_wakeref_resume_park(&mem->gt->wakeref);
}

static void free_errors(struct ras_errors *e)
{
	int n;

	if (!e)
		return;

	for (n = 0; n < e->max; n++)
		kfree(e->errors[n].attr.attr.attr.name);

	kfree(e);
}

static int init_shmem(struct intel_memory_region *mem)
{
	struct shmem_private *mp;
	int n;

	i915_gemfs_init(mem->i915);
	intel_memory_region_set_name(mem, "system");

	mp = kzalloc(sizeof(struct shmem_private), GFP_KERNEL);
	if (!mp)
		return -ENOMEM;

	for (n = 0; n < ARRAY_SIZE(mp->clear); n++) {
		spin_lock_init(&mp->clear[n].lock);
		INIT_LIST_HEAD(&mp->clear[n].clean);
		INIT_LIST_HEAD(&mp->clear[n].dirty);
	}

	if (!IS_SRIOV_VF(mem->i915)) {
		struct i915_dma_engine *dma;

		dma = get_dma_engine(mem_cpu(mem));
		if (dma)
			dev_info(mem->i915->drm.dev,
				 "Using dma engine '%s' for clearing system pages\n",
				 dma_chan_name(dma->dma));

		n = mem_node(mem);
		if (n != NUMA_NO_NODE)
			mp->max_clear_pages = node_present_pages(n) >> 3;
	}

	mem->region_private = mp;
	return 0; /* We have fallback to the kernel mnt if gemfs init failed. */
}

static void release_shmem(struct intel_memory_region *mem)
{
	struct shmem_private *mp;

	i915_gem_reap_clear_smem(mem, 0, -1ul);

	mp = to_shmem_private(mem);
	if (mp) {
		free_errors(mp->errors);
		kfree(mp);
	}

	i915_gemfs_fini(mem->i915);
}

static void show_shmem(struct intel_memory_region *mem, struct drm_printer *p)
{
	struct i915_dma_engine *de;
	struct shmem_private *mp;
	char bytes[16];
	char buf[256];
	int order;

	mp = to_shmem_private(mem);
	if (!mp)
		return;

	drm_printf(p, "  clear:\n");
	de = lookup_dma_engine(mem_cpu(mem));
	if (de) {
		drm_printf(p, "    using: %s (%s) [%s]\n",
			  dma_chan_name(de->dma),
			  de->zero_dma ? "memcpy" : "memset",
			  dev_name(de->dma->device->dev));
	}

	order = atomic_read(&mp->clear_pages);
	string_get_size(order, 4096, STRING_UNITS_2, bytes, sizeof(bytes));
	drm_printf(p, "    total: %d pages [%s]\n", order, bytes);

	if (mp->max_clear_pages) {
		string_get_size(mp->max_clear_pages, 4096, STRING_UNITS_2, bytes, sizeof(bytes));
		drm_printf(p, "    limit: %d pages [%s]\n", mp->max_clear_pages, bytes);
	}

	for (order = 0; order < ARRAY_SIZE(mp->clear); order++) {
		struct clear_pages *pages = &mp->clear[order];
		struct list_head *lists[] = {
			&mp->clear[order].clean,
			&mp->clear[order].dirty
		};
		const long sz = BIT(PAGE_SHIFT + order);
		size_t clean = 0, dirty = 0, active = 0;
		unsigned long count = 0;
		struct clear_page *cp;
		int i;

		spin_lock(&pages->lock);
		for (i = 0; i < ARRAY_SIZE(lists); i++) {
			list_for_each_entry(cp, lists[i], link) {
				if (i915_active_fence_isset(&cp->active))
					active += sz;
				else if (i915_active_fence_has_error(&cp->active))
					dirty += sz;
				else
					clean += sz;
				count++;
			}
		}
		spin_unlock(&pages->lock);

		if (!count)
			continue;

		i = 0;
		buf[0] = '\0';
		if (clean) {
			string_get_size(clean, 1, STRING_UNITS_2, bytes, sizeof(bytes));
			i += snprintf(buf + i, sizeof(buf) - i, ", clean: %s", bytes);
		}
		if (active) {
			string_get_size(active, 1, STRING_UNITS_2, bytes, sizeof(bytes));
			i += snprintf(buf + i, sizeof(buf) - i, ", active: %s", bytes);
		}
		if (dirty) {
			string_get_size(dirty, 1, STRING_UNITS_2, bytes, sizeof(bytes));
			i += snprintf(buf + i, sizeof(buf) - i, ", dirty: %s", bytes);
		}

		drm_printf(p, "  - [%d]: { count:%lu%s }\n",
			   PAGE_SHIFT + order, count, buf);
	}
}

static const struct intel_memory_region_ops shmem_region_ops = {
	.init = init_shmem,
	.show = show_shmem,
	.release = release_shmem,
	.init_object = shmem_object_init,
};

static u64 total_pages(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;
	int nid;

	nid = dev_to_node(i915->drm.dev);
	if (nid != NUMA_NO_NODE) {
		dev_info(i915->drm.dev,
			 "Attaching to %luMiB of system memory on node %d\n",
			 node_present_pages(nid) >> (20 - PAGE_SHIFT),
			 nid);
	}

	return (u64)totalram_pages() << PAGE_SHIFT;
}

struct intel_memory_region *i915_gem_shmem_setup(struct intel_gt *gt,
						 u16 type, u16 instance)
{
	return intel_memory_region_create(gt, 0,
					  total_pages(gt),
					  PAGE_SIZE, 0, 0,
					  type, instance,
					  &shmem_region_ops);
}

bool i915_gem_object_is_shmem(const struct drm_i915_gem_object *obj)
{
	return obj->ops == &i915_gem_shmem_ops;
}

bool i915_gem_shmem_register_sysfs(struct drm_i915_private *i915,
				   struct kobject *kobj)
{
	struct ras_errors *errors;
	unsigned int max, n;

	if (!IS_ENABLED(CONFIG_NUMA))
		return true;

	max = num_possible_nodes();
	errors = kzalloc(struct_size(errors, errors, max), GFP_KERNEL);
	if (!errors)
		return false;

	errors->max = max;
	for (n = 0; n < max; n++) {
		struct ras_error *e = &errors->errors[n];

		sysfs_attr_init(&e->attr.attr.attr);

		e->attr.attr.attr.name =
			kasprintf(GFP_KERNEL, "numa%04d_allocation", n);
		if (!e->attr.attr.attr.name)
			break;

		e->attr.attr.attr.mode = 0444;
		e->attr.attr.show = device_show_ulong;
		e->attr.var = &e->count;

		if (sysfs_create_file(kobj, &e->attr.attr.attr))
			break;
	}

	to_shmem_private(i915->mm.regions[INTEL_REGION_SMEM])->errors = errors;
	return true;
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftests/i915_gem_shmem.c"
#endif

static void cleanup_dma_engines(void)
{
	struct i915_dma_engine *de, *n;

	rbtree_postorder_for_each_entry_safe(de, n, &i915_dma_engines, node) {
		if (de->zero_dma)
			dma_unmap_page_attrs(de->dma->device->dev,
					     de->zero_dma, MAX_PAGE,
					     DMA_TO_DEVICE,
					     DMA_ATTR_SKIP_CPU_SYNC);
		if (de->zero)
			__free_pages(de->zero, get_order(MAX_PAGE));
		dma_release_channel(de->dma);
		kfree(de);
	}
}

void i915_gem_shmem_module_exit(void)
{
	cleanup_dma_engines();
	kmem_cache_destroy(slab_clear);
}

int __init i915_gem_shmem_module_init(void)
{
	slab_clear = KMEM_CACHE(clear_page, 0);
	if (!slab_clear)
		return -ENOMEM;

	return 0;
}
