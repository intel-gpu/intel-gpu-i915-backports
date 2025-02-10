/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2014-2016 Intel Corporation
 */

#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/numa.h>
#include <linux/pagevec.h>
#include <linux/shmem_fs.h>
#include <linux/swap.h>

#include <asm-generic/getorder.h>

#include "gt/intel_context.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_clock_utils.h"

#include "dma_tx.h"
#include "i915_drv.h"
#include "i915_gem_object.h"
#include "i915_gem_region.h"
#include "i915_gem_shmem.h"
#include "i915_gemfs.h"
#include "i915_memcpy.h"
#include "i915_scatterlist.h"
#include "i915_sw_fence_work.h"
#include "i915_tbb.h"
#include "i915_trace.h"

#define DMA_MAX_CLEAR SZ_2M
#define DMA_MAX_ORDER (ilog2(DMA_MAX_CLEAR) - PAGE_SHIFT)
static_assert(DMA_MAX_ORDER < MAX_ORDER);

static struct kmem_cache *slab_clear;
static struct kmem_cache *slab_dma;

static struct shmem_dma *shmem_dma_map(struct device *dev, struct page *page, int order, int dir)
{
	struct shmem_dma *map;

	map = kmem_cache_alloc(slab_dma, GFP_KERNEL | __GFP_NOWARN);
	if (unlikely(!map))
		return NULL;

	kref_init(&map->kref);
	map->dev = dev;
	map->dir = dir;
	map->size = BIT(order + PAGE_SHIFT);
	map->dma = dma_map_page_attrs(dev, page, 0, map->size, dir,
				      DMA_ATTR_SKIP_CPU_SYNC |
				      DMA_ATTR_NO_KERNEL_MAPPING |
				      DMA_ATTR_NO_WARN);
	if (dma_mapping_error(dev, map->dma)) {
		kmem_cache_free(slab_dma, map);
		return NULL;
	}

	return map;
}

static struct shmem_dma *shmem_dma_get(struct shmem_dma *map)
{
	kref_get(&map->kref);
	return map;
}

static void shmem_dma_release(struct kref *kref)
{
	struct shmem_dma *map = container_of(kref, typeof(*map), kref);

	dma_unmap_page_attrs(map->dev, map->dma, map->size, map->dir,
			     DMA_ATTR_SKIP_CPU_SYNC);
	kmem_cache_free(slab_dma, map);
}

static void shmem_dma_put(struct shmem_dma *map)
{
	kref_put(&map->kref, shmem_dma_release);
}

static inline struct shmem_private {
	struct clear_pages {
		spinlock_t lock;
		struct list_head clean;
		struct list_head dirty;
	} clear[DMA_MAX_ORDER + 1];
	unsigned long low_clear_pages;
	unsigned long high_clear_pages;
	atomic_long_t clear_pages;
	atomic_t clear_count;
	bool shrink;

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

static void set_fence_or_error(struct i915_active_fence *ref, struct dma_fence *f)
{
	if (__i915_active_fence_set(ref, f) && f->error)
		RCU_INIT_POINTER(ref->fence, ERR_PTR(f->error));
}

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
						     get_order(DMA_MAX_CLEAR));
			if (!new->zero)
				goto err_engine;

			new->zero_dma = dma_map_page(chan->device->dev,
						     new->zero, 0, DMA_MAX_CLEAR,
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
				     new->zero_dma, DMA_MAX_CLEAR,
				     DMA_TO_DEVICE,
				     DMA_ATTR_SKIP_CPU_SYNC);
err_page:
	if (new->zero)
		__free_pages(new->zero, get_order(DMA_MAX_CLEAR));
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

struct shmem_error {
	struct dma_fence_work base;
	struct i915_sw_dma_fence_cb cb;
	struct scatterlist *pages;
	unsigned long flags;
};

static void fence_chain(struct i915_request *rq,
			struct dma_fence *f,
			struct i915_sw_dma_fence_cb *cb)
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

	i915_sched_node_add_dependency(&rq->sched,
				       &to_request(f)->sched,
				       0);
}

static void error_inject(struct shmem_error *e, struct dma_fence *f)
{
	fence_chain(&e->base.rq, f, &e->cb);
}

static void add_clear_fences(struct i915_request *rq, struct scatterlist *sg, struct scatterlist *end)
{
	for (; sg != end; sg = __sg_next(sg)) {
		struct clear_page *cp = to_clear_page(sg_page(sg));
		struct dma_fence *f;

		f = i915_active_fence_get(&cp->active);
		if (f) {
			fence_chain(rq, f, &cp->cb);
			dma_fence_put(f);
		}
	}
}

struct shmem_work {
	struct dma_fence_work base;
	struct drm_i915_gem_object *obj;
	struct mempolicy *policy;
	struct scatterlist *pages;
	struct shmem_error *error;
	unsigned long flags;
};

struct shmem_chunk {
	struct scatterlist *sg;
	struct i915_tbb tbb;
	struct intel_memory_region *mem;
	struct address_space *mapping;
	struct i915_sw_fence *fence;
	struct mempolicy *policy;
	unsigned int idx, end;
	unsigned int flags;
#define SHMEM_CLEAR	BIT(0)
#define SHMEM_CLFLUSH	BIT(1)
#define SHMEM_CACHE	BIT(2)
#define SHMEM_ONCE	BIT(3)
};

#if IS_ENABLED(CONFIG_NUMA)
#define swap_mempolicy(tsk, pol) (pol) = xchg(&(tsk)->mempolicy, (pol))
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
			int remain = sg->length;

			do {
				int len = PageHighMem(page) ? PAGE_SIZE : remain;
				void *ptr = kmap_atomic(page);

				if (flags & SHMEM_CLEAR) {
					if (flags & SHMEM_CACHE || !i915_memclear_nocache(ptr, len))
						memset(ptr, 0, len);
					if (clear) {
						mark_clear(page);
						clear = false;
					}
				}
				if (flags & SHMEM_CLFLUSH)
					clflush_cache_range(ptr, len);

				kunmap_atomic(ptr);
				page = nth_page(page, 1);
				remain -= len;
			} while (remain);
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

