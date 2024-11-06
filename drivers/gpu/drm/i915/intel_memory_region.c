// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <linux/prandom.h>

#include <uapi/drm/i915_drm.h>

#include "gem/i915_gem_object.h"
#include "gt/intel_gt_clock_utils.h"
#include "gt/intel_gt_pm.h"
#include "gt/intel_gt_requests.h"

#include "i915_buddy.h"
#include "intel_memory_region.h"
#include "i915_drv.h"

static const struct {
	u16 class;
	u16 instance;
} intel_region_map[] = {
	[INTEL_REGION_SMEM] = {
		.class = INTEL_MEMORY_SYSTEM,
		.instance = 0,
	},
	[INTEL_REGION_LMEM_0] = {
		.class = INTEL_MEMORY_LOCAL,
		.instance = 0,
	},
	[INTEL_REGION_LMEM_1] = {
		.class = INTEL_MEMORY_LOCAL,
		.instance = 1,
	},
	[INTEL_REGION_STOLEN] = {
		.class = INTEL_MEMORY_STOLEN,
		.instance = 0,
	},
};

static int __iopagetest(struct intel_memory_region *mem,
			u8 __iomem *va, int pagesize,
			u8 value, resource_size_t offset,
			const void *caller)
{
	int byte = prandom_u32_max(pagesize);
	u8 result[3];

	memset_io(va, value, pagesize); /* or GPF! */
	wmb();

	result[0] = ioread8(va);
	result[1] = ioread8(va + byte);
	result[2] = ioread8(va + pagesize - 1);
	if (memchr_inv(result, value, sizeof(result))) {
		dev_err(mem->i915->drm.dev,
			"Failed to read back from memory region:%pR at [%pa + %pa] for %ps; wrote %x, read (%x, %x, %x)\n",
			&mem->region, &mem->io_start, &offset, caller,
			value, result[0], result[1], result[2]);
		return -EINVAL;
	}

	return 0;
}

static int iopagetest(struct intel_memory_region *mem,
		      resource_size_t offset,
		      const void *caller)
{
	const u8 val[] = { 0x0, 0xa5, 0xc3, 0xf0 };
	void __iomem *va;
	int err;
	int i;

	va = ioremap_wc(mem->io_start + offset, PAGE_SIZE);
	if (!va) {
		dev_err(mem->i915->drm.dev,
			"Failed to ioremap memory region [%pa + %pa] for %ps\n",
			&mem->io_start, &offset, caller);
		return -EFAULT;
	}

	for (i = 0; i < ARRAY_SIZE(val); i++) {
		err = __iopagetest(mem, va, PAGE_SIZE, val[i], offset, caller);
		if (err)
			break;

		err = __iopagetest(mem, va, PAGE_SIZE, ~val[i], offset, caller);
		if (err)
			break;
	}

	iounmap(va);
	return err;
}

static resource_size_t random_page(resource_size_t last)
{
	/* Limited to low 44b (16TiB), but should suffice for a spot check */
	return prandom_u32_max(last >> PAGE_SHIFT) << PAGE_SHIFT;
}

static int iomemtest(struct intel_memory_region *mem,
		     bool test_all,
		     const void *caller)
{
	resource_size_t last, page;
	int err;

	last = min(mem->io_size, mem->total);
	if (last < PAGE_SIZE)
		return 0;

	last -= PAGE_SIZE;

	/*
	 * Quick test to check read/write access to the iomap (backing store).
	 *
	 * Write a byte, read it back. If the iomapping fails, we expect
	 * a GPF preventing further execution. If the backing store does not
	 * exist, the read back will return garbage. We check a couple of pages,
	 * the first and last of the specified region to confirm the backing
	 * store + iomap does cover the entire memory region; and we check
	 * a random offset within as a quick spot check for bad memory.
	 */

	if (test_all) {
		for (page = 0; page <= last; page += PAGE_SIZE) {
			err = iopagetest(mem, page, caller);
			if (err)
				return err;
		}
	} else {
		err = iopagetest(mem, 0, caller);
		if (err)
			return err;

		err = iopagetest(mem, last, caller);
		if (err)
			return err;

		err = iopagetest(mem, random_page(last), caller);
		if (err)
			return err;
	}

	return 0;
}

struct intel_memory_region *
intel_memory_region_lookup(struct drm_i915_private *i915,
			   u16 class, u16 instance)
{
	struct intel_memory_region *mr;
	int id;

	/* XXX: consider maybe converting to an rb tree at some point */
	for_each_memory_region(mr, i915, id) {
		if (mr->type == class && mr->instance == instance)
			return mr;
	}

	return NULL;
}

