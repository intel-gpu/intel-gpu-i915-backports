// SPDX-License-Identifier: MIT
/*
 * Copyright © 2012-2023 Intel Corporation
 */

#ifdef BPM_RH_DRM_BACKPORT_MMU_NOTIFIER_WRAPPER_1
#define RH_DRM_BACKPORT
#endif

#include <linux/mmu_context.h>
#include <linux/pagevec.h>
#include <linux/swap.h>
#include <linux/sched/mm.h>


#ifdef BPM_MMAP_WRITE_LOCK_NOT_PRESENT
#include <linux/mmap_lock.h>
#endif

#include "i915_drv.h"
#include "i915_gem_ioctls.h"
#include "i915_gem_object.h"
#include "i915_gem_region.h"
#include "i915_scatterlist.h"
#include "i915_sw_fence_work.h"

#ifdef BPM_MMU_INTERVAL_NOTIFIER_NOTIFIER_NOT_PRESENT
#include <linux/i915_gem_mmu_notifier.h>
#endif

#ifdef BPM_MMU_INTERVAL_NOTIFIER_NOTIFIER_NOT_PRESENT
#ifdef BPM_I915_MMU_OBJECT_NOT_PRESENT
#include <linux/interval_tree.h>

struct i915_mmu_notifier {
        spinlock_t lock;
        struct hlist_node node;
        struct mmu_notifier mn;
        struct rb_root_cached objects;
        struct i915_mm_struct *mm;
};

struct i915_mmu_object {
        struct i915_mmu_notifier *mn;
        struct drm_i915_gem_object *obj;
        struct interval_tree_node it;
};
#else
struct i915_mm_struct;

struct i915_mmu_notifier {
       struct hlist_node node;
       struct mmu_notifier mn;
       struct i915_mm_struct *mm;
};
#endif
#endif

#ifndef MAX_STACK_ALLOC
#define MAX_STACK_ALLOC 512
#endif

#if IS_ENABLED(CONFIG_MMU_NOTIFIER)
static bool i915_gem_userptr_invalidate(struct mmu_interval_notifier *mni,
					const struct mmu_notifier_range *range,
					unsigned long cur_seq)
{
	if (range->event == MMU_NOTIFY_UNMAP)
		mmu_interval_set_seq(mni, cur_seq);
	return true;
}

#ifdef BPM_MMU_INTERVAL_NOTIFIER_NOTIFIER_NOT_PRESENT
#ifdef BPM_I915_MMU_OBJECT_NOT_PRESENT
static void
userptr_mn_invalidate_range_start(struct mmu_notifier *_mn,
                                  struct mm_struct *mm,
                                  unsigned long start, unsigned long end)
{
        struct i915_mmu_notifier *mn =
                container_of(_mn, struct i915_mmu_notifier, mn);
        struct interval_tree_node *it;
        int ret = 0;

        if (RB_EMPTY_ROOT(&mn->objects.rb_root))
                return;

        /* interval ranges are inclusive, but invalidate range is exclusive */
        end--;

        spin_lock(&mn->lock);
        it = interval_tree_iter_first(&mn->objects, start, end);
        while (it) {
                struct drm_i915_gem_object *obj;

                /*
                 * The mmu_object is released late when destroying the
                 * GEM object so it is entirely possible to gain a
                 * reference on an object in the process of being freed
                 * since our serialisation is via the spinlock and not
                 * the struct_mutex - and consequently use it after it
                 * is freed and then double free it. To prevent that
                 * use-after-free we only acquire a reference on the
                 * object if it is not in the process of being destroyed.
                 */
                obj = container_of(it, struct i915_mmu_object, it)->obj;
                if (!kref_get_unless_zero(&obj->base.refcount)) {
                        it = interval_tree_iter_next(it, start, end);
                        continue;
                }
                spin_unlock(&mn->lock);

                ret = i915_gem_object_unbind(obj,NULL,
                                             I915_GEM_OBJECT_UNBIND_ACTIVE |
                                             I915_GEM_OBJECT_UNBIND_BARRIER);
                if (ret == 0)
                        ret = __i915_gem_object_put_pages(obj);
                i915_gem_object_put(obj);
                if (ret)
                        return;

                spin_lock(&mn->lock);

                /*
                 * As we do not (yet) protect the mmu from concurrent insertion
                 * over this range, there is no guarantee that this search will
                 * terminate given a pathologic workload.
                 */
                it = interval_tree_iter_first(&mn->objects, start, end);
        }
        spin_unlock(&mn->lock);

        return;

}