static void shmem_chunk(struct i915_tbb *tbb)
{
	struct shmem_chunk *chunk = container_of(tbb, typeof(*chunk), tbb);
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
	} else {
		if (end - idx <= SG_MAX_SINGLE_ALLOC)
			sg->page_link = SG_END;
	}

	i915_sw_fence_complete(fence);
}

static void
shmem_queue(struct shmem_chunk *chunk,
	    struct i915_tbb_node *tbb,
	    struct list_head *tasks)
{
	chunk->tbb.fn = shmem_chunk;

	i915_tbb_lock(tbb);
	list_add_tail(&chunk->tbb.local, tasks);
	if (IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_PARALLEL_SHMEMFS))
		i915_tbb_add_task_locked(tbb, &chunk->tbb);
	else
		INIT_LIST_HEAD(&chunk->tbb.link);
	i915_tbb_unlock(tbb);
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

static unsigned long l2_cache_size(void)
{
	return SZ_2M; /* XXX see unexported get_cpu_cachinfo() */
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

	if (obj->flags & I915_BO_SYNC_HINT && obj->base.size <= l2_cache_size())
		flags |= SHMEM_CACHE;

	return flags;
}

static int __fence_started(struct i915_active_fence *ref)
{
	struct dma_fence *f;
	int ret;

	rcu_read_lock();
	f = rcu_dereference(ref->fence);
	if (IS_ERR_OR_NULL(f))
		ret = 1;
	else if (!dma_fence_is_i915(f) ||
		 __i915_request_is_running(to_request(f)))
		ret = 0;
	else
		ret = -1;
	rcu_read_unlock();

	return ret;
}

static bool __fence_error(struct i915_active_fence *ref)
{
	struct dma_fence *f;
	bool ret;

	rcu_read_lock();
	f = rcu_dereference(ref->fence);
	if (IS_ERR_OR_NULL(f))
		ret = f;
	else
		ret = f->error;
	rcu_read_unlock();

	return ret;
}

static struct page *
get_clear_page(struct intel_memory_region *mem, int order,
	       int maxnode, const unsigned long *nodes,
	       unsigned int flags, bool need_blt)
{
	struct shmem_private *mp = to_shmem_private(mem);
	struct clear_pages *pages = &mp->clear[order];
	struct list_head *lists[] = {
		&pages->dirty,
		&pages->clean,
	};
	struct clear_page *cp, *cn;
	int i;

	GEM_BUG_ON(order >= ARRAY_SIZE(mp->clear));
	if (list_empty(&pages->dirty) && list_empty(&pages->clean))
		return NULL;

	if (flags & SHMEM_CLEAR)
		swap(lists[0], lists[1]);
	else
		flags = 0;

	spin_lock(&pages->lock);
	for (i = 0; i < ARRAY_SIZE(lists); i++) {
		list_for_each_entry_safe(cp, cn, lists[i], link) {
			if (unlikely(!cp->page))
				continue;

			if (flags & SHMEM_CLEAR && __fence_error(&cp->active)) {
				struct list_head *head;

				head = &pages->dirty;
				if (!IS_ERR(rcu_access_pointer(cp->active.fence)))
					head = head->prev;
				list_move(&cp->link, head);
				continue;
			}

			if (maxnode && (cp->nid >= maxnode || !test_bit(cp->nid, nodes)))
				continue;

			/* Keep searching for a short while for an idle page */
			if (!need_blt && __fence_started(&cp->active) < !!(flags & SHMEM_CACHE))
				break;

			list_del(&cp->link);
			spin_unlock(&pages->lock);

			atomic_dec(&mp->clear_count);
			atomic_long_sub(BIT(order), &mp->clear_pages);
			mod_node_page_state(page_pgdat(cp->page), NR_KERNEL_MISC_RECLAIMABLE, -BIT(order));

			return cp->page;
		}

		if (flags & SHMEM_ONCE || need_blt)
			break;

		flags = 0;
	}
	spin_unlock(&pages->lock);

	return NULL;
}

static void __add_clear_page(struct shmem_private *mp, struct clear_page *cp, struct clear_pages *pages, int order)
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

	mod_node_page_state(page_pgdat(cp->page), NR_KERNEL_MISC_RECLAIMABLE, BIT(order));
}

static void add_clear_page(struct intel_memory_region *mem, struct clear_page *cp, int order)
{
	struct shmem_private *mp = to_shmem_private(mem);
	struct clear_pages *pages = &mp->clear[order];

	spin_lock(&pages->lock);
	__add_clear_page(mp, cp, pages, order);
	spin_unlock(&pages->lock);

	atomic_long_add(BIT(order), &mp->clear_pages);
	atomic_inc(&mp->clear_count);
}

static void keep_sg(struct intel_memory_region *mem,
		    struct scatterlist *sg,
		    struct drm_i915_gem_object *obj)
{
	struct shmem_private *mp = to_shmem_private(mem);
	struct clear_pages *pages;
	unsigned int length = 0;
	spinlock_t *lock = NULL;
	unsigned long total = 0;
	int count = 0;
	int order = 0;

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

			order = get_order(length);
			pages = &mp->clear[order];
			lock = &pages->lock;
			spin_lock(lock);
		}

		cp = to_clear_page(page);
		__add_clear_page(mp, cp, pages, order);
		if (obj)
			memcpy(cp->tlb, obj->mm.tlb, sizeof(obj->mm.tlb));

		total += BIT(order);
		count++;
	}

	if (!lock)
		return;

	spin_unlock(lock);
	atomic_long_add(total, &mp->clear_pages);
	atomic_add(count, &mp->clear_count);
}

static void release_clear_page(struct intel_memory_region *mem, struct page *page, int order, u32 *tlb)
{
	struct clear_page *cp = to_clear_page(page);

	might_sleep();

	ClearPagePrivate(page);
	page->private = 0;

	i915_active_fence_fini(&cp->active);

	intel_tlb_sync(mem->i915, tlb ?: cp->tlb);
	shmem_dma_put(cp->map[0]);
	if (cp->map[1])
		shmem_dma_put(cp->map[1]);

	kmem_cache_free(slab_clear, cp);
}

