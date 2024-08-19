/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2008-2015 Intel Corporation
 */

#include <linux/oom.h>
#include <linux/sched/mm.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/pci.h>
#include <linux/dma-buf.h>
#include <linux/vmalloc.h>

#include "gt/intel_gt.h"
#include "gt/intel_gt_requests.h"

#include "dma_resv_utils.h"
#include "i915_gem_shmem.h"
#include "i915_trace.h"

static bool swap_available(void)
{
	return get_nr_swap_pages() > 0;
}

static bool can_release_pages(struct drm_i915_gem_object *obj)
{
	/*
	 * We can only return physical pages to the system if we can either
	 * discard the contents (because the user has marked them as being
	 * purgeable) or if we can move their contents out to swap.
	 */
	return swap_available() || i915_gem_object_is_purgeable(obj);
}

static bool drop_pages(struct drm_i915_gem_object *obj,
		      unsigned long shrink)
{
	unsigned long flags;
	int err;

	flags = 0;
	if (!(shrink & I915_SHRINK_BOUND))
		flags |= I915_GEM_OBJECT_UNBIND_TEST;

	err = i915_gem_object_unbind(obj, NULL, flags);
	if (err == 0)
		err = __i915_gem_object_put_pages(obj);

	return err == 0;
}

/**
 * i915_gem_shrink - Shrink buffer object caches
 * @i915: i915 device
 * @target: amount of memory to make available, in pages
 * @nr_scanned: optional output for number of pages scanned (incremental)
 * @shrink: control flags for selecting cache types
 *
 * This function is the main interface to the shrinker. It will try to release
 * up to @target pages of main memory backing storage from buffer objects.
 * Selection of the specific caches can be done with @flags. This is e.g. useful
 * when purgeable objects should be removed from caches preferentially.
 *
 * Note that it's not guaranteed that released amount is actually available as
 * free system memory - the pages might still be in-used to due to other reasons
 * (like cpu mmaps) or the mm core has reused them before we could grab them.
 * Therefore code that needs to explicitly shrink buffer objects caches (e.g. to
 * avoid deadlocks in memory reclaim) must fall back to i915_gem_shrink_all().
 *
 * Also note that any kind of pinning (both per-vma address space pins and
 * backing storage pins at the buffer object level) result in the shrinker code
 * having to skip the object.
 *
 * Returns:
 * The number of pages of backing storage actually released.
 */