static const struct mmu_notifier_ops i915_gem_userptr_notifier = {
        .invalidate_range_start = userptr_mn_invalidate_range_start,
};

#else
static int
userptr_mn_invalidate_range_start(struct mmu_notifier *_mn,
		const struct mmu_notifier_range *range)
{
	struct i915_mmu_notifier *mn =
		container_of(_mn, struct i915_mmu_notifier, mn);
	struct mmu_notifier_subscriptions *subscriptions =
		mn->mm->notifier_subscriptions;
	int ret = 0;

	if (subscriptions->has_itree)
		ret = mn_itree_invalidate(subscriptions, range);

	return ret;
}

static void
userptr_mn_invalidate_range_end(struct mmu_notifier *_mn,
		const struct mmu_notifier_range *range)
{
	struct i915_mmu_notifier *mn =
		container_of(_mn, struct i915_mmu_notifier, mn);
	struct mmu_notifier_subscriptions *subscriptions =
		mn->mm->notifier_subscriptions;

	mn_itree_invalidate_end(subscriptions);
}

static void userptr_mn_release(struct mmu_notifier *_mn, struct mm_struct *mm)
{
	struct i915_mmu_notifier *mn =
		container_of(_mn, struct i915_mmu_notifier, mn);
	struct mmu_notifier_subscriptions *subscriptions =
		mn->mm->notifier_subscriptions;

	mn_itree_release(subscriptions, mn->mm);
}

static const struct mmu_notifier_ops i915_gem_userptr_notifier = {
	.invalidate_range_start = userptr_mn_invalidate_range_start,
	.invalidate_range_end = userptr_mn_invalidate_range_end,
	.release = userptr_mn_release,
};
#endif
#endif

static const struct mmu_interval_notifier_ops i915_gem_userptr_notifier_ops = {
	.invalidate = i915_gem_userptr_invalidate,
};

#ifdef BPM_MMU_INTERVAL_NOTIFIER_NOTIFIER_NOT_PRESENT
static struct i915_mmu_notifier *
i915_mmu_notifier_create(struct i915_mm_struct *mm)
{
	struct i915_mmu_notifier *mn;

	mn = kmalloc(sizeof(*mn), GFP_KERNEL);
	if (mn == NULL)
		return ERR_PTR(-ENOMEM);

	mn->mn.ops = &i915_gem_userptr_notifier;
	mn->mm = mm;

	return mn;
}

static struct i915_mmu_notifier *
i915_mmu_notifier_find(struct i915_mm_struct *mm)
{
	struct i915_mmu_notifier *mn, *old;
	int err;

	mn = READ_ONCE(mm->mn);
	if (likely(mn))
		return mn;

	mn = i915_mmu_notifier_create(mm);
	if (IS_ERR(mn))
		return mn;

	err = mmu_notifier_register(&mn->mn, mm->mm);
	if (err) {
		kfree(mn);
		return ERR_PTR(err);
	}

	old = cmpxchg(&mm->mn, NULL, mn);
	if (old) {
		mmu_notifier_unregister(&mn->mn, mm->mm);
		kfree(mn);
		mn = old;
	}

	return mn;
}

static int
__i915_gem_userptr_init__mmu_notifier(struct drm_i915_gem_object *obj)
{
	struct i915_mmu_notifier *mn;
	int ret;

	if (WARN_ON(obj->userptr.mm == NULL))
		return -EINVAL;

	ret = mmu_notifier_subscriptions_init(obj->userptr.mm);
	if (ret)
		return ret;

	mn = i915_mmu_notifier_find(obj->userptr.mm);
	if (IS_ERR(mn))
		return PTR_ERR(mn);

	return 0;
}