static unsigned long shrink_shmem_cache(struct intel_memory_region *mem, int order, unsigned long target)
{
	struct clear_page bookmark = {};
	struct shmem_private *mp;
	unsigned long count = 0;

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
			list_for_each_entry(cp, lists[i], link) {
				struct dma_fence *f;
				struct page *page;

				page = cp->page;
				if (!page)
					continue;

				if (target != -1 && i915_active_fence_isset(&cp->active))
					break;

				list_replace(&cp->link, &bookmark.link);
				spin_unlock(&pages->lock);

				f = i915_active_fence_get(&cp->active);
				if (f) { /* enforce cleanup */
					dma_fence_wait(f, false);
					dma_fence_put(f);
				}

				WRITE_ONCE(mp->shrink, true);
				atomic_dec(&mp->clear_count);
				atomic_long_sub(BIT(order), &mp->clear_pages);
				mod_node_page_state(page_pgdat(page), NR_KERNEL_MISC_RECLAIMABLE, -BIT(order));

				release_clear_page(mem, page, order, NULL);
				__free_pages(page, order);
				cond_resched();

				spin_lock(&pages->lock);
				__list_del_entry(&bookmark.link);
				cp = &bookmark;

				count += BIT(order);
				if (count >= target)
					break;
			}
		}
		spin_unlock(&pages->lock);
	}

	return count;
}

static unsigned long count_shmem_cache(struct intel_memory_region *mem, unsigned long *num_objects)
{
	struct shmem_private *mp;

	mp = to_shmem_private(mem);
	if (unlikely(!mp))
		return 0;

	*num_objects = atomic_read(&mp->clear_count);
	return atomic_long_read(&mp->clear_pages);
}

static void split_clear_page(struct intel_memory_region *mem,
			     struct page *page,
			     int order,
			     int need_order)
{
	const struct clear_page * const cp = to_clear_page(page);
	struct dma_fence *f = i915_active_fence_get_or_error(&cp->active);
	int i;

	for (i = need_order; i < order; i++) {
		struct page *p = nth_page(page, BIT(i));
		struct clear_page *split;

		/* XXX loses debug info like page_owner */
		init_page_count(p);

		split = kmem_cache_alloc(slab_clear, GFP_KERNEL);
		if (!split) {
			__free_pages(p, i);
			continue;
		}

		INIT_ACTIVE_FENCE(&split->active);
		if (IS_ERR_OR_NULL(f))
			RCU_INIT_POINTER(split->active.fence, f);
		else
			set_fence_or_error(&split->active, f);

		split->page = p;
		split->nid = cp->nid;

		split->map[0] = shmem_dma_get(cp->map[0]);
		split->dma[0] = cp->dma[0] + BIT(i + PAGE_SHIFT);

		split->map[1] = NULL;
		split->dma[1] = cp->dma[1];
		if (split->dma[1]) {
			split->dma[1] += BIT(i + PAGE_SHIFT);
			split->map[1] = shmem_dma_get(cp->map[1]);
		}
		split->engine = cp->engine;

		p->private = (unsigned long)split;
		GEM_BUG_ON(PagePrivate(p));
		__set_bit(PG_private, &p->flags); /* XXX workaround ICE on gcc-7.5 */

		add_clear_page(mem, split, i);
	}

	if (!IS_ERR_OR_NULL(f))
		dma_fence_put(f);
}

static bool smem_context_ready(struct intel_gt *gt)
{
	struct intel_context *ce = i915_gem_get_active_smem_context(gt);

	return ce && intel_context_throttle(ce, 0) == 0;
}