static void
intel_memory_region_free_pages(struct intel_memory_region *mem,
			       struct list_head *blocks,
			       bool dirty)
{
	struct i915_buddy_block *block, *on;
	u64 avail = 0;

	list_for_each_entry_safe(block, on, blocks, link) {
		avail += i915_buddy_block_size(&mem->mm, block);
		if (dirty && __i915_buddy_block_is_clear(block))
			i915_buddy_block_set_clear(block, false);
		i915_buddy_mark_free(&mem->mm, block);
	}
	INIT_LIST_HEAD(blocks);

	atomic64_add(avail, &mem->avail);
}

void
__intel_memory_region_put_pages_buddy(struct intel_memory_region *mem,
				      struct list_head *blocks,
				      bool dirty)
{
	if (unlikely(list_empty(blocks)))
		return;

	intel_memory_region_free_pages(mem, blocks, dirty);
}

static int size_index(unsigned long sz)
{
	if (sz >> 30)
		return 3;

	if (sz >> 20)
		return 2;

	if (sz >> 16)
		return 1;

	return 0;
}

static void show_xfer(struct intel_gt *gt,
		      const char *name,
		      u64 bytes,
		      u64 time,
		      struct drm_printer *p,
		      int indent)
{
	time = intel_gt_clock_interval_to_ns(gt, time);
	if (!time)
		return;

	i_printf(p, indent, "%-16s %llu MiB in %llums, %llu MiB/s\n",
		 name,
		 bytes >> 20,
		 div_u64(time, NSEC_PER_MSEC),
		 div64_u64(mul_u64_u32_shr(bytes, NSEC_PER_SEC, 20), time));
}