unsigned long
i915_gem_shrink(struct drm_i915_private *i915,
		unsigned long target,
		unsigned long *nr_scanned,
		unsigned int shrink)
{
	struct intel_memory_region *mem = i915->mm.regions[INTEL_REGION_SMEM];
	struct list_head *phases[] = {
		&mem->objects.migratable,
		&mem->objects.list,
		NULL,
	}, **phase;
	intel_wakeref_t wakeref = 0;
	unsigned long count = 0;
	unsigned long scanned = 0;

	trace_i915_gem_shrink(i915, target, shrink);

	count = i915_gem_reap_clear_smem(mem, 0, min(target, -2ul)); /* -1ul => wait */
	if (count)
		return count;

	/*
	 * Unbinding of objects will require HW access; Let us not wake the
	 * device just to recover a little memory. If absolutely necessary,
	 * we will force the wake during oom-notifier.
	 */
	if (shrink & I915_SHRINK_BOUND) {
		wakeref = intel_runtime_pm_get_if_in_use(&i915->runtime_pm);
		if (!wakeref)
			shrink &= ~I915_SHRINK_BOUND;
	}

	/*
	 * When shrinking the active list, we should also consider active
	 * contexts. Active contexts are pinned until they are retired, and
	 * so can not be simply unbound to retire and unpin their pages. To
	 * shrink the contexts, we must wait until the gpu is idle and
	 * completed its switch to the kernel context. In short, we do
	 * not have a good mechanism for idling a specific context, but
	 * what we can do is give them a kick so that we do not keep idle
	 * contexts around longer than is necessary.
	 */
	if (shrink & I915_SHRINK_ACTIVE) {
		struct intel_gt *gt;
		int id;

		/* Retire requests to unpin all idle contexts */
		for_each_gt(gt, i915, id)
			intel_gt_retire_requests(gt);
	}

	/*
	 * As we may completely rewrite the (un)bound list whilst unbinding
	 * (due to retiring requests) we have to strictly process only
	 * one element of the list at the time, and recheck the list
	 * on every iteration.
	 *
	 * In particular, we must hold a reference whilst removing the
	 * object as we may end up waiting for and/or retiring the objects.
	 * This might release the final reference (held by the active list)
	 * and result in the object being freed from under us. This is
	 * similar to the precautions the eviction code must take whilst
	 * removing objects.
	 *
	 * Also note that although these lists do not hold a reference to
	 * the object we can safely grab one here: The final object
	 * unreferencing and the bound_list are both protected by the
	 * dev->struct_mutex and so we won't ever be able to observe an
	 * object on the bound_list with a reference count equals 0.
	 */
	for (phase = phases; count < target && *phase; phase++) {
		struct intel_memory_region_link bookmark = {};
		struct intel_memory_region_link end = {};
		struct intel_memory_region_link *pos;
		long timeout = 0;
		bool keepalive;

		if (list_empty(*phase))
			continue;

		keepalive = true;
		spin_lock_irq(&mem->objects.lock);
		list_add_tail(&end.link, *phase);
		list_for_each_entry(pos, *phase, link) {
			struct drm_i915_gem_object *obj;

			if (unlikely(signal_pending(current)))
				break;

			if (unlikely(!pos->mem)) { /* skip over other bookmarks */
				if (pos == &end) {
					timeout = 0;
					if (shrink & I915_SHRINK_ACTIVE)
						timeout = msecs_to_jiffies(CPTCFG_DRM_I915_FENCE_TIMEOUT);
					keepalive = false;
				}
				continue;
			}

			/* only segment BOs should be in mem->objects.list */
			obj = container_of(pos, typeof(*obj), mm.region);
			GEM_BUG_ON(i915_gem_object_has_segments(obj));

			if (dma_resv_is_locked(obj->base.resv))
				continue;

			if (shrink & I915_SHRINK_VMAPS &&
			    !is_vmalloc_addr(obj->mm.mapping))
				continue;

			if (!(shrink & I915_SHRINK_ACTIVE)) {
				if (i915_gem_object_is_framebuffer(obj))
					continue;

				if (!can_release_pages(obj))
					continue;
			}

			list_replace(&pos->link, &bookmark.link);
			if (keepalive) {
				if (!(obj->flags & I915_BO_ALLOC_USER)) {
					list_add_tail(&pos->link, *phase);
					goto delete_bookmark;
				}

				if (!i915_gem_object_is_purgeable(obj)) {
					if (2 * obj->base.size < (target - count) ||
					    obj->base.size > 2 * (target - count)) {
						list_add(&pos->link, &end.link);
						goto delete_bookmark;
					}
				}

				if (i915_gem_object_is_active(obj)) {
					list_add(&pos->link, &end.link);
					goto delete_bookmark;
				}
			}
			INIT_LIST_HEAD(&pos->link);

			if (!i915_gem_object_get_rcu(obj))
				goto delete_bookmark;

			spin_unlock_irq(&mem->objects.lock);

			/* Flush activity prior to grabbing locks */
			timeout = __i915_gem_object_wait(obj,
							 I915_WAIT_INTERRUPTIBLE |
							 I915_WAIT_PRIORITY |
							 I915_WAIT_ALL,
							 timeout);
			if (timeout < 0) {
				timeout = 0;
				goto relock;
			}

			/* May arrive from get_pages on another bo */
			if (!i915_gem_object_trylock(obj))
				goto relock;

			if (!i915_gem_object_has_pages(obj))
				goto unlock;

			i915_gem_object_move_notify(obj);

			scanned += obj->base.size >> PAGE_SHIFT;
			if (drop_pages(obj, shrink))
				count += obj->base.size >> PAGE_SHIFT;

unlock:
			i915_gem_object_unlock(obj);
relock:
			cond_resched();
			spin_lock_irq(&mem->objects.lock);
			if (i915_gem_object_has_pages(obj) && list_empty(&pos->link))
				list_add_tail(&pos->link, &bookmark.link);

			i915_gem_object_put(obj);
delete_bookmark:
			__list_del_entry(&bookmark.link);
			if (count >= target)
				break;

			pos = &bookmark;
		}
		__list_del_entry(&end.link);
		spin_unlock_irq(&mem->objects.lock);
	}

	if (shrink & I915_SHRINK_BOUND)
		intel_runtime_pm_put(&i915->runtime_pm, wakeref);

	if (nr_scanned)
		*nr_scanned += scanned;
	return count;
}