static int shmem_create(struct shmem_work *wrk)
{
	const unsigned int limit = SZ_4M;
	const uint32_t max_segment = i915_sg_segment_size();
	struct drm_i915_gem_object *obj = wrk->obj;
	struct i915_tbb_node *tbb = i915_tbb_node(dev_to_node(obj->base.dev->dev));
	struct intel_memory_region *mem = obj->mm.region.mem;
	struct i915_dma_engine *de = get_dma_engine(mem_cpu(mem));
	struct scatterlist *sgt = wrk->pages, *need_blt = NULL;
	u64 remain = obj->base.size, dirty = limit;
	unsigned long flags = wrk->flags;
	struct shmem_chunk *chunk = NULL;
	struct scatterlist *sg, *chain;
	int last_node = mem_node(mem);
	int min_order = DMA_MAX_ORDER;
	struct i915_sw_fence fence;
	LIST_HEAD(tasks);
	unsigned long n;
	gfp_t gfp;

	gfp = GFP_HIGHUSER | __GFP_RECLAIMABLE;
	gfp |= __GFP_RETRY_MAYFAIL | __GFP_NOWARN;

	if (obj->flags & I915_BO_ALLOC_CONTIGUOUS)
		gfp &= ~__GFP_HIGH;
	else
		gfp &= ~__GFP_RECLAIM;

	i915_sw_fence_init_onstack(&fence);

	n = 0;
	sg = sgt;
	chain = sg + SG_NUM_INLINE - 1;
	GEM_BUG_ON(sg_capacity(sgt) > SG_NUM_INLINE);
	do {
		int need_order = ilog2(min_t(u64, remain, max_segment)) - PAGE_SHIFT;
		struct clear_page *cp;
		struct page *page;
		int order, nid;

		/* First see if we can split a clear page to fit */
		if (IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_SMEM_SPLIT)) {
			for (order = need_order; order <= min_order; order++) {
				page = get_clear_page(mem, order,
						      obj->maxnode,
						      get_obj_nodes(obj),
						      flags | SHMEM_ONCE,
						      need_blt);
				if (!page)
					continue;

				if (order > need_order) {
					split_clear_page(mem, page, order, need_order);
					order = need_order;
				}

				goto page;
			}
		}

restart:	/* Nothing readily available in the cache? Allocate some fresh pages */
		order = min(need_order, min_order);
		do {
			page = get_clear_page(mem, order,
					      obj->maxnode,
					      get_obj_nodes(obj),
					      flags, need_blt);
			if (page)
				break;

			page = alloc_pages_for_object(obj, &mem->interleave, gfp, order);
			if (page)
				break;

			if (shrink_shmem_cache(mem, order + 1, roundup_pow_of_two(remain) >> PAGE_SHIFT)) {
				min_order = DMA_MAX_ORDER;
				goto restart;
			}

			if (gfp & __GFP_DIRECT_RECLAIM)
				break;

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
			ras_error(obj);
			i915_sw_fence_set_error_once(&fence, -ENOMEM);
			sg->page_link = 0;
			break;
		}

		nid = page_to_nid(page);
		if (obj->maxnode && (nid >= obj->maxnode || !test_bit(nid, get_obj_nodes(obj))))
			ras_error(obj);

		if (!PagePrivate(page)) {
			cp = kmem_cache_alloc(slab_clear, GFP_KERNEL);
			if (!cp) {
				i915_sw_fence_set_error_once(&fence, -ENOMEM);
				sg->page_link = 0;
				break;
			}

			cp->map[0] = shmem_dma_map(obj->base.dev->dev,
						   page, order, DMA_BIDIRECTIONAL);
			if (!cp->map[0]) {
				i915_sw_fence_set_error_once(&fence, -ENOMEM);
				kmem_cache_free(slab_clear, cp);
				sg->page_link = 0;
				break;
			}

			__i915_active_fence_init(&cp->active,
						 no_init_on_alloc ? ERR_PTR(-ENODEV) : NULL,
						 NULL);
			cp->nid = nid;
			cp->page = page;
			cp->dma[0] = cp->map[0]->dma;

			if (de && nid != last_node) {
				de = get_dma_engine(__local_cpu(nid));
				last_node = nid;
			}

			cp->dma[1] = 0;
			cp->map[1] = NULL;
			if (de && order <= get_order(DMA_MAX_CLEAR))
				cp->map[1] = shmem_dma_map(de->dma->device->dev,
							   page, order, DMA_FROM_DEVICE);
			if (cp->map[1])
				cp->dma[1] = cp->map[1]->dma;
			cp->engine = de;

			page->private = (unsigned long)cp;
			GEM_BUG_ON(PagePrivate(page));
			SetPagePrivate(page);
		}

page:
		cp = to_clear_page(page);
		sg->page_link = (unsigned long)page;
		sg->length = BIT(order + PAGE_SHIFT);
		GEM_BUG_ON(get_order(sg->length) != order);
		sg->offset = 0;
		sg_dma_address(sg) = cp->dma[0];
		sg_dma_len(sg) = sg->length;
		sg_page_sizes(sgt) |= sg->length;

		if (flags && __fence_error(&cp->active)) {
			if (dirty >= limit) {
				struct dma_fence *f = NULL;

				if (i915_active_fence_isset(&cp->active)) {
					if (IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_SMEM_BLT) &&
					    i915_gem_get_active_smem_context(mem->gt)) {
						f = ERR_PTR(-1);
						need_blt = sg;
						flags = 0;
					}
				} else if (remain > limit) { /* keep some busywork for ourselves */
					if (cp->dma[1] && !cp->engine->zero_dma) {
						f = dma_clear(cp->engine, cp->dma[1], sg->length);
						if (f) {
							set_fence_or_error(&cp->active, f);
							fence_chain(&wrk->error->base.rq, f, &cp->cb);
							dma_fence_put(f);
						}
					}

					if (!f &&
					    IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_SMEM_BLT) &&
					    !list_empty(&tasks) &&
					    !waitqueue_active(&tbb->wq) &&
					    smem_context_ready(mem->gt)) {
						f = ERR_PTR(-1);
						need_blt = sg;
						flags = 0;
					}
				}

				if (chunk) {
					chunk->end = n;
					shmem_queue(chunk, tbb, &tasks);
					chunk = NULL;
				}

				if (!f) {
					chunk = kmap(page);
					chunk->sg = sg;
					chunk->fence = &fence;
					chunk->idx = n;
					chunk->flags = flags;
					chunk->policy = wrk->policy;

					i915_sw_fence_await(&fence);
					dirty = 0;
				}
			}

			dirty += sg->length;
		}
		n++;

		GEM_BUG_ON(sg->length > remain);
		remain -= sg->length;
		if (!remain)
			break;

		if (sg == chain) {
			unsigned int x;

			x = min_t(unsigned int, (remain >> PAGE_SHIFT) + 1, SG_MAX_SINGLE_ALLOC);
			chain = sg_pool_alloc(x, I915_GFP_ALLOW_FAIL);
			if (unlikely(!chain)) {
				i915_sw_fence_set_error_once(&fence, -ENOMEM);
				break;
			}

			__sg_chain(sg, memcpy(chain, sg, sizeof(*sg)));
			sg_capacity(sgt) += x - 1;

			if (chunk && chunk->sg == sg)
				chunk->sg = chain;
			if (need_blt == sg)
				need_blt = chain;

			GEM_BUG_ON(sg_chain_ptr(sg) != chain);
			GEM_BUG_ON(sg_page(chain) != page);
			sg = chain;
			chain += x - 1;

			cond_resched();
		}
		GEM_BUG_ON(sg_is_chain(sg));
		GEM_BUG_ON(sg_is_last(sg));
		sg++;
	} while (1);
	i915_sw_fence_commit(&fence);

	sg_mark_end(sg);
	sg_count(sgt) = n;
	GEM_BUG_ON(sg_count(sgt) > sg_capacity(sgt));

	if (chunk) {
		chunk->end = n;
		GEM_BUG_ON(need_blt);
		shmem_queue(chunk, tbb, &tasks);
	}

	if (!READ_ONCE(fence.error) && need_blt) {
		struct i915_request *rq = NULL;
		int err;

		err = i915_gem_clear_smem(i915_gem_get_active_smem_context(mem->gt),
					  need_blt, &rq);
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

	i915_tbb_run_local(tbb, &tasks, shmem_chunk);
	i915_sw_fence_wait(&fence);

	i915_sw_fence_fini(&fence);
	if (unlikely(fence.error))
		goto err;

	add_clear_fences(&wrk->error->base.rq, sgt, need_blt);

	GEM_BUG_ON(__sg_total_length(sgt, false) != obj->base.size);
	GEM_BUG_ON(__sg_total_length(sgt, true) != obj->base.size);

	//i915_request_set_priority(&wrk->error->base.rq, I915_PRIORITY_MAX);
	return 0;

err:
	keep_sg(mem, sgt, NULL);
	i915_sg_free_excess(sgt);
	return fence.error;
}