void intel_memory_region_print(struct intel_memory_region *mem,
			       resource_size_t target,
			       struct drm_printer *p,
			       int indent)
{
	const struct {
		const char *name;
		struct list_head *list;
	} objects[] = {
		{ "migratable", &mem->objects.migratable },
		{ "evictable", &mem->objects.list },
		{ "internal", &mem->objects.pt },
		{}
	}, *o;
	resource_size_t x;
	char bytes[16];
	char buf[256];
	int i;

	i_printf(p, indent, "name: %s\n", mem->name);
	i_printf(p, indent, "uAPI: { class: %d, instance: %d }\n", mem->type, mem->instance);

	if (mem->mm.chunk_size)
		i_printf(p, indent, "chunk:  %pa\n", &mem->mm.chunk_size);
	i_printf(p, indent, "page:   %pa\n", &mem->min_page_size);
	string_get_size(mem->total, 1, STRING_UNITS_2, bytes, sizeof(bytes));
	i_printf(p, indent, "total:  %pa [%s]\n", &mem->total, bytes);

	x = atomic64_read(&mem->avail);
	string_get_size(x, 1, STRING_UNITS_2, bytes, sizeof(bytes));
	i_printf(p, indent, "avail:  %pa [%s]\n", &x, bytes);

	x = atomic64_read(&mem->evict);
	if (x) {
		string_get_size(x, 1, STRING_UNITS_2, bytes, sizeof(bytes));
		i_printf(p, indent, "evict:  %pa [%s]\n", &x, bytes);
	}

	if (target) {
		string_get_size(target, 1, STRING_UNITS_2, bytes, sizeof(bytes));
		i_printf(p, indent, "target: %pa [%s]\n", &target, bytes);
	}

	if (test_bit(INTEL_MEMORY_CLEAR_FREE, &mem->flags))
		i_printf(p, indent, "clear-on-free: %s\n", str_enabled_disabled(test_bit(INTEL_MEMORY_CLEAR_FREE, &mem->flags)));
	if (!completion_done(&mem->parking))
		i_printf(p, indent, "clear-on-idle: %s\n", str_yes_no(!completion_done(&mem->parking)));

	for (o = objects; o->name; o++) {
		resource_size_t active = 0, dmabuf = 0, pinned = 0, avail = 0;
		struct drm_i915_gem_object *obj;
		unsigned long sizes[4] = {};
		unsigned long bookmark = 0;
		unsigned long locked = 0;
		unsigned long empty = 0;
		unsigned long count = 0;
		int len;

		spin_lock_irq(&mem->objects.lock);
		list_for_each_entry(obj, o->list, mm.region.link) {
			if (!obj->mm.region.mem) {
				bookmark++;
				continue;
			}

			if (!i915_gem_object_has_pages(obj)) {
				empty++;
				continue;
			}

			if (i915_gem_object_is_active(obj))
				active += obj->base.size;
			else if (dma_resv_is_locked(obj->base.resv))
				locked += obj->base.size;
			else if (obj->base.dma_buf && !obj->base.import_attach)
				dmabuf += obj->base.size;
			else if (i915_gem_object_has_pinned_pages(obj))
				pinned += obj->base.size;
			else
				avail += obj->base.size;

			sizes[size_index(sg_page_sizes(obj->mm.pages))]++;
			count++;
		}
		spin_unlock_irq(&mem->objects.lock);
		if (!count)
			continue;

		len = 0;
		buf[0] = '\0';
		if (bookmark)
			len += snprintf(buf + len, sizeof(buf) - len, "bookmark:%lu, ", bookmark);
		if (empty)
			len += snprintf(buf + len, sizeof(buf) - len, "empty:%lu, ", empty);
		if (active) {
			string_get_size(active, 1, STRING_UNITS_2, bytes, sizeof(bytes));
			len += snprintf(buf + len, sizeof(buf) - len, "active:%s, ", bytes);
		}
		if (locked) {
			string_get_size(locked, 1, STRING_UNITS_2, bytes, sizeof(bytes));
			len += snprintf(buf + len, sizeof(buf) - len, "locked:%s, ", bytes);
		}
		if (dmabuf) {
			string_get_size(dmabuf, 1, STRING_UNITS_2, bytes, sizeof(bytes));
			len += snprintf(buf + len, sizeof(buf) - len, "dmabuf:%s, ", bytes);
		}
		if (pinned) {
			string_get_size(pinned, 1, STRING_UNITS_2, bytes, sizeof(bytes));
			len += snprintf(buf + len, sizeof(buf) - len, "pinned:%s, ", bytes);
		}
		if (avail) {
			string_get_size(avail, 1, STRING_UNITS_2, bytes, sizeof(bytes));
			len += snprintf(buf + len, sizeof(buf) - len, "avail:%s, ", bytes);
		}

		i_printf(p, indent, "%s: { count:%lu, %ssizes:[%lu, %lu, %lu, %lu] }\n",
			 o->name, count, buf,
			 sizes[0], sizes[1], sizes[2], sizes[3]);
	}

	if (mem->mm.size) {
		struct intel_gt *gt = mem->gt;

		if (gt->counters.map) {
			intel_wakeref_t wf;

			i_printf(p, indent, "offload:\n");
			with_intel_gt_pm_if_awake(gt, wf) {
				show_xfer(gt, "clear-on-alloc:",
					  gt->counters.map[INTEL_GT_CLEAR_ALLOC_BYTES],
					  gt->counters.map[INTEL_GT_CLEAR_ALLOC_CYCLES],
					  p, indent + 2);
				show_xfer(gt, "clear-on-free:",
					  gt->counters.map[INTEL_GT_CLEAR_FREE_BYTES],
					  gt->counters.map[INTEL_GT_CLEAR_FREE_CYCLES],
					  p, indent + 2);
				show_xfer(gt, "clear-on-idle:",
					  gt->counters.map[INTEL_GT_CLEAR_IDLE_BYTES],
					  gt->counters.map[INTEL_GT_CLEAR_IDLE_CYCLES],
					  p, indent + 2);
				show_xfer(gt, "swap-in:",
					  gt->counters.map[INTEL_GT_SWAPIN_BYTES],
					  gt->counters.map[INTEL_GT_SWAPIN_CYCLES],
					  p, indent + 2);
				show_xfer(gt, "swap-out:",
					  gt->counters.map[INTEL_GT_SWAPOUT_BYTES],
					  gt->counters.map[INTEL_GT_SWAPOUT_CYCLES],
					  p, indent + 2);
				show_xfer(gt, "copy:",
					  gt->counters.map[INTEL_GT_COPY_BYTES],
					  gt->counters.map[INTEL_GT_COPY_CYCLES],
					  p, indent + 2);
			}
		}

		i_printf(p, indent, "free:\n");
		for (i = 0; i <= mem->mm.max_order; i++) {
			const u64 sz = mem->mm.chunk_size << i;
			struct i915_buddy_block *block;
			struct i915_buddy_list *bl;
			u64 clear, dirty, active;
			bool defrag = false;
			long count = 0;
			int len;

			dirty = 0;
			bl = &mem->mm.dirty_list[i];
			spin_lock(&bl->lock);
			list_for_each_entry(block, &bl->list, link) {
				if (!block->node.list)
					continue;

				dirty += sz;
				count++;
			}
			defrag |= bl->defrag;
			spin_unlock(&bl->lock);

			clear = 0;
			active = 0;
			bl = &mem->mm.clear_list[i];
			spin_lock(&bl->lock);
			list_for_each_entry(block, &bl->list, link) {
				if (!block->node.list)
					continue;

				if (i915_buddy_block_is_active(block))
					active += sz;
				else if (i915_buddy_block_is_clear(block))
					clear += sz;
				else
					dirty += sz;
				count++;
			}
			defrag |= bl->defrag;
			spin_unlock(&bl->lock);

			if (!count)
				continue;

			len = 0;
			buf[0] = '\0';
			if (active) {
				string_get_size(active, 1, STRING_UNITS_2, bytes, sizeof(bytes));
				len += snprintf(buf + len, sizeof(buf) - len, "active: %s, ", bytes);
			}
			if (clear) {
				string_get_size(clear, 1, STRING_UNITS_2, bytes, sizeof(bytes));
				len += snprintf(buf + len, sizeof(buf) - len, "clear: %s, ", bytes);
			}
			if (dirty) {
				string_get_size(dirty, 1, STRING_UNITS_2, bytes, sizeof(bytes));
				len += snprintf(buf + len, sizeof(buf) - len, "dirty: %s, ", bytes);
			}
			if (defrag)
				len += snprintf(buf + len, sizeof(buf) - len, "defrag, ");
			if (len > 0)
				buf[len - 2] = '\0';
			i_printf(p, indent + 2, "- [%d]: { count:%lu, %s }\n", i, count, buf);
		}
	}

	if (mem->ops->show)
		mem->ops->show(mem, p, indent);
}