static int
i915_gem_userptr_init__mmu_notifier(struct drm_i915_gem_object *obj)
{
	int ret;

	ret = __i915_gem_userptr_init__mmu_notifier(obj);
	if (ret)
		return ret;

	return mmu_interval_notifier_insert(&obj->userptr.notifier, obj->userptr.mm,
					    obj->userptr.ptr, obj->base.size,
					    &i915_gem_userptr_notifier_ops);
}
#else
static int
i915_gem_userptr_init__mmu_notifier(struct drm_i915_gem_object *obj)
{
	return mmu_interval_notifier_insert(&obj->userptr.notifier, current->mm,
					    obj->userptr.ptr, obj->base.size,
					    &i915_gem_userptr_notifier_ops);
}
#endif

#ifdef BPM_MMU_INTERVAL_NOTIFIER_NOTIFIER_NOT_PRESENT
static struct i915_mm_struct *
__i915_mm_struct_find(struct drm_i915_private *i915, struct mm_struct *real)
{
	struct i915_mm_struct *it, *mm = NULL;

	rcu_read_lock();
	hash_for_each_possible_rcu(i915->mm_structs,
			it, node,
			(unsigned long)real)
		if (it->mm == real && kref_get_unless_zero(&it->kref)) {
			mm = it;
			break;
		}
	rcu_read_unlock();

	return mm;
}

static int
i915_gem_userptr_init__mm_struct(struct drm_i915_gem_object *obj)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct i915_mm_struct *mm, *new;
	int ret = 0;

	/* During release of the GEM object we hold the struct_mutex. This
	 * precludes us from calling mmput() at that time as that may be
	 * the last reference and so call exit_mmap(). exit_mmap() will
	 * attempt to reap the vma, and if we were holding a GTT mmap
	 * would then call drm_gem_vm_close() and attempt to reacquire
	 * the struct mutex. So in order to avoid that recursion, we have
	 * to defer releasing the mm reference until after we drop the
	 * struct_mutex, i.e. we need to schedule a worker to do the clean
	 * up.
	 */

	mm = __i915_mm_struct_find(i915, current->mm);
	if (mm)
		goto out;

	new = kzalloc(sizeof(*mm), GFP_KERNEL);
	if (!new)
		return -ENOMEM;

	kref_init(&new->kref);
	new->i915 = to_i915(obj->base.dev);
	new->mm = current->mm;

	spin_lock(&i915->mm_lock);
	mm = __i915_mm_struct_find(i915, current->mm);
	if (!mm) {
		hash_add_rcu(i915->mm_structs,
				&new->node,
				(unsigned long)new->mm);
		mmgrab(current->mm);
		mm = new;
	}
	spin_unlock(&i915->mm_lock);
	if (mm != new) {
		kfree(new);
	}

out:
	obj->userptr.mm = mm;
	return ret;
}

static void
i915_mmu_notifier_free(struct i915_mmu_notifier *mn,
			struct mm_struct *mm)
{
	if (mn == NULL)
		return;

	mmu_notifier_unregister(&mn->mn, mm);
	kfree(mn);
}

static void
__i915_mm_struct_free__worker(struct work_struct *work)
{
	struct i915_mm_struct *mm = container_of(work, typeof(*mm), work.work);

	i915_mmu_notifier_free(mm->mn, mm->mm);
	mmu_notifier_subscriptions_destroy(mm);
	mmdrop(mm->mm);
	kfree(mm);
}

static void
__i915_mm_struct_free(struct kref *kref)
{
	struct i915_mm_struct *mm = container_of(kref, typeof(*mm), kref);

	spin_lock(&mm->i915->mm_lock);
	hash_del_rcu(&mm->node);
	spin_unlock(&mm->i915->mm_lock);

	INIT_RCU_WORK(&mm->work, __i915_mm_struct_free__worker);
	queue_rcu_work(system_wq, &mm->work);
}