static int shmem_swapin(struct shmem_work *wrk)
{
	const unsigned int spread = max_t(unsigned int, SG_MAX_SINGLE_ALLOC, SZ_8M >> PAGE_SHIFT);
	struct drm_i915_gem_object *obj = wrk->obj;
	struct i915_tbb_node *tbb = i915_tbb_node(dev_to_node(obj->base.dev->dev));
	const unsigned int num_pages = obj->base.size >> PAGE_SHIFT;
	struct address_space *mapping = obj->base.filp->f_mapping;
	struct scatterlist *sgt = wrk->pages;
	struct scatterlist *sg = sgt;
	struct shmem_chunk *chunk = NULL;
	struct i915_sw_fence fence;
	unsigned int n;
	LIST_HEAD(tasks);

	BUILD_BUG_ON(sizeof(*chunk) > SG_NUM_INLINE * sizeof(*sg));

	i915_sw_fence_init_onstack(&fence);
	mapping_set_unevictable(mapping);

	n = num_pages;
	if (n > sg_capacity(sgt))
		n = sg_capacity(sgt) - 1;
	fence.error = __shmem_chunk(sg, obj->mm.region.mem,
				    mapping, wrk->policy,
				    0, n, wrk->flags,
				    &fence.error);

	while (!READ_ONCE(fence.error) && n < num_pages) {
		struct scatterlist *chain;
		unsigned int x;

		x = min_t(unsigned int, num_pages - n, SG_MAX_SINGLE_ALLOC);
		chain = sg_pool_alloc(x, I915_GFP_ALLOW_FAIL);
		if (unlikely(!chain)) {
			i915_sw_fence_set_error_once(&fence, -ENOMEM);
			n++;
			break;
		}

		sg_init_table(chain, x);
		__sg_chain(sg + min_t(unsigned int, n, I915_MAX_CHAIN_ALLOC), chain);
		sg = chain;

		if (chunk && n - chunk->idx > spread) {
			chunk->end = n;
			shmem_queue(chunk, tbb, &tasks);
			cond_resched();
			chunk = NULL;
		}

		if (chunk == NULL) {
			chunk = memset(sg, 0, sizeof(*chunk));
			chunk->sg = sg;
			chunk->fence = &fence;
			chunk->mem = obj->mm.region.mem;
			chunk->mapping = mapping;
			chunk->policy = wrk->policy;
			chunk->idx = n;
			chunk->flags = wrk->flags;
			i915_sw_fence_await(&fence);
		}

		n += x;
		n -= n < num_pages;
	}
	i915_sw_fence_commit(&fence);
	GEM_BUG_ON(n > num_pages);
	__sg_set_capacity(sgt, n);

	/* Leaving the last chunk for ourselves */
	if (chunk) {
		chunk->end = n;
		shmem_queue(chunk, tbb, &tasks);
		i915_tbb_run_local(tbb, &tasks, shmem_chunk);
		i915_sw_fence_wait(&fence);
	}
	GEM_BUG_ON(!list_empty(&tasks));

	i915_sw_fence_fini(&fence);
	if (unlikely(fence.error))
		goto err;

	GEM_BUG_ON(sg_capacity(sgt) != num_pages);
	GEM_BUG_ON(__sg_total_length(sgt, false) != obj->base.size);

	fence.error = i915_sg_map(sgt, obj->base.size,
				  i915_gem_sg_segment_size(obj),
				  obj->base.dev->dev);
	if (unlikely(fence.error))
		goto err;

	GEM_BUG_ON(__sg_total_length(sgt, false) != obj->base.size);
	GEM_BUG_ON(__sg_total_length(sgt, true) != obj->base.size);

	return 0;

err:
	mapping_clear_unevictable(mapping);
	for (sg = sgt; sg; sg = __sg_next(sg)) {
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
	}
	i915_sg_free_excess(sgt);

	/*
	 * shmemfs first checks if there is enough memory to allocate the page
	 * and reports ENOSPC should there be insufficient, along with the usual
	 * ENOMEM for a genuine allocation failure.
	 *
	 * We use ENOSPC in our driver to mean that we have run out of aperture
	 * space and so want to translate the error from shmemfs back to our
	 * usual understanding of ENOMEM.
	 */
	if (fence.error == -ENOSPC)
		fence.error = -ENOMEM;

	return fence.error;
}

static int shmem_work(struct dma_fence_work *base)
{
	struct shmem_work *wrk = container_of(base, typeof(*wrk), base);
	int err, cpu;

	cpu = i915_tbb_suspend_local();
	if (!wrk->obj->base.filp)
		err = shmem_create(wrk);
	else
		err = shmem_swapin(wrk);
	i915_tbb_resume_local(cpu);
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
	.name = "[shmem]",
	.work = shmem_work,
	.release = shmem_work_release,
};

static int shmem_error(struct dma_fence_work *base)
{
	struct shmem_error *e = container_of(base, typeof(*e), base);

	if (likely(!e->base.rq.submit.error) ||
	    !(e->flags & SHMEM_CLEAR) ||
	    !sg_count(e->pages))
		return e->base.rq.submit.error;

	if (test_bit(DMA_FENCE_WORK_IMM, &base->rq.fence.flags))
		return -ERESTARTSYS; /* only run from kworker */

	/* Ignore any blt errors and redo the work */
	return __shmem_chunk(e->pages,
			     NULL, NULL, NULL,
			     0, sg_count(e->pages),
			     e->flags, NULL);
}

static const struct dma_fence_work_ops error_ops = {
	.name = "shmem",
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

	e->pages = wrk->pages;
	e->flags = wrk->flags;

	return e;
}

