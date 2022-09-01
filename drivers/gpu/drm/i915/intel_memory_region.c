// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */
#include <linux/compiler_attributes.h>

#include <linux/random.h>

#include "gt/intel_gt_requests.h"
#include "gt/intel_gt_regs.h"

#include "intel_memory_region.h"
#include "i915_drv.h"
#include "i915_svm.h"

static const struct {
	u16 class;
	u16 instance;
} intel_region_map[] = {
	[INTEL_REGION_SMEM] = {
		.class = INTEL_MEMORY_SYSTEM,
		.instance = 0,
	},
	[INTEL_REGION_LMEM] = {
		.class = INTEL_MEMORY_LOCAL,
		.instance = 0,
	},
	[INTEL_REGION_LMEM1] = {
		.class = INTEL_MEMORY_LOCAL,
		.instance = 1,
	},
	[INTEL_REGION_LMEM2] = {
		.class = INTEL_MEMORY_LOCAL,
		.instance = 2,
	},
	[INTEL_REGION_LMEM3] = {
		.class = INTEL_MEMORY_LOCAL,
		.instance = 3,
	},
	[INTEL_REGION_STOLEN_SMEM] = {
		.class = INTEL_MEMORY_STOLEN_SYSTEM,
		.instance = 0,
	},
	[INTEL_REGION_STOLEN_LMEM] = {
		.class = INTEL_MEMORY_STOLEN_LOCAL,
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

	if (resource_size(&mem->region) < PAGE_SIZE)
		return 0;

	last = resource_size(&mem->region) - PAGE_SIZE;

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

static u64
intel_memory_region_free_pages(struct intel_memory_region *mem,
			       struct list_head *blocks)
{
	struct i915_buddy_block *block, *on;
	u64 size = 0;

	list_for_each_entry_safe(block, on, blocks, link) {
		size += i915_buddy_block_size(&mem->mm, block);
		i915_buddy_free(&mem->mm, block);
	}
	INIT_LIST_HEAD(blocks);

	return size;
}

void
__intel_memory_region_put_pages_buddy(struct intel_memory_region *mem,
				      struct list_head *blocks)
{
	mutex_lock(&mem->mm_lock);
	mem->avail += intel_memory_region_free_pages(mem, blocks);
	mutex_unlock(&mem->mm_lock);
}

static void __intel_memory_region_put_block_work(struct work_struct *work)
{
	struct intel_memory_region *mem =
		container_of(work, struct intel_memory_region, pd_put.work);
	struct llist_node *freed = llist_del_all(&mem->pd_put.blocks);
	struct i915_buddy_block *block;
	struct list_head blocks;

	INIT_LIST_HEAD(&blocks);

	llist_for_each_entry(block, freed, freed)
		list_add(&block->link, &blocks);

	__intel_memory_region_put_pages_buddy(mem, &blocks);
}

void
__intel_memory_region_put_block_buddy(struct i915_buddy_block *block)
{
	struct intel_memory_region *mem = block->private;

	if (llist_add(&block->freed, &mem->pd_put.blocks))
		schedule_work(&mem->pd_put.work);
}

static int intel_memory_region_evict(struct intel_memory_region *mem,
				     struct i915_gem_ww_ctx *ww,
				     resource_size_t target)
{
	struct list_head still_in_list;
	struct drm_i915_gem_object *obj;
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
		&mem->objects.purgeable,
		&mem->objects.list,
		NULL,
	};
	unsigned long timeout = 0;
	struct list_head **phase;
	resource_size_t found;
	int err = 0;

	found = 0;
	phase = phases;

next:
	INIT_LIST_HEAD(&still_in_list);
	mutex_lock(&mem->objects.lock);

	while (found < target &&
	       (obj = list_first_entry_or_null(*phase,
					       typeof(*obj),
					       mm.region_link))) {
		unsigned long flags = 0;

		list_move_tail(&obj->mm.region_link, &still_in_list);

		if (i915_gem_object_is_framebuffer(obj))
			continue;

		/* Already locked this object? */
		if (ww && ww == i915_gem_get_locking_ctx(obj))
			continue;

		if (!kref_get_unless_zero(&obj->base.refcount))
			continue;

		mutex_unlock(&mem->objects.lock);

		/* Flush activity prior to grabbing locks */
		err = i915_gem_object_wait(obj,
					   I915_WAIT_INTERRUPTIBLE |
					   I915_WAIT_ALL,
					   timeout);
		if (err)
			goto put;
		if (ww) {
			err = i915_gem_object_lock_to_evict(obj, ww);
			if (err)
				goto put;
		} else {
			flags |= I915_GEM_OBJECT_UNBIND_TRYLOCK;
			if (!i915_gem_object_trylock(obj))
				goto put;
		}

		if (!i915_gem_object_has_pages(obj))
			goto unlock;

		/* tell callee to do swapping */
		if (i915_gem_object_type_has(obj, I915_GEM_OBJECT_HAS_IOMEM)
		    && obj->mm.madv == I915_MADV_WILLNEED)
			obj->do_swapping = true;

		err = i915_gem_object_unbind(obj, ww, flags);
		if (!err) {
			/* May arrive from get_pages on another bo */
			err = __i915_gem_object_put_pages(obj);
			if (!err && !i915_gem_object_has_pages(obj)) {
				/* conservative estimate of reclaimed pages */
				found += obj->base.size;
				if (obj->mm.madv == I915_MADV_DONTNEED)
					obj->mm.madv = __I915_MADV_PURGED;
			}
		}

		/* If error not EDEADLK or EINTR or ERESTARTSYS, skip object */
		if (err != -EDEADLK && err != -EINTR && err != -ERESTARTSYS)
			err = 0;

		obj->do_swapping = false;
unlock:
		if (!ww)
			i915_gem_object_unlock(obj);
put:
		i915_gem_object_put(obj);
		mutex_lock(&mem->objects.lock);

		if (err == -EDEADLK || err == -EINTR ||
		    err == -ERESTARTSYS || found >= target)
			break;
	}
	list_splice_tail(&still_in_list, *phase);
	mutex_unlock(&mem->objects.lock);
	if (err == -EDEADLK || err == -EINTR || err == -ERESTARTSYS)
		return err;

	if (found < target) {
		if (!timeout && !list_empty(&still_in_list)) {
			/* Repeat, waiting for the active objects */
			timeout = msecs_to_jiffies(CPTCFG_DRM_I915_FENCE_TIMEOUT);
			goto next;
		}

		if (mem->i915->params.enable_eviction) {
			/* And try to release all stale kernel objects */
			intel_gt_retire_requests(mem->gt);

			timeout = 0;
			if (*++phase)
				goto next;
		}
	}

	if (ww)
		i915_gem_ww_ctx_unlock_evictions(ww);

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
	return found ? 0 : -ENOSPC;
}

int
__intel_memory_region_get_pages_buddy(struct intel_memory_region *mem,
				      struct i915_gem_ww_ctx *ww,
				      resource_size_t size,
				      unsigned int flags,
				      struct list_head *blocks)
{
	unsigned int min_order = 0;
	unsigned long n_pages;
	int err;

	GEM_BUG_ON(!IS_ALIGNED(size, mem->mm.chunk_size));
	GEM_BUG_ON(!list_empty(blocks));

	GEM_BUG_ON((flags &
		   (I915_ALLOC_CHUNK_4K | I915_ALLOC_CHUNK_64K | I915_ALLOC_CHUNK_2M |
		    I915_ALLOC_CHUNK_1G)) && (flags & I915_ALLOC_CHUNK_MIN_PAGE_SIZE));

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

	mutex_lock(&mem->mm_lock);

	do {
		struct i915_buddy_block *block;
		unsigned int order;

retry:
		order = min_t(unsigned int, __fls(n_pages), mem->mm.max_order);
		GEM_BUG_ON(order < min_order);

		do {
			block = i915_buddy_alloc(&mem->mm, order);
			if (!IS_ERR(block))
				break;

			if (order-- == min_order) {
				resource_size_t target;

				target = n_pages * mem->mm.chunk_size;

				mutex_unlock(&mem->mm_lock);
				err = intel_memory_region_evict(mem, ww,
								target);
				mutex_lock(&mem->mm_lock);
				if (err)
					goto err_free_blocks;

				goto retry;
			}
		} while (1);

		n_pages -= BIT(order);

		block->private = mem;
		list_add_tail(&block->link, blocks);

		if (!n_pages)
			break;
	} while (1);

	mem->avail -= size;
	mutex_unlock(&mem->mm_lock);
	return 0;

err_free_blocks:
	intel_memory_region_free_pages(mem, blocks);
	mutex_unlock(&mem->mm_lock);
	if (err == -EDEADLK || err == -EINTR || err == -ERESTARTSYS)
		return err;

	return -ENXIO;
}

struct i915_buddy_block *
__intel_memory_region_get_block_buddy(struct intel_memory_region *mem,
				      resource_size_t size,
				      unsigned int flags)
{
	struct i915_buddy_block *block;
	LIST_HEAD(blocks);
	int ret;

	ret = __intel_memory_region_get_pages_buddy(mem, NULL, size, flags, &blocks);
	if (ret)
		return ERR_PTR(ret);

	block = list_first_entry(&blocks, typeof(*block), link);
	list_del_init(&block->link);
	return block;
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

	mutex_lock(&mem->mm_lock);

	/* offset is relative to the region, but the buddy is absolute */
	ret = i915_buddy_alloc_range(&mem->mm, &mem->reserved,
				     mem->region.start + offset, size);
	if (!ret)
		mem->avail -= size;

	mutex_unlock(&mem->mm_lock);

	return ret;
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
	mem->min_page_size = min_page_size;
	mem->ops = ops;
	mem->total = size;
	mem->avail = mem->total;

	INIT_WORK(&mem->pd_put.work, __intel_memory_region_put_block_work);

	mutex_init(&mem->objects.lock);
	INIT_LIST_HEAD(&mem->objects.list);
	INIT_LIST_HEAD(&mem->objects.purgeable);
	INIT_LIST_HEAD(&mem->reserved);

	mutex_init(&mem->mm_lock);

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

	/* Flush any pending work to free blocks region */
	flush_work(&mem->pd_put.work);

	if (mem->ops->release)
		mem->ops->release(mem);

	mutex_destroy(&mem->mm_lock);
	mutex_destroy(&mem->objects.lock);
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
		u16 type;

		if (!HAS_REGION(i915, BIT(i)))
			continue;

		type = intel_region_map[i].class;
		gt = to_root_gt(i915);

		switch (type) {
		case INTEL_MEMORY_SYSTEM:
			mem = i915_gem_shmem_setup(gt);
			break;
		case INTEL_MEMORY_STOLEN_LOCAL:
			mem = i915_gem_stolen_lmem_setup(gt);
			if (!IS_ERR(mem))
				i915->mm.stolen_region = mem;
			break;
		case INTEL_MEMORY_STOLEN_SYSTEM:
			mem = i915_gem_stolen_smem_setup(gt);
			if (!IS_ERR(mem))
				i915->mm.stolen_region = mem;
			break;
		default:
			continue;
		}

		if (IS_ERR(mem)) {
			drm_err(&i915->drm,
				"Failed to setup global region %d type=%d (%pe)\n", i, type, mem);
			continue;
		}

		GEM_BUG_ON(intel_region_map[i].instance);

		mem->id = i;
		mem->type = type;
		mem->instance = 0;

		i915->mm.regions[i] = mem;
	}

	err = 0;
	if (!intel_memory_region_by_type(i915, INTEL_MEMORY_SYSTEM)) {
		drm_err(&i915->drm,
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

const char *intel_memory_region_id2str(enum intel_region_id id)
{
	static const char * const regions[] = {
		[INTEL_REGION_SMEM] = "smem",
		[INTEL_REGION_LMEM] = "lmem",
		[INTEL_REGION_LMEM1] = "lmem1",
		[INTEL_REGION_LMEM2] = "lmem2",
		[INTEL_REGION_LMEM3] = "lmem3",
		[INTEL_REGION_STOLEN_SMEM] = "stolen smem",
		[INTEL_REGION_STOLEN_LMEM] = "stolen lmem",
		[INTEL_REGION_UNKNOWN] = "unknown",
	};

	if (id > INTEL_REGION_UNKNOWN || !regions[id])
		return "invalid memory region";

	return regions[id];
}

void intel_memory_regions_remove(struct drm_i915_private *i915)
{
	struct intel_memory_region *mr;
	enum intel_region_id id;

	for_each_memory_region(mr, i915, id)
		i915_svm_devmem_remove(mr);
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftests/intel_memory_region.c"
#include "selftests/mock_region.c"
#endif