static bool smem_allow_eviction(struct drm_i915_gem_object *obj)
{
	if (obj->memory_mask != REGION_SMEM)
		return true;

	if (obj->mempol != I915_GEM_CREATE_MPOL_BIND)
		return true;

	return bitmap_weight(get_obj_nodes(obj), obj->maxnode) > 1;
}

static bool i915_gem_object_allows_eviction(struct drm_i915_gem_object *obj)
{
	/* Only evict user lmem only objects if overcommit is enabled */
	if (!(obj->flags & I915_BO_ALLOC_USER))
		return true;

	if (obj->memory_mask & REGION_SMEM)
		return smem_allow_eviction(obj);

	return i915_allows_overcommit(to_i915(obj->base.dev));
}

int intel_memory_region_evict(struct intel_memory_region *mem,
			      struct i915_gem_ww_ctx *ww,
			      resource_size_t target,
			      unsigned long age,
			      int chunk)
{
	const unsigned long end_time =
		jiffies + (ww ? msecs_to_jiffies(CPTCFG_DRM_I915_FENCE_TIMEOUT) : 5);
	struct list_head *phases[] = {
		/*
		 * Purgeable objects are deemed to be free by userspace
		 * and exist purely as a means to cache pages. They are
		 * a resource that we can reallocate from as userspace
		 * must revalidate the purgeable object prior to use,
		 * and be prepared to recover if the content was lost.
		 *
		 * It is always preferrable to reuse the purgeable objects
		 * as we we can immediately reallocate their pages without
		 * swapping them out to shmemfs, even to prefer waiting
		 * for those to complete prior to looking at inactive
		 * objects, as those inactive objects will need to be swapped
		 * out and so impose their own execution barrier, similar
		 * to waiting for work completion on the purgeable lists.
		 */
		&mem->objects.migratable,
		&mem->objects.list,
		NULL,
	};
	struct intel_memory_region_link bookmark = { .age = jiffies };
	struct intel_memory_region_link end = { .age = age };
	struct intel_memory_region_link *pos;
	struct list_head **phase = phases;
	resource_size_t found = 0;
	bool keepalive, scan;
	long max_timeout = 0;
	long timeout;
	int err = 0;

	if (mem->ops->shrink_cache) {
		found = mem->ops->shrink_cache(mem, 0, target);
		if (found >= target)
			return found;
	}

	/*
	 * Eviction uses a per-object mutex to reclaim pages, so how do
	 * we ensure both deadlock avoidance and that all clients can
	 * take a turn at evicting every object? ww_mutex.
	 *
	 * The deadlock avoidance algorithm built into ww_mutex essentially
	 * provides each client with a ticket for the order in which
	 * they can reclaim objects from the memory region. (If there's
	 * a conflict between two clients, the lowest ticket holder has
	 * priority.) And by ensuring that we never lose track of objects
	 * that are known to the memory region, both objects that are in
	 * the process of being allocated and of being evicted, all clients
	 * that need eviction may scan all objects and in doing so provide
	 * a fair ordering across all current evictions and future allocations.
	 */
next:
	timeout = 0;
	scan = false;
	keepalive = true;

	while (list_empty(*phase)) {
		if (!*++phase)
			goto out;
	}

	i915_gem_flush_free_objects(mem->i915);

	spin_lock_irq(&mem->objects.lock);
	list_add_tail(&end.link, *phase);
	list_for_each_entry(pos, *phase, link) {
		struct drm_i915_gem_object *obj;

		if (unlikely(signal_pending(current))) {
			err = -EINTR;
			break;
		}

		if (unlikely(!pos->mem)) { /* skip over other bookmarks */
			if (pos == &end) {
				if (time_before(age, bookmark.age)) {
					pos = list_prev_entry(pos, link);
					list_move_tail(&end.link, *phase);
					age = bookmark.age;
				} else {
					timeout = max_timeout;
					keepalive = false;
				}
			}
			continue;
		}

		/* only segment BOs should be in mem->objects.list */
		obj = container_of(pos, typeof(*obj), mm.region);
		GEM_BUG_ON(i915_gem_object_has_segments(obj));

		if (i915_gem_object_is_framebuffer(obj))
			continue;

		if (!i915_gem_object_allows_eviction(obj))
			continue;

		if (ww && ww == i915_gem_get_locking_ctx(obj))
			continue;

		if (!ww && dma_resv_is_locked(obj->base.resv))
			continue;

		if (!i915_gem_object_get_rcu(obj))
			continue;

		list_replace(&pos->link, &bookmark.link);
		if (keepalive) {
			if (!(obj->flags & I915_BO_ALLOC_USER)) {
				list_add_tail(&pos->link, *phase);
				goto delete_bookmark;
			}

			if (obj->eviction || dma_resv_is_locked(obj->base.resv)) {
				list_add_tail(&pos->link, *phase);
				goto delete_bookmark;
			}

			if (!i915_gem_object_is_purgeable(obj)) {
				if (!time_before(obj->mm.region.age, age)) {
					list_add(&pos->link, &end.link);
					goto delete_bookmark;
				}

				/*
				 * On the first pass, be picky with our evictions
				 * and try to limit to only evicting just enough
				 * to satisfy our request. Everything we evict
				 * is likely to try and make space for itself,
				 * perpetuating the problem and multiplying the
				 * swap bandwidth required.
				 */
				if (2 * obj->base.size < (target - found) ||
				    obj->base.size > 2 * (target - found)) {
					list_add(&pos->link, &end.link);
					goto delete_bookmark;
				}
			}

			if (i915_gem_object_is_active(obj)) {
				list_add(&pos->link, &end.link);
				goto delete_bookmark;
			}
		}

		obj->eviction++;
		list_add_tail(&pos->link, &bookmark.link);
		spin_unlock_irq(&mem->objects.lock);

		/* Flush activity prior to grabbing locks */
		timeout = __i915_gem_object_wait(obj,
						 I915_WAIT_INTERRUPTIBLE |
						 I915_WAIT_ALL,
						 timeout);
		if (timeout < 0) {
			timeout = 0;
			goto relock;
		}

		err = __i915_gem_object_lock_to_evict(obj, ww);
		if (err)
			goto relock;

		if (!i915_gem_object_has_pages(obj) ||
		    obj->mm.region.mem != mem)
			goto unlock;

		if (i915_gem_object_is_active(obj))
			goto unlock;

		scan = true;
		i915_gem_object_move_notify(obj);

		err = i915_gem_object_unbind(obj, ww, I915_GEM_OBJECT_UNBIND_KEEP_RESIDENT);
		if (err == 0)
			err = __i915_gem_object_put_pages(obj);
		if (err == 0) {
			/* After eviction, try to keep using it from its new backing store */
			if (obj->swapto && obj->memory_mask & BIT(INTEL_REGION_SMEM)) {
				if (i915_gem_object_migrate(obj, INTEL_REGION_SMEM, false) == 0)
					__set_bit(I915_BO_EVICT_BIT, &obj->flags);
			}

			/* conservative estimate of reclaimed pages */
			GEM_TRACE("%s:{ target:%pa, found:%pa, evicting:%zu, remaining timeout:%ld }\n",
				  mem->name, &target, &found, obj->base.size, timeout);
			found += obj->base.size;
		}

unlock:
		i915_gem_object_unlock(obj);
relock:
		cond_resched();
		spin_lock_irq(&mem->objects.lock);

		GEM_BUG_ON(!obj->eviction);
		obj->eviction--;
delete_bookmark:
		i915_gem_object_put(obj);
		__list_del_entry(&bookmark.link);
		if (err == -EDEADLK)
			break;

		err = 0;
		if (found >= target)
			break;

		pos = &bookmark;
	}
	__list_del_entry(&end.link);
	spin_unlock_irq(&mem->objects.lock);
	if (err || found >= target)
		return err;

	/* And try to release all stale kernel objects */
	intel_gt_retire_requests(mem->gt);
	if (*++phase)
		goto next;

out:
	/*
	 * Keep retrying the allocation until there is nothing more to evict.
	 *
	 * If we have made any forward progress towards completing our
	 * allocation; retry. On the next pass, especially if we are competing
	 * with other threads, we may find more to evict and succeed. It is
	 * not until there is nothing left to evict on this pass and make
	 * no forward progress, do we conclude that it is better to report
	 * failure.
	 */
	if (found || i915_buddy_defrag(&mem->mm, chunk, chunk))
		return 0;

	scan |= i915_px_cache_release(mem->gt);

	/* XXX optimistic busy wait for transient pins */
	if (scan && !time_after(jiffies, end_time)) {
		max_timeout = ww ? msecs_to_jiffies(CPTCFG_DRM_I915_FENCE_TIMEOUT) : 0;
		phase = phases;
		yield();
		goto next;
	}

	if (IS_ENABLED(CPTCFG_DRM_I915_DEBUG) &&
	    target != (resource_size_t)-1 &&
	    !test_and_set_bit(INTEL_MEMORY_EVICT_SHOW, &mem->flags)) {
		struct drm_printer p = drm_info_printer(mem->gt->i915->drm.dev);

		intel_memory_region_print(mem, target, &p, 0);
	}

	return -ENXIO;
}