static void
i915_gem_userptr_release__mm_struct(struct drm_i915_gem_object *obj)
{
	if (obj->userptr.mm == NULL)
		return;

	kref_put(&obj->userptr.mm->kref, __i915_mm_struct_free);
	obj->userptr.mm = NULL;
}
#endif

static void
i915_gem_userptr_release(struct drm_i915_gem_object *obj)
{
	i915_gem_object_release_memory_region(obj);
	if (!obj->userptr.notifier.mm)
		return;

	mmu_interval_notifier_remove(&obj->userptr.notifier);
#ifdef BPM_MMU_INTERVAL_NOTIFIER_NOTIFIER_NOT_PRESENT
	i915_gem_userptr_release__mm_struct(obj);
#endif
	obj->userptr.notifier.mm = NULL;
}
#else
static int
i915_gem_userptr_init__mmu_notifier(struct drm_i915_gem_object *obj)
{
	obj->userptr.notifier.mm = current->mm;
	mmgrab(current->mm);
	return 0;
}

static void
i915_gem_userptr_release(struct drm_i915_gem_object *obj)
{
	i915_gem_object_release_memory_region(obj);
	if (!obj->userptr.notifier.mm)
		return;

	mmdrop(obj->userptr.notifier.mm);
}

#define mmu_interval_read_begin(n) 0
#define mmu_interval_read_retry(n, seq) false
#endif


struct userptr_work {
	struct dma_fence_work base;
	struct drm_i915_gem_object *obj;
	struct sg_table *pages;
};

struct userptr_chunk {
	struct work_struct work;
	struct mmu_interval_notifier *notifier;
	struct i915_sw_fence *fence;
	unsigned long addr, count;
};

static int __userptr_chunk(struct mmu_interval_notifier *notifier,
			   struct scatterlist *sg,
			   unsigned long start,
			   unsigned long max,
			   unsigned long flags)
{
	struct page *pages[MAX_STACK_ALLOC / sizeof(struct page *)];
	unsigned long count = 0;
	int err;

	/*
	 * Currently when we break out of multi-threaded mode (FOLL_FAST_ONLY)
	 * we completely replay in single-threaded mode, clearing any
	 * in-progress chunking.
	 *
	 * A possible optimization here would be to keep the chunking that has
	 * already happened to this point and only replay the pages which
	 * haven't yet been pinned. For now, take the brute force approach.
	 */

#ifdef BPM_KTHREAD_USE_MM_NOT_PRESENT
	use_mm(notifier->mm);
#else
	kthread_use_mm(notifier->mm);
#endif
	do {
		unsigned long addr = start + (count << PAGE_SHIFT);
		int n = min_t(int, max - count, ARRAY_SIZE(pages));

		err = pin_user_pages_fast(addr, n, flags, pages);
		if (err <= 0) {
			if (flags & FOLL_FAST_ONLY)
				err = -EAGAIN;
			GEM_BUG_ON(err == 0);
			goto out;
		}

		for (n = 0; n < err; n++) {
			GEM_BUG_ON(!sg || !pages[n]);
			sg_set_page(sg, pages[n], PAGE_SIZE, 0);
			sg = __sg_next(sg);
		}
		count += n;
	} while (count < max);

	err = 0;
out:
#ifdef BPM_KTHREAD_USE_MM_NOT_PRESENT
	unuse_mm(notifier->mm);
#else
	kthread_unuse_mm(notifier->mm);
#endif
	return err;
}

static void userptr_chunk(struct work_struct *wrk)
{
	struct userptr_chunk *chunk = container_of(wrk, typeof(*chunk), work);
	struct mmu_interval_notifier *notifier = chunk->notifier;
	struct i915_sw_fence *fence = chunk->fence;
	unsigned long count = chunk->count;
	unsigned long addr = chunk->addr;
	int err;

	err = __userptr_chunk(notifier,
			      memset(chunk, 0, sizeof(*chunk)),
			      addr & PAGE_MASK, count,
			      (addr & ~PAGE_MASK) | FOLL_FAST_ONLY);
	i915_sw_fence_set_error_once(fence, err);
	i915_sw_fence_complete(fence);
}