static int shmem_get_pages(struct drm_i915_gem_object *obj)
{
	struct intel_memory_region *mem = obj->mm.region.mem;
	struct scatterlist *sg;
	struct shmem_work *wrk;
	unsigned int num_pages;
	int err;

	if (!safe_conversion(&num_pages, obj->base.size >> PAGE_SHIFT))
		return -E2BIG;

	/*
	 * If there's no chance of allocating enough pages for the whole
	 * object, bail early.
	 */
	if (num_pages > totalram_pages())
		return -E2BIG;

	sg = sg_table_inline_create(I915_GFP_ALLOW_FAIL);
	if (unlikely(!sg))
		return -ENOMEM;

	wrk = kmalloc(sizeof(*wrk), GFP_KERNEL);
	if (!wrk) {
		err = -ENOMEM;
		goto err_sg;
	}
	dma_fence_work_init(&wrk->base, &shmem_ops,
			    to_i915(obj->base.dev)->mm.sched);
	wrk->obj = i915_gem_object_get(obj);
	wrk->pages = sg;
	wrk->flags = shmem_create_mode(obj, i915_memclear_nocache(NULL, 0));
	wrk->policy = get_mempolicy(current);
	if (wrk->policy)
		wrk->base.cpu = raw_smp_processor_id();

	if (!obj->base.filp) {
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
	} else {
		i915_gem_object_migrate_prepare(obj, &wrk->base.rq.fence);
	}

	atomic64_sub(obj->base.size, &mem->avail);
	__i915_gem_object_set_pages(obj, sg); /* placeholder */

	dma_fence_work_commit_imm_if(&wrk->base,
				     obj->flags & I915_BO_SYNC_HINT ||
				     obj->base.size <= SZ_64K ||
				     !obj->base.filp);
	return 0;

err_sg:
	sg_table_inline_free(sg);
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
shmem_put_pages(struct drm_i915_gem_object *obj, struct scatterlist *pages)
{
	struct intel_memory_region *mem = obj->mm.region.mem;
	bool clflush = shmem_create_mode(obj, false) & SHMEM_CLFLUSH;
	bool do_swap = need_swap(obj);
	struct pagevec pvec;

	i915_gem_object_migrate_finish(obj);
	if (!sg_count(pages))
		goto empty;

	pagevec_init(&pvec);
	if (obj->base.filp) {
		struct address_space *mapping = obj->base.filp->f_mapping;
		struct iommu_domain *domain;
		struct scatterlist *sg;

		mapping_clear_unevictable(mapping);

		intel_tlb_sync(to_i915(obj->base.dev), obj->mm.tlb);

		domain = get_iommu_domain(obj->base.dev->dev);
		if (domain && sg_dma_len(pages))
			__i915_iommu_free(sg_dma_address(pages), obj->base.size, obj->base.size, domain);

		for (sg = pages; sg; sg = __sg_next(sg)) {
			struct page *page = sg_page(sg);
			int i;

			if (clflush) {
				void *ptr = kmap_atomic(page);
				clflush_cache_range(ptr, sg->length);
				kunmap_atomic(ptr);
			}

			if (do_swap) {
				set_page_dirty(page);
				mark_page_accessed(page);
			} else {
				cancel_dirty_page(page);
			}

			for (i = 0; i < sg->length >> PAGE_SHIFT; i++)
				page_release(nth_page(page, i), &pvec);
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

		for (sg = pages; sg; sg = __sg_next(sg)) {
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
#ifdef BPM_SET_PAGE_SWAP_BACKED_NOT_PRESENT
					__folio_set_swapbacked(page_folio(p));
#else
					__SetPageSwapBacked(p);
#endif
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

		for (sg = pages; sg; sg = __sg_next(sg)) {
			int order = get_order(sg->length);
			struct page *page = sg_page(sg);

			release_clear_page(mem, page, order, obj->mm.tlb);
			__free_pages(page, order);
		}
	} else { /* device-local host pages, keep for future use */
		struct scatterlist *sg = pages;

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

				set_fence_or_error(&cp->active, f);
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
					if (i915_gem_clear_smem(ce, sg, &rq) == 0)
						sg = NULL;
					if (rq) {
						dma_fence_enable_sw_signaling(&rq->fence);
						i915_sw_fence_complete(&rq->submit);
						i915_request_put(rq);
					}
				}
			}
		}

		for (; sg; sg = __sg_next(sg)) {
			struct clear_page *cp = to_clear_page(sg_page(sg));

			cmpxchg_relaxed((struct dma_fence **__force)&cp->active.fence,
					NULL, ERR_PTR(-ENODEV));
		}

		keep_sg(mem, pages, obj);
	}
	if (pagevec_count(&pvec))
		check_release_pagevec(&pvec);

empty:
	atomic64_add(obj->base.size, &mem->avail);

	sg_table_inline_free(pages);
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

	if (flags & I915_BO_ALLOC_CONTIGUOUS &&
	    (size > BIT(DMA_MAX_ORDER + PAGE_SHIFT) || !is_power_of_2(size)))
		return -E2BIG;

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
	loff_t pos = 0;
	struct file *file;
	int err;

	obj = i915_gem_object_create_shmem(dev_priv, round_up(size, PAGE_SIZE));
	if (IS_ERR(obj))
		return obj;

	err = __create_shmem(obj);
	if (err)
		goto fail;

	file = obj->base.filp;
#ifdef BPM_WRITE_BEGIN_STRUCT_PAGE_MEMBER_NOT_PRESENT
	const struct address_space_operations *aops = file->f_mapping->a_ops;
	if (aops == NULL) {
		err = EFAULT;
		goto fail;
	}
#endif

	do {
		unsigned int len = min_t(typeof(size), size, PAGE_SIZE);
#ifdef BPM_WRITE_BEGIN_STRUCT_PAGE_MEMBER_NOT_PRESENT
		struct folio *folio;
		void *fsdata;

		err = aops->write_begin(file, file->f_mapping, pos, len,
					&folio, &fsdata);
#else
		struct page *page;
		void *pgdata, *vaddr;

		err = pagecache_write_begin(file, file->f_mapping,
					    pos, len, 0,
					    &page, &pgdata);
#endif
		if (err < 0)
			goto fail;

#ifdef BPM_WRITE_BEGIN_STRUCT_PAGE_MEMBER_NOT_PRESENT
		memcpy_to_folio(folio, offset_in_folio(folio, pos), data, len);

		err = aops->write_end(file, file->f_mapping, pos, len, len,
					folio, fsdata);
#else
		vaddr = kmap(page);
		memcpy(vaddr, data, len);
		kunmap(page);

		err = pagecache_write_end(file, file->f_mapping,
					  pos, len, len,
					  page, pgdata);
#endif
		if (err < 0)
			goto fail;

		size -= len;
		data += len;
		pos += len;
	} while (size);

	return obj;

fail:
	i915_gem_object_put(obj);
	return ERR_PTR(err);
}