static unsigned int
__max_order(const struct intel_memory_region *mem, unsigned long n_pages)
{
	if (n_pages >> mem->mm.max_order)
		return mem->mm.max_order;

	return __fls(n_pages);
}

static bool
available_chunks(const struct intel_memory_region *mem, resource_size_t sz)
{
	/*
	 * Only allow this client to take from the pool of freed chunks
	 * only if there is enough memory available to satisfy all
	 * current allocation requests. If there is not enough memory
	 * available, we need to evict objects and we want to prevent
	 * the next allocation stealing our reclaimed chunk before we
	 * can use it for ourselves. So effectively when eviction
	 * has been started, every allocation goes through the eviction
	 * loop and will be ordered fairly by the ww_mutex, ensuring
	 * all clients continue to make forward progress.
	 */
	return atomic64_read(&mem->avail) >= sz;
}

int
__intel_memory_region_get_pages_buddy(struct intel_memory_region *mem,
				      struct i915_gem_ww_ctx *ww,
				      resource_size_t size,
				      unsigned long age,
				      unsigned int flags,
				      struct list_head *blocks)
{
	unsigned int order, min_order, max_order;
	unsigned long n_pages;
	int err = 0;

	GEM_BUG_ON(!IS_ALIGNED(size, mem->mm.chunk_size));