/**
 * i915_gem_shrink_all - Shrink buffer object caches completely
 * @i915: i915 device
 *
 * This is a simple wraper around i915_gem_shrink() to aggressively shrink all
 * caches completely. It also first waits for and retires all outstanding
 * requests to also be able to release backing storage for active objects.
 *
 * This should only be used in code to intentionally quiescent the gpu or as a
 * last-ditch effort when memory seems to have run out.
 *
 * Returns:
 * The number of pages of backing storage actually released.
 */
unsigned long i915_gem_shrink_all(struct drm_i915_private *i915)
{
	intel_wakeref_t wakeref;
	unsigned long freed = 0;

	with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
		freed = i915_gem_shrink(i915, -1UL, NULL,
					I915_SHRINK_BOUND |
					I915_SHRINK_UNBOUND |
					I915_SHRINK_ACTIVE);
	}

	return freed;
}

static unsigned long
i915_gem_shrinker_count(struct shrinker *shrinker, struct shrink_control *sc)
{
#ifdef BPM_REGISTER_SHRINKER_NOT_PRESENT
	struct drm_i915_private *i915 = shrinker->private_data;
#else
	struct drm_i915_private *i915 =
		container_of(shrinker, struct drm_i915_private, mm.shrinker);
#endif
	//struct intel_memory_region *mem = i915->mm.regions[INTEL_REGION_SMEM];
	unsigned long num_objects;
	unsigned long count;

	count = 1;
	num_objects = 1;

	/*
	 * Update our preferred vmscan batch size for the next pass.
	 * Our rough guess for an effective batch size is roughly 2
	 * available GEM objects worth of pages. That is we don't want
	 * the shrinker to fire, until it is worth the cost of freeing an
	 * entire GEM object.
	 */
	if (num_objects) {
		unsigned long avg = 2 * count / num_objects;

#ifdef BPM_REGISTER_SHRINKER_NOT_PRESENT
	i915->mm.shrinker->batch =
		max((i915->mm.shrinker->batch + avg) >> 1,
#else
		i915->mm.shrinker.batch =
			max((i915->mm.shrinker.batch + avg) >> 1,
#endif
			    128ul /* default SHRINK_BATCH */);
	}

	return count;
}

static unsigned long run_swapper(struct drm_i915_private *i915,
				 unsigned long target,
				 unsigned long *nr_scanned)
{
	unsigned long found = 0;

	found += i915_gem_shrink(i915, target, nr_scanned,
				 I915_SHRINK_BOUND |
				 I915_SHRINK_UNBOUND |
				 I915_SHRINK_WRITEBACK);
	if (found < target)
		found += i915_gem_shrink(i915, target, nr_scanned,
					 I915_SHRINK_ACTIVE |
					 I915_SHRINK_BOUND |
					 I915_SHRINK_UNBOUND |
					 I915_SHRINK_WRITEBACK);

	return found;
}

static int swapper(void *arg)
{
	struct drm_i915_private *i915 = arg;
	atomic_long_t *target = &i915->mm.swapper.target;
	unsigned int noreclaim_state;

	/*
	 * For us to be running the swapper implies that the system is under
	 * enough memory pressure to be swapping. At that point, we both want
	 * to ensure we make forward progress in order to reclaim pages from
	 * the device and not contribute further to direct reclaim pressure. We
	 * mark ourselves as a memalloc task in order to not trigger direct
	 * reclaim ourselves, but dip into the system memory reserves for
	 * shrinkers.
	 */
	noreclaim_state = memalloc_noreclaim_save();

	do {
		intel_wakeref_t wakeref;

		___wait_var_event(target,
				  atomic_long_read(target) ||
				  kthread_should_stop(),
				  TASK_IDLE, 0, 0, schedule());
		if (kthread_should_stop())
			break;

		with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
			unsigned long nr_scan = atomic_long_xchg(target, 0);

			/*
			 * Now that we have woken up the device hierarchy,
			 * act as a normal shrinker. Our shrinker is primarily
			 * focussed on supporting direct reclaim (low latency,
			 * avoiding contention that may lead to more reclaim,
			 * or prevent that reclaim from making forward progress)
			 * and we wish to continue that good practice even
			 * here where we could accidentally sleep holding locks.
			 *
			 * Let lockdep know and warn us about any bad practice
			 * that may lead to high latency in direct reclaim, or
			 * anywhere else.
			 *
			 * While the swapper is active, direct reclaim from
			 * other threads will also be running in parallel
			 * through i915_gem_shrink(), scouring for idle pages.
			 */
			fs_reclaim_acquire(GFP_KERNEL);
			run_swapper(i915, nr_scan, &nr_scan);
			fs_reclaim_release(GFP_KERNEL);
		}
	} while (1);

	memalloc_noreclaim_restore(noreclaim_state);
	return 0;
}