static void userptr_queue(struct userptr_chunk *chunk)
{
	if (IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_PARALLEL_USERPTR))
		queue_work(system_unbound_wq, &chunk->work);
	else
		userptr_chunk(&chunk->work);
}

static void unpin_sg(struct sg_table *sgt)
{
	struct scatterlist *sg;

	for (sg = sgt->sgl; sg; sg = __sg_next(sg)) {
		unsigned long pfn;
		struct page *page;

		page = sg_page(sg);
		if (!page)
			continue;

		for (pfn = 0; pfn < sg->length >> PAGE_SHIFT; pfn++)
#ifdef BPM_PIN_OR_UNPIN_USER_PAGE_NOT_PRESENT
			put_user_page(nth_page(page, pfn));
#else
			unpin_user_page(nth_page(page, pfn));
#endif

		sg_set_page(sg, NULL, 0, 0);
	}
}

static int userptr_work(struct dma_fence_work *base)
{
	struct userptr_work *wrk = container_of(base, typeof(*wrk), base);
	struct drm_i915_gem_object *obj = wrk->obj;
	unsigned long use_threads = FOLL_FAST_ONLY;
	struct sg_table *sgt = wrk->pages;
	struct userptr_chunk *chunk;
	struct i915_sw_fence fence;
	unsigned long seq, addr, n;
	struct scatterlist *sg;
        int err;

	addr = obj->userptr.ptr;
	if (!i915_gem_object_is_readonly(obj))
		addr |= FOLL_WRITE | FOLL_FORCE;
	BUILD_BUG_ON((FOLL_WRITE | FOLL_FORCE) & PAGE_MASK);

	if (!mmget_not_zero(obj->userptr.notifier.mm))
		return -EFAULT;

restart: /* Spread the pagefaulting across the cores (~4MiB per core) */
	err = 0;
	chunk = NULL;
	i915_sw_fence_init_onstack(&fence);
	seq = mmu_interval_read_begin(&obj->userptr.notifier);
	for (n = 0, sg = sgt->sgl; use_threads && n + SG_MAX_SINGLE_ALLOC < sgt->orig_nents;) {
		if (chunk == NULL) {
			chunk = memset(sg, 0, sizeof(*chunk));

			i915_sw_fence_await(&fence);
			chunk->fence = &fence;
			chunk->addr = addr + (n << PAGE_SHIFT);
			chunk->count = -n;
			chunk->notifier = &obj->userptr.notifier;
			INIT_WORK(&chunk->work, userptr_chunk);
		}

		sg += I915_MAX_CHAIN_ALLOC;
		GEM_BUG_ON(!sg_is_chain(sg));
		sg = sg_chain_ptr(sg);

		/* PMD-split locks (2M), try to minimise lock contention */
		n += I915_MAX_CHAIN_ALLOC;
		if (((addr + (n << PAGE_SHIFT) - 1) ^ chunk->addr) & SZ_4M) {
			chunk->count += n;
			userptr_queue(chunk);
			chunk = NULL;
		}

		if (READ_ONCE(fence.error))
			break;
	}
	i915_sw_fence_commit(&fence);

	/* Leaving the last chunk for ourselves */
	if (READ_ONCE(fence.error)) {
		/* Do nothing more if already in error */
		if (chunk) {
			memset(chunk, 0, sizeof(*chunk));
			i915_sw_fence_complete(&fence);
		}
	} else if (chunk) {
		chunk->count += sgt->orig_nents;
		userptr_chunk(&chunk->work);
	} else {
		err = __userptr_chunk(&obj->userptr.notifier, sg,
				      (addr & PAGE_MASK) + (n << PAGE_SHIFT),
				      sgt->orig_nents - n,
				      (addr & ~PAGE_MASK) | use_threads);
	}

	if (n) {
		i915_sw_fence_set_error_once(&fence, err);
		i915_sw_fence_wait(&fence);
		err = fence.error;
	}

	if (err == 0 && mmu_interval_read_retry(&obj->userptr.notifier, seq))
		err = -EAGAIN;
	i915_sw_fence_fini(&fence);
	if (err)
		goto err;

	obj->mm.page_sizes =
		i915_sg_compact(sgt, i915_gem_sg_segment_size(obj));

	if (i915_gem_object_can_bypass_llc(obj))
		drm_clflush_sg(sgt);

	err = i915_gem_gtt_prepare_pages(obj, sgt);
	if (err) {
err:		unpin_sg(sgt);

		if (err == -EAGAIN) {
			use_threads = 0;
			goto restart;
		}

		sg_mark_end(sgt->sgl);
		sgt->nents = 0;
	}

	mmput(obj->userptr.notifier.mm);
	return err;
}