	GEM_BUG_ON((flags & (I915_ALLOC_CHUNK_4K |
			     I915_ALLOC_CHUNK_64K |
			     I915_ALLOC_CHUNK_2M |
			     I915_ALLOC_CHUNK_1G)) &&
		   (flags & I915_ALLOC_CHUNK_MIN_PAGE_SIZE));

	min_order = 0;
	if (flags & I915_ALLOC_CHUNK_1G)
		min_order = ilog2(SZ_1G) - ilog2(mem->mm.chunk_size);
	else if (flags & I915_ALLOC_CHUNK_2M)
		min_order = ilog2(SZ_2M) - ilog2(mem->mm.chunk_size);
	else if (flags & I915_ALLOC_CHUNK_64K)
		min_order = ilog2(SZ_64K) - ilog2(mem->mm.chunk_size);
	else if (flags & I915_ALLOC_CHUNK_4K)
		min_order = ilog2(SZ_4K) - ilog2(mem->mm.chunk_size);
	else if (flags & I915_ALLOC_CHUNK_MIN_PAGE_SIZE)
		min_order = ilog2(mem->min_page_size) - ilog2(mem->mm.chunk_size);

	if (flags & I915_ALLOC_CONTIGUOUS) {
		size = roundup_pow_of_two(size);
		min_order = ilog2(size) - ilog2(mem->mm.chunk_size);
	}

	if (size > mem->mm.size)
		return -E2BIG;

	n_pages = size >> ilog2(mem->mm.chunk_size);
	order = __max_order(mem, n_pages);
	GEM_BUG_ON(order < min_order);
	max_order = UINT_MAX;

	/* On the first pass, try to only reuse idle pages */
	if (!READ_ONCE(mem->parking.done))
		flags |= I915_BUDDY_ALLOC_NEVER_ACTIVE;