static void start_swapper(struct drm_i915_private *i915)
{
	i915->mm.swapper.tsk = kthread_run(swapper, i915, "i915-swapd");
	if (IS_ERR(i915->mm.swapper.tsk))
		drm_err(&i915->drm,
			"Failed to launch swapper; memory reclaim may be degraded\n");
}

static unsigned long kick_swapper(struct drm_i915_private *i915,
				  unsigned long nr_scan,
				  unsigned long *scanned)
{
	/* Run immediately under kswap if disabled */
	if (IS_ERR_OR_NULL(i915->mm.swapper.tsk))
		/*
		 * Note that as we are still inside kswapd, we are still
		 * inside a fs_reclaim context and cannot forcibly wake the
		 * device and so can only opportunitiscally reclaim bound
		 * memory.
		 */
		return run_swapper(i915, nr_scan, scanned);

	if (!atomic_long_fetch_add(nr_scan, &i915->mm.swapper.target))
		wake_up_var(&i915->mm.swapper.target);

	return 0;
}

static void stop_swapper(struct drm_i915_private *i915)
{
	struct task_struct *tsk = fetch_and_zero(&i915->mm.swapper.tsk);

	if (IS_ERR_OR_NULL(tsk))
		return;

	kthread_stop(tsk);
}

static unsigned long
i915_gem_shrinker_scan(struct shrinker *shrinker, struct shrink_control *sc)
{
#ifdef BPM_REGISTER_SHRINKER_NOT_PRESENT
	struct drm_i915_private *i915 = shrinker->private_data;
#else
	struct drm_i915_private *i915 =
		container_of(shrinker, struct drm_i915_private, mm.shrinker);
#endif
	unsigned long freed;

	sc->nr_scanned = 0;
	freed = i915_gem_shrink(i915,
				sc->nr_to_scan,
				&sc->nr_scanned,
				I915_SHRINK_BOUND |
				I915_SHRINK_UNBOUND);
	if (!sc->nr_scanned) /* nothing left to reclaim */
		return SHRINK_STOP;

	/* Pages still bound and system is failing with direct reclaim? */
	if (sc->nr_scanned < sc->nr_to_scan && current_is_kswapd())
		/* Defer high latency tasks to a background thread. */
		freed += kick_swapper(i915,
				      sc->nr_to_scan - sc->nr_scanned,
				      &sc->nr_scanned);

	return freed;
}