static struct page *
get_dirty_page(struct intel_memory_region *mem, int *order, unsigned long *total)
{
	struct page *page = NULL;
	struct shmem_private *mp;
	int nid = mem_node(mem);
	struct clear_page *cp;
	int nr_free;

	if (READ_ONCE(mem->gt->suspend))
		return NULL;

	mp = to_shmem_private(mem);
	if (unlikely(!mp))
		return NULL;

	for (*order = ARRAY_SIZE(mp->clear); --*order >= 0; ) {
		struct clear_pages *pages = &mp->clear[*order];

		if (list_empty(&pages->dirty))
			continue;

		spin_lock(&pages->lock);
		list_for_each_entry(cp, &pages->dirty, link) {
			if (!cp->page)
				continue;

			GEM_BUG_ON(!i915_active_fence_has_error(&cp->active));
			list_del(&cp->link);
			page = cp->page;
			break;
		}
		spin_unlock(&pages->lock);
		if (page) {
			atomic_dec(&mp->clear_count);
			atomic_long_sub(BIT(*order), &mp->clear_pages);
			mod_node_page_state(page_pgdat(page), NR_KERNEL_MISC_RECLAIMABLE, -BIT(*order));
			return page;
		}
	}

	if (atomic_long_read(&mp->clear_pages) + *total > mp->low_clear_pages)
		return NULL;

	nr_free = sum_node_pages(nid, NR_FREE_PAGES);
	if (nr_free < 128 * BIT(DMA_MAX_ORDER))
		return NULL;

	/* If we have shrunk the cache since the last time, stop expanding */
	nr_free = READ_ONCE(mp->shrink);
	WRITE_ONCE(mp->shrink, false);
	if (nr_free)
		return NULL;

	for (*order = DMA_MAX_ORDER; *order >= get_order(SZ_64K); --*order)  {
		page = alloc_pages_node(nid,
					GFP_NOWAIT | __GFP_THISNODE | __GFP_NORETRY | __GFP_NOWARN,
					*order);
		if (page)
			break;
	}
	if (!page)
		return NULL;

	cp = kmem_cache_alloc(slab_clear, GFP_KERNEL);
	if (!cp) {
		__free_pages(page, *order);
		return NULL;
	}

	__i915_active_fence_init(&cp->active,
				 no_init_on_alloc ? ERR_PTR(-ENODEV) : NULL,
				 NULL);
	cp->page = page;
	cp->nid = nid;

	cp->map[0] = shmem_dma_map(mem->i915->drm.dev, page, *order, DMA_BIDIRECTIONAL);
	if (!cp->map[0]) {
		kmem_cache_free(slab_clear, cp);
		__free_pages(page, *order);
		return NULL;
	}
	cp->dma[0] = cp->map[0]->dma;

	cp->dma[1] = 0;
	cp->map[1] = NULL;
	cp->engine = NULL;
	if (*order <= get_order(DMA_MAX_CLEAR)) {
		cp->engine = get_dma_engine(__local_cpu(cp->nid));
		if (cp->engine)
			cp->map[1] = shmem_dma_map(cp->engine->dma->device->dev, page, *order, DMA_FROM_DEVICE);
		if (cp->map[1])
			cp->dma[1] = cp->map[1]->dma;
		if (cp->dma[1] && i915_active_fence_has_error(&cp->active)) {
			struct dma_fence *f;

			f = dma_clear(cp->engine, cp->dma[1], BIT(*order + PAGE_SHIFT));
			if (f) {
				set_fence_or_error(&cp->active, f);
				dma_fence_put(f);
			}
		}
	}

	page->private = (unsigned long)cp;
	GEM_BUG_ON(PagePrivate(page));
	SetPagePrivate(page);

	*total += BIT(*order);
	return page;
}

static void
free_dirty_pages(struct intel_memory_region *mem)
{
	struct clear_page bookmark = {};
	struct shmem_private *mp;
	unsigned long remain;
	int order;

	mp = to_shmem_private(mem);
	if (unlikely(!mp))
		return;

	remain = atomic_long_read(&mp->clear_pages);
	for (order = 0;
	     remain > mp->high_clear_pages - BIT(order) &&
	     order < ARRAY_SIZE(mp->clear);
	     order++) {
		struct clear_pages *pages = &mp->clear[order];
		struct clear_page *cp;

		if (list_empty(&pages->dirty))
			continue;

		spin_lock(&pages->lock);
		list_for_each_entry_reverse(cp, &pages->dirty, link) {
			struct page *page;

			page = cp->page;
			if (!page)
				continue;

			list_replace(&cp->link, &bookmark.link);
			spin_unlock(&pages->lock);

			atomic_dec(&mp->clear_count);
			remain = atomic_long_sub_return(BIT(order), &mp->clear_pages);
			mod_node_page_state(page_pgdat(page), NR_KERNEL_MISC_RECLAIMABLE, -BIT(order));
			release_clear_page(mem, page, order, NULL);
			__free_pages(page, order);
			cond_resched();

			spin_lock(&pages->lock);
			__list_del_entry(&bookmark.link);
			cp = &bookmark;

			if (remain <= mp->high_clear_pages)
				break;
		}
		spin_unlock(&pages->lock);
	}
}