	/* Reserve the memory we reclaim for ourselves! */
	if (!available_chunks(mem, atomic64_add_return(size, &mem->evict)))
		goto evict;

	do {
		resource_size_t sz = mem->mm.chunk_size << order;
		struct i915_buddy_block *block;

		block = __i915_buddy_alloc(&mem->mm, order, max_order, flags);
		if (!IS_ERR(block)) {
			GEM_BUG_ON(i915_buddy_block_order(block) != order);
			GEM_BUG_ON(i915_buddy_block_is_free(block));
			atomic64_sub(sz, &mem->avail);
			list_add_tail(&block->link, blocks);

			n_pages -= BIT(order);
			if (!n_pages) {
				atomic64_sub(size, &mem->evict);
				return 0;
			}

			while (!(n_pages >> order))
				order--;

			continue;
		}

		if (!READ_ONCE(mem->parking.done) &&
		    wait_for_completion_interruptible(&mem->parking)) {
			err = -EINTR;
			break;
		}

		if (order && i915_buddy_defrag(&mem->mm, min_order, order)) {
			/* Merged a few blocks, try again */
			max_order = UINT_MAX;
			continue;
		}

		if (order-- == min_order) {
			if (flags & I915_BUDDY_ALLOC_NEVER_ACTIVE) {
				flags &= ~I915_BUDDY_ALLOC_NEVER_ACTIVE;
				goto reset;
			}

evict:			sz = n_pages * mem->mm.chunk_size;
			err = intel_memory_region_evict(mem, ww, sz, age, min_order);
			if (err)
				break;

			/* Make these chunks available for defrag */
			intel_memory_region_free_pages(mem, blocks, false);
reset:			n_pages = size >> ilog2(mem->mm.chunk_size);
			order = __max_order(mem, n_pages);
			max_order = UINT_MAX;
		}

		if (signal_pending(current)) {
			err = -EINTR;
			break;
		}
	} while (1);

	intel_memory_region_free_pages(mem, blocks, false);
	atomic64_sub(size, &mem->evict);
	return err;
}

int intel_memory_region_init_buddy(struct intel_memory_region *mem,
				   u64 start, u64 end, u64 chunk)
{
	return i915_buddy_init(&mem->mm, start, end, chunk);
}

void intel_memory_region_release_buddy(struct intel_memory_region *mem)
{
	i915_buddy_free_list(&mem->mm, &mem->reserved);
	i915_buddy_fini(&mem->mm);
}

int intel_memory_region_reserve(struct intel_memory_region *mem,
				u64 offset, u64 size)
{
	int ret;

	/* offset is relative to the region, but the buddy is absolute */
	ret = i915_buddy_alloc_range(&mem->mm, &mem->reserved,
				     mem->region.start + offset, size);
	if (!ret)
		atomic64_sub(size, &mem->avail);

	return ret;
 }
 
struct intel_memory_region *
intel_memory_region_by_type(struct drm_i915_private *i915,
			    enum intel_memory_type mem_type)
{
	struct intel_memory_region *mr;
	int id;

	for_each_memory_region(mr, i915, id)
		if (mr->type == mem_type)
			return mr;

	return NULL;
}

static int intel_memory_region_memtest(struct intel_memory_region *mem,
				       void *caller)
{
	struct drm_i915_private *i915 = mem->i915;
	int err = 0;

	if (!mem->io_start)
		return 0;

	if (IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GEM) || i915->params.memtest)
		err = iomemtest(mem, i915->params.memtest, caller);

	return err;
}

struct intel_memory_region *
intel_memory_region_create(struct intel_gt *gt,
			   resource_size_t start,
			   resource_size_t size,
			   resource_size_t min_page_size,
			   resource_size_t io_start,
			   resource_size_t io_size,
			   u16 type,
			   u16 instance,
			   const struct intel_memory_region_ops *ops)
{
	struct intel_memory_region *mem;
	int err;

	mem = kzalloc(sizeof(*mem), GFP_KERNEL);
	if (!mem)
		return ERR_PTR(-ENOMEM);

	mem->gt = gt;
	mem->i915 = gt->i915;
	mem->region = (struct resource)DEFINE_RES_MEM(start, size);
	mem->io_start = io_start;
	mem->io_size = io_size;
	mem->min_page_size = min_page_size;
	mem->ops = ops;
	mem->total = size;
	atomic64_set(&mem->avail, size);
	mem->type = type;
	mem->instance = instance;

	spin_lock_init(&mem->objects.lock);
	INIT_LIST_HEAD(&mem->objects.list);
	INIT_LIST_HEAD(&mem->objects.migratable);
	INIT_LIST_HEAD(&mem->objects.pt);

	INIT_LIST_HEAD(&mem->reserved);

	init_completion(&mem->parking);
	complete_all(&mem->parking);

	spin_lock_init(&mem->acct_lock);

	if (ops->init) {
		err = ops->init(mem);
		if (err)
			goto err_free;
	}

	err = intel_memory_region_memtest(mem, (void *)_RET_IP_);
	if (err)
		goto err_release;

	kref_init(&mem->kref);
	return mem;

err_release:
	if (mem->ops->release)
		mem->ops->release(mem);
err_free:
	kfree(mem);
	return ERR_PTR(err);
}