static int
i915_gem_shrinker_oom(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct drm_i915_private *i915 =
		container_of(nb, struct drm_i915_private, mm.oom_notifier);
	unsigned long freed_pages;
	intel_wakeref_t wakeref;

	freed_pages = 0;
	with_intel_runtime_pm(&i915->runtime_pm, wakeref)
		freed_pages += i915_gem_shrink(i915, -1UL, NULL,
					       I915_SHRINK_BOUND |
					       I915_SHRINK_UNBOUND |
					       I915_SHRINK_WRITEBACK);

	*(unsigned long *)ptr += freed_pages;
	return NOTIFY_DONE;
}

static int
i915_gem_shrinker_vmap(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct drm_i915_private *i915 =
		container_of(nb, struct drm_i915_private, mm.vmap_notifier);
	struct i915_vma *vma, *next;
	unsigned long freed_pages = 0;
	intel_wakeref_t wakeref;

	with_intel_runtime_pm(&i915->runtime_pm, wakeref)
		freed_pages += i915_gem_shrink(i915, -1UL, NULL,
					       I915_SHRINK_BOUND |
					       I915_SHRINK_UNBOUND |
					       I915_SHRINK_VMAPS);

	/* We also want to clear any cached iomaps as they wrap vmap */
	mutex_lock(&to_gt(i915)->ggtt->vm.mutex);
	list_for_each_entry_safe(vma, next,
				 &to_gt(i915)->ggtt->vm.bound_list, vm_link) {
		unsigned long count = i915_vma_size(vma) >> PAGE_SHIFT;

		if (!vma->iomap || i915_vma_is_active(vma))
			continue;

		if (__i915_vma_unbind(vma) == 0)
			freed_pages += count;
	}
	mutex_unlock(&to_gt(i915)->ggtt->vm.mutex);

	*(unsigned long *)ptr += freed_pages;
	return NOTIFY_DONE;
}

void i915_gem_driver_register__shrinker(struct drm_i915_private *i915)
{
#ifdef BPM_REGISTER_SHRINKER_NOT_PRESENT
	i915->mm.shrinker = shrinker_alloc(0, "drm-i915_gem");
	if (!i915->mm.shrinker) {
		drm_WARN_ON(&i915->drm, 1);
	} else {
		i915->mm.shrinker->scan_objects = i915_gem_shrinker_scan;
		i915->mm.shrinker->count_objects = i915_gem_shrinker_count;
		i915->mm.shrinker->batch = 4096;
		i915->mm.shrinker->private_data = i915;
		shrinker_register(i915->mm.shrinker);
	}
#else
	i915->mm.shrinker.scan_objects = i915_gem_shrinker_scan;
	i915->mm.shrinker.count_objects = i915_gem_shrinker_count;
	i915->mm.shrinker.seeks = DEFAULT_SEEKS;
	i915->mm.shrinker.batch = 4096;
	drm_WARN_ON(&i915->drm, register_shrinker(&i915->mm.shrinker));
#endif

	i915->mm.oom_notifier.notifier_call = i915_gem_shrinker_oom;
	drm_WARN_ON(&i915->drm, register_oom_notifier(&i915->mm.oom_notifier));

	i915->mm.vmap_notifier.notifier_call = i915_gem_shrinker_vmap;
	drm_WARN_ON(&i915->drm,
		    register_vmap_purge_notifier(&i915->mm.vmap_notifier));

	start_swapper(i915);
}

void i915_gem_driver_unregister__shrinker(struct drm_i915_private *i915)
{
	stop_swapper(i915);

	drm_WARN_ON(&i915->drm,
		    unregister_vmap_purge_notifier(&i915->mm.vmap_notifier));
	drm_WARN_ON(&i915->drm,
		    unregister_oom_notifier(&i915->mm.oom_notifier));
#ifdef BPM_REGISTER_SHRINKER_NOT_PRESENT
	shrinker_free(i915->mm.shrinker);
#else
	unregister_shrinker(&i915->mm.shrinker);
#endif
}