static const struct dma_fence_work_ops userptr_ops = {
	.name = "userptr",
	.work = userptr_work,
};

static int
probe_range(struct mm_struct *mm, unsigned long addr, unsigned long len)
{
	const unsigned long end = addr + len;
	struct vm_area_struct *vma;
	int ret = -EFAULT;

	if (!mmap_read_trylock(mm))
		return 0;

	for (vma = find_vma(mm, addr); vma; vma = vma->vm_next) {
		if (vma->vm_start > addr)
			break;

		if (vma->vm_flags & (VM_IO | VM_PFNMAP))
			break;

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
	unsigned int num_pages; /* limited by sg_alloc_table */
	struct userptr_work *wrk;
	struct sg_table *st;
#ifdef BPM_SG_ALLOC_TABLE_FROM_PAGES_RETURNS_SCATTERLIST
	struct scatterlist *sg;
#endif
	int err;

	err = probe_range(obj->userptr.notifier.mm,
			  obj->userptr.ptr,
			  obj->base.size);
	if (err)
		return err;

	if (!safe_conversion(&num_pages, obj->base.size >> PAGE_SHIFT))
		return -E2BIG;

	st = kmalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	err = sg_alloc_table(st, num_pages, I915_GFP_ALLOW_FAIL);
	if (err)
		goto err_free;

	wrk = kmalloc(sizeof(*wrk), GFP_KERNEL);
	if (!wrk) {
		err = -ENOMEM;
		goto err_sg;
	}
	dma_fence_work_init(&wrk->base, &userptr_ops,
			    to_i915(obj->base.dev)->mm.sched);
	wrk->obj = obj;
	wrk->pages = st;

	obj->cache_dirty = false;
	__i915_gem_object_set_pages(obj, st, PAGE_SIZE); /* placeholder */
	atomic64_sub(obj->base.size, &obj->mm.region.mem->avail);

	i915_gem_object_migrate_prepare(obj, &wrk->base.rq.fence);
	dma_fence_work_commit(&wrk->base);
	return 0;

err_sg:
	sg_free_table(st);
err_free:
	kfree(st);
	return err;
}

static int
i915_gem_userptr_put_pages(struct drm_i915_gem_object *obj,
			   struct sg_table *pages)
{
	struct sgt_iter sgt_iter;
	struct pagevec pvec;
	struct page *page;
	bool dirty;

	if (!i915_gem_object_migrate_finish(obj))
		i915_gem_gtt_finish_pages(obj, pages);

	__i915_gem_object_release_shmem(obj, pages, false);

	/*
	 * We always mark objects as dirty when they are used by the GPU,
	 * just in case. However, if we set the vma as being read-only we know
	 * that the object will never have been written to.
	 */
	dirty = !i915_gem_object_is_readonly(obj);

	pagevec_init(&pvec);
	for_each_sgt_page(page, sgt_iter, pages) {
		if (!pagevec_add(&pvec, page)) {
#ifdef BPM_PIN_OR_UNPIN_USER_PAGE_NOT_PRESENT
#ifdef BPM_PUT_USER_PAGES_DIRTY_LOCK_ARG_NOT_PRESENT
			put_user_pages_dirty_lock(pvec.pages,
						  pagevec_count(&pvec));
#else
			put_user_pages_dirty_lock(pvec.pages,
						  pagevec_count(&pvec),
						  true);
#endif
#else
			unpin_user_pages_dirty_lock(pvec.pages,
						    pagevec_count(&pvec),
						    true);
#endif
			pagevec_reinit(&pvec);
		}
	}
	if (pagevec_count(&pvec))

#ifdef BPM_PIN_OR_UNPIN_USER_PAGE_NOT_PRESENT
#ifdef BPM_PUT_USER_PAGES_DIRTY_LOCK_ARG_NOT_PRESENT
		put_user_pages_dirty_lock(pvec.pages,
						  pagevec_count(&pvec));
#else
		put_user_pages_dirty_lock(pvec.pages,
					  pagevec_count(&pvec),
					  true);
#endif
#else
		unpin_user_pages_dirty_lock(pvec.pages,
					    pagevec_count(&pvec),
					    true);
#endif
	atomic64_add(obj->base.size, &obj->mm.region.mem->avail);

	sg_free_table(pages);
	kfree(pages);
	return 0;
}

static int
i915_gem_userptr_dmabuf_export(struct drm_i915_gem_object *obj)
{
	drm_dbg(obj->base.dev, "Exporting userptr no longer allowed\n");

	return -EINVAL;
}

static int
i915_gem_userptr_pwrite(struct drm_i915_gem_object *obj,
			const struct drm_i915_gem_pwrite *args)
{
	drm_dbg(obj->base.dev, "pwrite to userptr no longer allowed\n");

	return -EINVAL;
}

static int
i915_gem_userptr_pread(struct drm_i915_gem_object *obj,
		       const struct drm_i915_gem_pread *args)
{
	drm_dbg(obj->base.dev, "pread from userptr no longer allowed\n");

	return -EINVAL;
}

static const struct drm_i915_gem_object_ops i915_gem_userptr_ops = {
	.name = "i915_gem_object_userptr",
	.flags = I915_GEM_OBJECT_IS_SHRINKABLE |
		 I915_GEM_OBJECT_NO_MMAP,
	.get_pages = i915_gem_userptr_get_pages,
	.put_pages = i915_gem_userptr_put_pages,
	.dmabuf_export = i915_gem_userptr_dmabuf_export,
	.pwrite = i915_gem_userptr_pwrite,
	.pread = i915_gem_userptr_pread,
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
	static struct lock_class_key lock_class;
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

	i915_gem_flush_free_objects(i915);

	obj = i915_gem_object_alloc();
	if (obj == NULL)
		return -ENOMEM;

	drm_gem_private_object_init(dev, &obj->base, args->user_size);
	i915_gem_object_init(obj, &i915_gem_userptr_ops, &lock_class,
			     I915_BO_STRUCT_PAGE |
			     I915_BO_ALLOC_USER);
	obj->read_domains = I915_GEM_DOMAIN_CPU;
	obj->write_domain = I915_GEM_DOMAIN_CPU;
	i915_gem_object_set_cache_coherency(obj, I915_CACHE_LLC);

	obj->userptr.ptr = args->user_ptr;
	if (args->flags & I915_USERPTR_READ_ONLY)
		i915_gem_object_set_readonly(obj);

	i915_gem_object_init_memory_region(obj,
					   i915->mm.regions[INTEL_REGION_SMEM]);

	/* And keep a pointer to the current->mm for resolving the user pages
	 * at binding. This means that we need to hook into the mmu_notifier
	 * in order to detect if the mmu is destroyed.
	 */
#ifdef BPM_MMU_INTERVAL_NOTIFIER_NOTIFIER_NOT_PRESENT
	ret = i915_gem_userptr_init__mm_struct(obj);
	if (ret == 0)
		ret = i915_gem_userptr_init__mmu_notifier(obj);
#else
	ret = i915_gem_userptr_init__mmu_notifier(obj);
#endif
	if (ret == 0)
		ret = drm_gem_handle_create(file, &obj->base, &handle);

	/* drop reference from allocate - handle holds it now */
	i915_gem_object_put(obj);
	if (ret)
		return ret;

	args->handle = handle;
	return 0;
}