void intel_memory_region_set_name(struct intel_memory_region *mem,
				  const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(mem->name, sizeof(mem->name), fmt, ap);
	va_end(ap);
}

static void __intel_memory_region_destroy(struct kref *kref)
{
	struct intel_memory_region *mem =
		container_of(kref, typeof(*mem), kref);

	if (mem->ops->release)
		mem->ops->release(mem);

	kfree(mem);
}

struct intel_memory_region *
intel_memory_region_get(struct intel_memory_region *mem)
{
	kref_get(&mem->kref);
	return mem;
}

void intel_memory_region_put(struct intel_memory_region *mem)
{
	kref_put(&mem->kref, __intel_memory_region_destroy);
}

/* Global memory region registration -- only slight layer inversions! */

int intel_memory_regions_hw_probe(struct drm_i915_private *i915)
{
	int err, i;

	/* All platforms currently have system memory */
	GEM_BUG_ON(!HAS_REGION(i915, REGION_SMEM));

	if (IS_DGFX(i915)) {
		if (IS_ENABLED(CPTCFG_DRM_I915_PCIE_STRICT_WRITE_ORDERING))
			pcie_capability_clear_word(to_pci_dev(i915->drm.dev), PCI_EXP_DEVCTL,
						   PCI_EXP_DEVCTL_RELAX_EN);
		else
			pcie_capability_set_word(to_pci_dev(i915->drm.dev), PCI_EXP_DEVCTL,
						 PCI_EXP_DEVCTL_RELAX_EN);
	}

	for (i = 0; i < ARRAY_SIZE(i915->mm.regions); i++) {
		struct intel_memory_region *mem = ERR_PTR(-ENODEV);
		struct intel_gt *gt;
		u16 type, instance;

		if (!HAS_REGION(i915, BIT(i)))
			continue;

		type = intel_region_map[i].class;
		instance = intel_region_map[i].instance;
		gt = to_root_gt(i915);

		switch (type) {
		case INTEL_MEMORY_SYSTEM:
			mem = i915_gem_shmem_setup(gt, type, instance);
			break;
		case INTEL_MEMORY_STOLEN:
			mem = i915_gem_stolen_setup(gt, type, instance);
			if (!IS_ERR(mem))
				i915->mm.stolen_region = mem;
			break;
		default:
			continue;
		}

		if (IS_ERR(mem)) {
			dev_warn(i915->drm.dev,
				 "Failed to setup global region %d type=%d (%pe)\n", i, type, mem);
			continue;
		}

		GEM_BUG_ON(intel_region_map[i].instance);

		mem->id = i;
		mem->instance = 0;
		i915->mm.regions[i] = mem;
	}

	err = 0;
	if (!intel_memory_region_by_type(i915, INTEL_MEMORY_SYSTEM)) {
		dev_err(i915->drm.dev,
			"Failed to setup system memory, unable to continue\n");
		intel_memory_regions_driver_release(i915);
		err = -ENODEV;
	}

	return err;
}

int intel_memory_regions_resume_early(struct drm_i915_private *i915)
{
	int ret = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(i915->mm.regions); i++) {
		struct intel_memory_region *region;
		int err = 0;

		region = i915->mm.regions[i];
		if (!region)
			continue;

		err = intel_memory_region_memtest(region, (void *)_RET_IP_);
		if (err && !ret)
			ret = err;

		i915_buddy_discard_clears(&region->mm);
	}

	return ret;
}

void intel_memory_regions_driver_release(struct drm_i915_private *i915)
{
	int i;

	/* flush pending work that might use the memory regions */
	flush_workqueue(i915->wq);

	for (i = 0; i < ARRAY_SIZE(i915->mm.regions); i++) {
		struct intel_memory_region *region =
			fetch_and_zero(&i915->mm.regions[i]);

		if (region)
			intel_memory_region_put(region);
	}
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftests/intel_memory_region.c"
#include "selftests/mock_region.c"
#endif