bool i915_gem_shmem_park(struct intel_memory_region *mem)
{
	struct scatterlist *sg, *tail, *end = NULL;
	struct intel_migrate_window *w;
	struct intel_context *ce;
	unsigned long total = 0;
	struct scatterlist *sgt;
	struct page *page;
	int order;

	free_dirty_pages(mem); /* throwaway excess */

	if (!IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_SMEM_IDLE))
		return false;

	ce = i915_gem_get_free_smem_context(mem->gt);
	if (!ce || !ce->private)
		return false;

	page = get_dirty_page(mem, &order, &total);
	if (!page) {
		clear_bit(INTEL_MEMORY_CLEAR_FREE, &mem->flags);
		return false;
	}

	__intel_wakeref_defer_park(&mem->gt->wakeref);
	reinit_completion(&mem->parking);
	mutex_unlock(&mem->gt->wakeref.mutex);

	w = ce->private;
	sgt = __sg_table_inline_create(GFP_NOWAIT | __GFP_NOWARN);
	if (unlikely(!sgt)) {
		release_clear_page(mem, page, order, NULL);
		__free_pages(page, order);
		goto out;
	}

	sg = sgt;
	sg_init_capacity(sgt);
	tail = sg + sg_capacity(sgt) - 1;
	do {
		struct clear_page *cp = to_clear_page(page);

		if (!__fence_error(&cp->active)) {
			add_clear_page(mem, cp, order);
			continue;
		}

		sg->page_link = (unsigned long)page;
		sg->length = BIT(order + PAGE_SHIFT);
		GEM_BUG_ON(get_order(sg->length) != order);
		sg->offset = 0;
		sg_dma_address(sg) = cp->dma[0];
		sg_dma_len(sg) = sg->length;

		if (sg == tail) {
			struct scatterlist *chain;

			chain = sg_pool_alloc(SG_MAX_SINGLE_ALLOC, GFP_NOWAIT | __GFP_NOWARN);
			if (unlikely(!chain))
				break;

			__sg_chain(sg, memcpy(chain, sg, sizeof(*sg)));
			GEM_BUG_ON(sg_chain_ptr(sg) != chain);
			GEM_BUG_ON(sg_page(chain) != page);

			sg_capacity(sgt) += I915_MAX_CHAIN_ALLOC;
			tail = chain + I915_MAX_CHAIN_ALLOC;
			sg = chain;

			cond_resched();
		}
		GEM_BUG_ON(sg_is_last(sg));
		GEM_BUG_ON(sg_is_chain(sg));
		end = sg++;
	} while (atomic_read(&mem->gt->wakeref.count) == 1 &&
		 (page = get_dirty_page(mem, &order, &total)));

	if (end) {
		struct i915_request *rq = NULL;

		sg_mark_end(end);
		i915_gem_clear_smem(ce, sgt, &rq);
		if (rq) {
			dma_fence_enable_sw_signaling(&rq->fence); /* fast retire */
			i915_sw_fence_complete(&rq->submit);
			i915_request_put(rq);
		}
		keep_sg(mem, sgt, NULL);
	}

	sg_table_inline_free(sgt);

out:
	complete_all(&mem->parking);
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

		if (IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_SMEM_IDLE)) {
			n = mem_node(mem);
			if (n != NUMA_NO_NODE)
				mp->high_clear_pages = node_present_pages(n);
			else
				mp->high_clear_pages = totalram_pages();
			mp->high_clear_pages >>= 2;
		}
		mp->low_clear_pages = min_t(unsigned long, SZ_8G >> PAGE_SHIFT, mp->high_clear_pages >> 2);
	}

	mem->region_private = mp;
	return 0; /* We have fallback to the kernel mnt if gemfs init failed. */
}

static void release_shmem(struct intel_memory_region *mem)
{
	struct shmem_private *mp;

	shrink_shmem_cache(mem, 0, -1ul);

	mp = to_shmem_private(mem);
	if (mp) {
		free_errors(mp->errors);
		kfree(mp);
	}

	i915_gemfs_fini(mem->i915);
}

static void show_shmem(struct intel_memory_region *mem, struct drm_printer *p, int indent)
{
	struct i915_dma_engine *de;
	struct shmem_private *mp;
	unsigned long count;
	intel_wakeref_t wf;
	char bytes[16];
	char buf[256];
	int order;

	mp = to_shmem_private(mem);
	if (!mp)
		return;

	i_printf(p, indent, "clear:\n");
	indent += 2;

	de = lookup_dma_engine(mem_cpu(mem));
	if (de) {
		i_printf(p, indent, "using: %s (%s) [%s]\n",
			  dma_chan_name(de->dma),
			  de->zero_dma ? "memcpy" : "memset",
			  dev_name(de->dma->device->dev));
	}

	count = atomic_long_read(&mp->clear_pages);
	string_get_size(count, 4096, STRING_UNITS_2, bytes, sizeof(bytes));
	i_printf(p, indent, "total: %lu pages [%s]\n", count, bytes);

	if (mp->high_clear_pages) {
		string_get_size(mp->low_clear_pages, 4096, STRING_UNITS_2, buf, sizeof(buf));
		string_get_size(mp->high_clear_pages, 4096, STRING_UNITS_2, bytes, sizeof(bytes));
		i_printf(p, indent, "limit: { low: %lu pages [%s], high: %lu pages [%s] }\n",
			 mp->low_clear_pages, buf,
			 mp->high_clear_pages, bytes);
	}

	if (mem->gt->counters.map && (wf = intel_gt_pm_get_if_awake(mem->gt))) {
		u64 time = mem->gt->counters.map[INTEL_GT_CLEAR_SMEM_CYCLES];
		u64 total = mem->gt->counters.map[INTEL_GT_CLEAR_SMEM_BYTES];

		if (total != -1 && time) {
			time = intel_gt_clock_interval_to_ns(mem->gt, time);
			time = div_u64(time + NSEC_PER_MSEC - 1, NSEC_PER_MSEC);
			string_get_size(total, 1,
					STRING_UNITS_2, bytes, sizeof(bytes));
			string_get_size(div64_u64(total, time), 1000,
					STRING_UNITS_2, buf, sizeof(buf));
			i_printf(p, indent, "offload: %s in %llu ms, %s/s\n",
				   bytes, time, buf);
		}
		intel_gt_pm_put_async(mem->gt, wf);
	}

	i_printf(p, indent, "order:\n");
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

		i_printf(p, indent + 2, "- [%d]: { count:%lu%s }\n",
			 PAGE_SHIFT + order, count, buf);
	}
}

static const struct intel_memory_region_ops shmem_region_ops = {
	.init = init_shmem,
	.show = show_shmem,
	.count_cache = count_shmem_cache,
	.shrink_cache = shrink_shmem_cache,
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
					     de->zero_dma, DMA_MAX_CLEAR,
					     DMA_TO_DEVICE,
					     DMA_ATTR_SKIP_CPU_SYNC);
		if (de->zero)
			__free_pages(de->zero, get_order(DMA_MAX_CLEAR));
		dma_release_channel(de->dma);
		kfree(de);
	}
}

void i915_gem_shmem_module_exit(void)
{
	cleanup_dma_engines();
	kmem_cache_destroy(slab_dma);
	kmem_cache_destroy(slab_clear);
}

int __init i915_gem_shmem_module_init(void)
{
	slab_clear = KMEM_CACHE(clear_page, 0);
	if (!slab_clear)
		return -ENOMEM;

	slab_dma = KMEM_CACHE(shmem_dma, 0);
	if (!slab_dma) {
		kmem_cache_destroy(slab_clear);
		return -ENOMEM;
	}

	return 0;
}
