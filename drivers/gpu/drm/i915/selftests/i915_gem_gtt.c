/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <linux/list_sort.h>
#include <linux/prime_numbers.h>

#include "gem/i915_gem_context.h"
#include "gem/i915_gem_internal.h"
#include "gem/i915_gem_lmem.h"
#include "gem/i915_gem_object_blt.h"
#include "gem/selftests/mock_context.h"
#include "gt/intel_context.h"
#include "gt/intel_gpu_commands.h"
#include "gt/gen8_ppgtt.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_gt.h"

#include "i915_random.h"
#include "i915_selftest.h"

#include "mock_drm.h"
#include "mock_gem_device.h"
#include "mock_gtt.h"
#include "igt_flush_test.h"

static void cleanup_freed_objects(struct drm_i915_private *i915)
{
	i915_gem_drain_freed_objects(i915);
}

static void fake_free_pages(struct drm_i915_gem_object *obj,
			    struct sg_table *pages)
{
	sg_free_table(pages);
	kfree(pages);
}

static int fake_get_pages(struct drm_i915_gem_object *obj)
{
#define GFP (GFP_KERNEL | __GFP_NOWARN | __GFP_NORETRY)
#define PFN_BIAS 0x1000
	struct sg_table *pages;
	struct scatterlist *sg;
	unsigned int sg_page_sizes;
	typeof(obj->base.size) rem;

	pages = kmalloc(sizeof(*pages), GFP);
	if (!pages)
		return -ENOMEM;

	rem = round_up(obj->base.size, BIT(31)) >> 31;
	if (sg_alloc_table(pages, rem, GFP)) {
		kfree(pages);
		return -ENOMEM;
	}

	sg_page_sizes = 0;
	rem = obj->base.size;
	for (sg = pages->sgl; sg; sg = sg_next(sg)) {
		unsigned long len = min_t(typeof(rem), rem, BIT(31));

		GEM_BUG_ON(!len);
		sg_set_page(sg, pfn_to_page(PFN_BIAS), len, 0);
		sg_dma_address(sg) = page_to_phys(sg_page(sg));
		sg_dma_len(sg) = len;
		sg_page_sizes |= len;

		rem -= len;
	}
	GEM_BUG_ON(rem);

	__i915_gem_object_set_pages(obj, pages, sg_page_sizes);

	return 0;
#undef GFP
}

static int fake_put_pages(struct drm_i915_gem_object *obj,
			   struct sg_table *pages)
{
	fake_free_pages(obj, pages);
	obj->mm.dirty = false;
	return 0;
}

static const struct drm_i915_gem_object_ops fake_ops = {
	.name = "fake-gem",
	.flags = I915_GEM_OBJECT_IS_SHRINKABLE,
	.get_pages = fake_get_pages,
	.put_pages = fake_put_pages,
};

static struct drm_i915_gem_object *
fake_dma_object(struct drm_i915_private *i915, u64 size)
{
	static struct lock_class_key lock_class;
	struct drm_i915_gem_object *obj;

	GEM_BUG_ON(!size);
	GEM_BUG_ON(!IS_ALIGNED(size, I915_GTT_PAGE_SIZE));

	if (overflows_type(size, obj->base.size))
		return ERR_PTR(-E2BIG);

	obj = i915_gem_object_alloc();
	if (!obj)
		goto err;

	drm_gem_private_object_init(&i915->drm, &obj->base, size);
	i915_gem_object_init(obj, &fake_ops, &lock_class, 0);

	i915_gem_object_set_volatile(obj);

	obj->write_domain = I915_GEM_DOMAIN_CPU;
	obj->read_domains = I915_GEM_DOMAIN_CPU;
	obj->pat_index = i915_gem_get_pat_index(i915, I915_CACHE_NONE);

	/* Preallocate the "backing storage" */
	if (i915_gem_object_pin_pages_unlocked(obj))
		goto err_obj;

	i915_gem_object_unpin_pages(obj);
	return obj;

err_obj:
	i915_gem_object_put(obj);
err:
	return ERR_PTR(-ENOMEM);
}

static int igt_ppgtt_alloc(void *arg)
{
	struct drm_i915_private *dev_priv = arg;
	struct i915_ppgtt *ppgtt;
	struct i915_gem_ww_ctx ww;
	u64 size, last, limit;
	int err = 0;

	/* Allocate a ppggt and try to fill the entire range */

	if (!HAS_PPGTT(dev_priv))
		return 0;

	ppgtt = i915_ppgtt_create(to_gt(dev_priv), 0);
	if (IS_ERR(ppgtt))
		return PTR_ERR(ppgtt);

	if (!ppgtt->vm.allocate_va_range)
		goto err_ppgtt_cleanup;

	/*
	 * While we only allocate the page tables here and so we could
	 * address a much larger GTT than we could actually fit into
	 * RAM, a practical limit is the amount of physical pages in the system.
	 * This should ensure that we do not run into the oomkiller during
	 * the test and take down the machine wilfully.
	 */
	limit = totalram_pages() << PAGE_SHIFT;
	limit = min(ppgtt->vm.total, limit);

	i915_gem_ww_ctx_init(&ww, false);
retry:
	err = i915_vm_lock_objects(&ppgtt->vm, &ww);
	if (err)
		goto err_ppgtt_cleanup;

	/* Check we can allocate the entire range */
	for (size = 4096; size <= limit; size <<= 2) {
		struct i915_vm_pt_stash stash = {};

		err = i915_vm_alloc_pt_stash(&ppgtt->vm, &stash, size);
		if (err)
			goto err_ppgtt_cleanup;

		err = i915_vm_map_pt_stash(&ppgtt->vm, &stash);
		if (err) {
			i915_vm_free_pt_stash(&ppgtt->vm, &stash);
			goto err_ppgtt_cleanup;
		}

		ppgtt->vm.allocate_va_range(&ppgtt->vm, &stash, 0, size);
		cond_resched();

		ppgtt->vm.clear_range(&ppgtt->vm, 0, size);

		i915_vm_free_pt_stash(&ppgtt->vm, &stash);
	}

	/* Check we can incrementally allocate the entire range */
	for (last = 0, size = 4096; size <= limit; last = size, size <<= 2) {
		struct i915_vm_pt_stash stash = {};

		err = i915_vm_alloc_pt_stash(&ppgtt->vm, &stash, size - last);
		if (err)
			goto err_ppgtt_cleanup;

		err = i915_vm_map_pt_stash(&ppgtt->vm, &stash);
		if (err) {
			i915_vm_free_pt_stash(&ppgtt->vm, &stash);
			goto err_ppgtt_cleanup;
		}

		ppgtt->vm.allocate_va_range(&ppgtt->vm, &stash,
					    last, size - last);
		cond_resched();

		i915_vm_free_pt_stash(&ppgtt->vm, &stash);
	}

err_ppgtt_cleanup:
	if (err == -EDEADLK) {
		err = i915_gem_ww_ctx_backoff(&ww);
		if (!err)
			goto retry;
	}
	i915_gem_ww_ctx_fini(&ww);

	i915_vm_put(&ppgtt->vm);
	return err;
}

static int lowlevel_hole(struct i915_address_space *vm,
			 u64 hole_start, u64 hole_end,
			 unsigned long end_time)
{
	const unsigned int min_alignment =
		i915_vm_min_alignment(vm, INTEL_MEMORY_SYSTEM);
	I915_RND_STATE(seed_prng);
	struct i915_vma *mock_vma;
	unsigned int size;

	mock_vma = kzalloc(sizeof(*mock_vma), GFP_KERNEL);
	if (!mock_vma)
		return -ENOMEM;

	__set_bit(DRM_MM_NODE_ALLOCATED_BIT, &mock_vma->node.flags);
	if (i915_is_ggtt(vm))
		__set_bit(I915_VMA_GGTT_BIT, __i915_vma_flags(mock_vma));

	/* Keep creating larger objects until one cannot fit into the hole */
	for (size = 12; (hole_end - hole_start) >> size; size++) {
		I915_RND_SUBSTATE(prng, seed_prng);
		struct drm_i915_gem_object *obj;
		unsigned int *order, count, n;
		u64 hole_size, aligned_size;

		aligned_size = max_t(u32, ilog2(min_alignment), size);
		hole_size = (hole_end - hole_start) >> aligned_size;
		if (hole_size > KMALLOC_MAX_SIZE / sizeof(u32))
			hole_size = KMALLOC_MAX_SIZE / sizeof(u32);
		count = hole_size >> 1;
		if (!count) {
			pr_debug("%s: hole is too small [%llx - %llx] >> %d: %lld\n",
				 __func__, hole_start, hole_end, size, hole_size);
			break;
		}

		do {
			order = i915_random_order(count, &prng);
			if (order)
				break;
		} while (count >>= 1);
		if (!count) {
			kfree(mock_vma);
			return -ENOMEM;
		}
		GEM_BUG_ON(!order);

		GEM_BUG_ON(count * BIT_ULL(aligned_size) > vm->total);
		GEM_BUG_ON(hole_start + count * BIT_ULL(aligned_size) > hole_end);

		/* Ignore allocation failures (i.e. don't report them as
		 * a test failure) as we are purposefully allocating very
		 * large objects without checking that we have sufficient
		 * memory. We expect to hit -ENOMEM.
		 */

		obj = fake_dma_object(vm->i915, BIT_ULL(size));
		if (IS_ERR(obj)) {
			kfree(order);
			break;
		}

		GEM_BUG_ON(obj->base.size != BIT_ULL(size));

		if (i915_gem_object_pin_pages_unlocked(obj)) {
			i915_gem_object_put(obj);
			kfree(order);
			break;
		}

		for (n = 0; n < count; n++) {
			u64 addr = hole_start + order[n] * BIT_ULL(aligned_size);
			intel_wakeref_t wakeref;

			GEM_BUG_ON(addr + BIT_ULL(aligned_size) > vm->total);

			if (igt_timeout(end_time,
					"%s timed out before %d/%d\n",
					__func__, n, count)) {
				hole_end = hole_start; /* quit */
				break;
			}

			if (vm->allocate_va_range) {
				struct i915_vm_pt_stash stash = {};
				struct i915_gem_ww_ctx ww;
				int err;

				i915_gem_ww_ctx_init(&ww, false);
retry:
				err = i915_vm_lock_objects(vm, &ww);
				if (err)
					goto alloc_vm_end;

				err = -ENOMEM;
				if (i915_vm_alloc_pt_stash(vm, &stash,
							   BIT_ULL(size)))
					goto alloc_vm_end;

				err = i915_vm_map_pt_stash(vm, &stash);
				if (!err)
					vm->allocate_va_range(vm, &stash,
							      addr, BIT_ULL(size));
				i915_vm_free_pt_stash(vm, &stash);
alloc_vm_end:
				if (err == -EDEADLK) {
					err = i915_gem_ww_ctx_backoff(&ww);
					if (!err)
						goto retry;
				}
				i915_gem_ww_ctx_fini(&ww);

				if (err)
					break;
			}

			mock_vma->vm = vm;
			mock_vma->size = BIT_ULL(size);
			mock_vma->pages = obj->mm.pages;
			mock_vma->node.size = BIT_ULL(aligned_size);
			mock_vma->node.start = addr;

			with_intel_runtime_pm(vm->gt->uncore->rpm, wakeref)
				vm->insert_entries(vm, mock_vma,
					i915_gem_get_pat_index(vm->i915,
							       I915_CACHE_NONE),
					0);
		}
		count = n;

		i915_random_reorder(order, count, &prng);
		for (n = 0; n < count; n++) {
			u64 addr = hole_start + order[n] * BIT_ULL(aligned_size);
			intel_wakeref_t wakeref;

			GEM_BUG_ON(addr + BIT_ULL(size) > vm->total);
			with_intel_runtime_pm(vm->gt->uncore->rpm, wakeref)
				vm->clear_range(vm, addr, BIT_ULL(size));
		}

		i915_gem_object_unpin_pages(obj);
		i915_gem_object_put(obj);

		kfree(order);

		cleanup_freed_objects(vm->i915);
	}

	kfree(mock_vma);
	return 0;
}

static void close_object_list(struct list_head *objects,
			      struct i915_address_space *vm)
{
	struct drm_i915_gem_object *obj, *on;
	int ignored;

	list_for_each_entry_safe(obj, on, objects, st_link) {
		struct i915_vma *vma;

		vma = i915_vma_instance(obj, vm, NULL);
		if (!IS_ERR(vma))
			ignored = i915_vma_unbind(vma);

		list_del(&obj->st_link);
		i915_gem_object_put(obj);
	}
}

static int fill_hole(struct i915_address_space *vm,
		     u64 hole_start, u64 hole_end,
		     unsigned long end_time)
{
	const u64 hole_size = hole_end - hole_start;
	struct drm_i915_gem_object *obj;
	const unsigned int min_alignment =
		i915_vm_min_alignment(vm, INTEL_MEMORY_SYSTEM);
	const unsigned long max_pages =
		min_t(u64, ULONG_MAX - 1, hole_size/2 >> ilog2(min_alignment));
	const unsigned long max_step = max(int_sqrt(max_pages), 2UL);
	unsigned long npages, prime, flags;
	struct i915_vma *vma;
	LIST_HEAD(objects);
	int err;

	/* Try binding many VMA working inwards from either edge */

	flags = PIN_OFFSET_FIXED | PIN_USER;
	if (i915_is_ggtt(vm))
		flags |= PIN_GLOBAL;


	for_each_prime_number_from(prime, 2, max_step) {
		for (npages = 1; npages <= max_pages; npages *= prime) {
			const u64 full_size = npages << PAGE_SHIFT;
			const struct {
				const char *name;
				u64 offset;
				int step;
			} phases[] = {
				{ "top-down", hole_end, -1, },
				{ "bottom-up", hole_start, 1, },
				{ }
			}, *p;

			obj = fake_dma_object(vm->i915, full_size);
			if (IS_ERR(obj))
				break;

			list_add(&obj->st_link, &objects);

			/* Align differing sized objects against the edges, and
			 * check we don't walk off into the void when binding
			 * them into the GTT.
			 */
			for (p = phases; p->name; p++) {
				u64 offset;

				offset = p->offset;
				list_for_each_entry(obj, &objects, st_link) {
					u64 aligned_size = round_up(obj->base.size,
								    min_alignment);

					vma = i915_vma_instance(obj, vm, NULL);
					if (IS_ERR(vma))
						continue;

					if (p->step < 0) {
						if (offset < hole_start + aligned_size)
							break;
						offset -= aligned_size;
					}

					err = i915_vma_pin(vma, 0, 0, offset | flags);
					if (err) {
						pr_err("%s(%s) pin (forward) failed with err=%d on size=%lu pages (prime=%lu), offset=%llx\n",
						       __func__, p->name, err, npages, prime, offset);
						goto err;
					}

					if (!drm_mm_node_allocated(&vma->node) ||
					    i915_vma_misplaced(vma, 0, 0, offset | flags)) {
						pr_err("%s(%s) (forward) insert failed: vma.node=%llx + %llx [allocated? %d], expected offset %llx\n",
						       __func__, p->name, vma->node.start, vma->node.size, drm_mm_node_allocated(&vma->node),
						       offset);
						err = -EINVAL;
						goto err;
					}

					i915_vma_unpin(vma);

					if (p->step > 0) {
						if (offset + aligned_size > hole_end)
							break;
						offset += aligned_size;
					}
				}

				offset = p->offset;
				list_for_each_entry(obj, &objects, st_link) {
					u64 aligned_size = round_up(obj->base.size,
								    min_alignment);

					vma = i915_vma_instance(obj, vm, NULL);
					if (IS_ERR(vma))
						continue;

					if (p->step < 0) {
						if (offset < hole_start + aligned_size)
							break;
						offset -= aligned_size;
					}

					if (!drm_mm_node_allocated(&vma->node) ||
					    i915_vma_misplaced(vma, 0, 0, offset | flags)) {
						pr_err("%s(%s) (forward) moved vma.node=%llx + %llx, expected offset %llx\n",
						       __func__, p->name, vma->node.start, vma->node.size,
						       offset);
						err = -EINVAL;
						goto err;
					}

					err = i915_vma_unbind(vma);
					if (err) {
						pr_err("%s(%s) (forward) unbind of vma.node=%llx + %llx failed with err=%d\n",
						       __func__, p->name, vma->node.start, vma->node.size,
						       err);
						goto err;
					}

					if (p->step > 0) {
						if (offset + aligned_size > hole_end)
							break;
						offset += aligned_size;
					}
				}

				offset = p->offset;
				list_for_each_entry_reverse(obj, &objects, st_link) {
					u64 aligned_size = round_up(obj->base.size,
								    min_alignment);

					vma = i915_vma_instance(obj, vm, NULL);
					if (IS_ERR(vma))
						continue;

					if (p->step < 0) {
						if (offset < hole_start + aligned_size)
							break;
						offset -= aligned_size;
					}

					err = i915_vma_pin(vma, 0, 0, offset | flags);
					if (err) {
						pr_err("%s(%s) pin (backward) failed with err=%d on size=%lu pages (prime=%lu), offset=%llx\n",
						       __func__, p->name, err, npages, prime, offset);
						goto err;
					}

					if (!drm_mm_node_allocated(&vma->node) ||
					    i915_vma_misplaced(vma, 0, 0, offset | flags)) {
						pr_err("%s(%s) (backward) insert failed: vma.node=%llx + %llx [allocated? %d], expected offset %llx\n",
						       __func__, p->name, vma->node.start, vma->node.size, drm_mm_node_allocated(&vma->node),
						       offset);
						err = -EINVAL;
						goto err;
					}

					i915_vma_unpin(vma);

					if (p->step > 0) {
						if (offset + aligned_size > hole_end)
							break;
						offset += aligned_size;
					}
				}

				offset = p->offset;
				list_for_each_entry_reverse(obj, &objects, st_link) {
					u64 aligned_size = round_up(obj->base.size,
								    min_alignment);

					vma = i915_vma_instance(obj, vm, NULL);
					if (IS_ERR(vma))
						continue;

					if (p->step < 0) {
						if (offset < hole_start + aligned_size)
							break;
						offset -= aligned_size;
					}

					if (!drm_mm_node_allocated(&vma->node) ||
					    i915_vma_misplaced(vma, 0, 0, offset | flags)) {
						pr_err("%s(%s) (backward) moved vma.node=%llx + %llx [allocated? %d], expected offset %llx\n",
						       __func__, p->name, vma->node.start, vma->node.size, drm_mm_node_allocated(&vma->node),
						       offset);
						err = -EINVAL;
						goto err;
					}

					err = i915_vma_unbind(vma);
					if (err) {
						pr_err("%s(%s) (backward) unbind of vma.node=%llx + %llx failed with err=%d\n",
						       __func__, p->name, vma->node.start, vma->node.size,
						       err);
						goto err;
					}

					if (p->step > 0) {
						if (offset + aligned_size > hole_end)
							break;
						offset += aligned_size;
					}
				}
			}

			if (igt_timeout(end_time, "%s timed out (npages=%lu, prime=%lu)\n",
					__func__, npages, prime)) {
				err = -EINTR;
				goto err;
			}
		}

		close_object_list(&objects, vm);
		cleanup_freed_objects(vm->i915);
	}

	return 0;

err:
	close_object_list(&objects, vm);
	return err;
}

static int walk_hole(struct i915_address_space *vm,
		     u64 hole_start, u64 hole_end,
		     unsigned long end_time)
{
	const u64 hole_size = hole_end - hole_start;
	const unsigned long max_pages =
		min_t(u64, ULONG_MAX - 1, hole_size >> PAGE_SHIFT);
	unsigned long min_alignment;
	unsigned long flags;
	u64 size;

	/* Try binding a single VMA in different positions within the hole */

	flags = PIN_OFFSET_FIXED | PIN_USER;
	if (i915_is_ggtt(vm))
		flags |= PIN_GLOBAL;

	min_alignment = i915_vm_min_alignment(vm, INTEL_MEMORY_SYSTEM);

	for_each_prime_number_from(size, 1, max_pages) {
		struct drm_i915_gem_object *obj;
		struct i915_vma *vma;
		u64 addr;
		int err = 0;

		obj = fake_dma_object(vm->i915, size << PAGE_SHIFT);
		if (IS_ERR(obj))
			break;

		vma = i915_vma_instance(obj, vm, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto err_put;
		}

		for (addr = hole_start;
		     addr + obj->base.size < hole_end;
		     addr += round_up(obj->base.size, min_alignment)) {
			err = i915_vma_pin(vma, 0, 0, addr | flags);
			if (err) {
				pr_err("%s bind failed at %llx + %llx [hole %llx- %llx] with err=%d\n",
				       __func__, addr, vma->size,
				       hole_start, hole_end, err);
				goto err_put;
			}
			i915_vma_unpin(vma);

			if (!drm_mm_node_allocated(&vma->node) ||
			    i915_vma_misplaced(vma, 0, 0, addr | flags)) {
				pr_err("%s incorrect at %llx + %llx\n",
				       __func__, addr, vma->size);
				err = -EINVAL;
				goto err_put;
			}

			err = i915_vma_unbind(vma);
			if (err) {
				pr_err("%s unbind failed at %llx + %llx  with err=%d\n",
				       __func__, addr, vma->size, err);
				goto err_put;
			}

			GEM_BUG_ON(drm_mm_node_allocated(&vma->node));

			if (igt_timeout(end_time,
					"%s timed out at %llx\n",
					__func__, addr)) {
				err = -EINTR;
				goto err_put;
			}
		}

err_put:
		i915_gem_object_put(obj);
		if (err)
			return err;

		cleanup_freed_objects(vm->i915);
	}

	return 0;
}

static int pot_hole(struct i915_address_space *vm,
		    u64 hole_start, u64 hole_end,
		    unsigned long end_time)
{
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma;
	unsigned int min_alignment;
	unsigned long flags;
	unsigned int pot;
	int err = 0;

	flags = PIN_OFFSET_FIXED | PIN_USER;
	if (i915_is_ggtt(vm))
		flags |= PIN_GLOBAL;

	min_alignment = i915_vm_min_alignment(vm, INTEL_MEMORY_SYSTEM);

	obj = i915_gem_object_create_internal(vm->i915, 2 * I915_GTT_PAGE_SIZE);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	vma = i915_vma_instance(obj, vm, NULL);
	if (IS_ERR(vma)) {
		err = PTR_ERR(vma);
		goto err_obj;
	}

	/* Insert a pair of pages across every pot boundary within the hole */
	for (pot = fls64(hole_end - 1) - 1;
	     pot > ilog2(2 * min_alignment);
	     pot--) {
		u64 step = BIT_ULL(pot);
		u64 addr;

		for (addr = round_up(hole_start + min_alignment, step) - min_alignment;
		     hole_end > addr && hole_end - addr >= 2 * min_alignment;
		     addr += step) {
			err = i915_vma_pin(vma, 0, 0, addr | flags);
			if (err) {
				pr_err("%s failed to pin object at %llx in hole [%llx - %llx], with err=%d\n",
				       __func__,
				       addr,
				       hole_start, hole_end,
				       err);
				goto err_obj;
			}

			if (!drm_mm_node_allocated(&vma->node) ||
			    i915_vma_misplaced(vma, 0, 0, addr | flags)) {
				pr_err("%s incorrect at %llx + %llx\n",
				       __func__, addr, vma->size);
				i915_vma_unpin(vma);
				err = i915_vma_unbind(vma);
				err = -EINVAL;
				goto err_obj;
			}

			i915_vma_unpin(vma);
			err = i915_vma_unbind(vma);
			GEM_BUG_ON(err);
		}

		if (igt_timeout(end_time,
				"%s timed out after %d/%d\n",
				__func__, pot, fls64(hole_end - 1) - 1)) {
			err = -EINTR;
			goto err_obj;
		}
	}

err_obj:
	i915_gem_object_put(obj);
	return err;
}

static int drunk_hole(struct i915_address_space *vm,
		      u64 hole_start, u64 hole_end,
		      unsigned long end_time)
{
	I915_RND_STATE(prng);
	unsigned int min_alignment;
	unsigned int size;
	unsigned long flags;

	flags = PIN_OFFSET_FIXED | PIN_USER;
	if (i915_is_ggtt(vm))
		flags |= PIN_GLOBAL;

	min_alignment = i915_vm_min_alignment(vm, INTEL_MEMORY_SYSTEM);

	/* Keep creating larger objects until one cannot fit into the hole */
	for (size = 12; (hole_end - hole_start) >> size; size++) {
		struct drm_i915_gem_object *obj;
		unsigned int *order, count, n;
		struct i915_vma *vma;
		u64 hole_size, aligned_size;
		int err = -ENODEV;

		aligned_size = max_t(u32, ilog2(min_alignment), size);
		hole_size = (hole_end - hole_start) >> aligned_size;
		if (hole_size > KMALLOC_MAX_SIZE / sizeof(u32))
			hole_size = KMALLOC_MAX_SIZE / sizeof(u32);
		count = hole_size >> 1;
		if (!count) {
			pr_debug("%s: hole is too small [%llx - %llx] >> %d: %lld\n",
				 __func__, hole_start, hole_end, size, hole_size);
			break;
		}

		do {
			order = i915_random_order(count, &prng);
			if (order)
				break;
		} while (count >>= 1);
		if (!count)
			return -ENOMEM;
		GEM_BUG_ON(!order);

		/* Ignore allocation failures (i.e. don't report them as
		 * a test failure) as we are purposefully allocating very
		 * large objects without checking that we have sufficient
		 * memory. We expect to hit -ENOMEM.
		 */

		obj = fake_dma_object(vm->i915, BIT_ULL(size));
		if (IS_ERR(obj)) {
			kfree(order);
			break;
		}

		vma = i915_vma_instance(obj, vm, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto err_obj;
		}

		GEM_BUG_ON(vma->size != BIT_ULL(size));

		for (n = 0; n < count; n++) {
			u64 addr = hole_start + order[n] * BIT_ULL(aligned_size);

			err = i915_vma_pin(vma, 0, 0, addr | flags);
			if (err) {
				pr_err("%s failed to pin object at %llx + %llx in hole [%llx - %llx], with err=%d\n",
				       __func__,
				       addr, BIT_ULL(size),
				       hole_start, hole_end,
				       err);
				goto err_obj;
			}

			if (!drm_mm_node_allocated(&vma->node) ||
			    i915_vma_misplaced(vma, 0, 0, addr | flags)) {
				pr_err("%s incorrect at %llx + %llx\n",
				       __func__, addr, BIT_ULL(size));
				i915_vma_unpin(vma);
				err = i915_vma_unbind(vma);
				err = -EINVAL;
				goto err_obj;
			}

			i915_vma_unpin(vma);
			err = i915_vma_unbind(vma);
			GEM_BUG_ON(err);

			if (igt_timeout(end_time,
					"%s timed out after %d/%d\n",
					__func__, n, count)) {
				err = -EINTR;
				goto err_obj;
			}
		}

err_obj:
		i915_gem_object_put(obj);
		kfree(order);
		if (err)
			return err;

		cleanup_freed_objects(vm->i915);
	}

	return 0;
}

static int __shrink_hole(struct i915_address_space *vm,
			 u64 hole_start, u64 hole_end,
			 unsigned long end_time)
{
	struct drm_i915_gem_object *obj;
	unsigned long flags = PIN_OFFSET_FIXED | PIN_USER;
	unsigned int min_alignment;
	unsigned int order = 12;
	LIST_HEAD(objects);
	int err = 0;
	u64 addr;

	min_alignment = i915_vm_min_alignment(vm, INTEL_MEMORY_SYSTEM);

	/* Keep creating larger objects until one cannot fit into the hole */
	for (addr = hole_start; addr < hole_end; ) {
		struct i915_vma *vma;
		u64 size = BIT_ULL(order++);

		size = min(size, hole_end - addr);
		obj = fake_dma_object(vm->i915, size);
		if (IS_ERR(obj)) {
			err = PTR_ERR(obj);
			break;
		}

		list_add(&obj->st_link, &objects);

		vma = i915_vma_instance(obj, vm, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			break;
		}

		GEM_BUG_ON(vma->size != size);

		err = i915_vma_pin(vma, 0, 0, addr | flags);
		if (err) {
			pr_err("%s failed to pin object at %llx + %llx in hole [%llx - %llx], with err=%d\n",
			       __func__, addr, size, hole_start, hole_end, err);
			break;
		}

		if (!drm_mm_node_allocated(&vma->node) ||
		    i915_vma_misplaced(vma, 0, 0, addr | flags)) {
			pr_err("%s incorrect at %llx + %llx\n",
			       __func__, addr, size);
			i915_vma_unpin(vma);
			err = i915_vma_unbind(vma);
			err = -EINVAL;
			break;
		}

		i915_vma_unpin(vma);
		addr += round_up(size, min_alignment);

		/*
		 * Since we are injecting allocation faults at random intervals,
		 * wait for this allocation to complete before we change the
		 * faultinjection.
		 */
		err = i915_vma_sync(vma);
		if (err)
			break;

		if (igt_timeout(end_time,
				"%s timed out at ofset %llx [%llx - %llx]\n",
				__func__, addr, hole_start, hole_end)) {
			err = -EINTR;
			break;
		}
	}

	close_object_list(&objects, vm);
	cleanup_freed_objects(vm->i915);
	return err;
}

static int shrink_hole(struct i915_address_space *vm,
		       u64 hole_start, u64 hole_end,
		       unsigned long end_time)
{
	unsigned long prime;
	int err;

	vm->fault_attr.probability = 999;
	atomic_set(&vm->fault_attr.times, -1);

	for_each_prime_number_from(prime, 0, ULONG_MAX - 1) {
		vm->fault_attr.interval = prime;
		err = __shrink_hole(vm, hole_start, hole_end, end_time);
		if (err)
			break;
	}

	memset(&vm->fault_attr, 0, sizeof(vm->fault_attr));

	return err;
}

static int shrink_boom(struct i915_address_space *vm,
		       u64 hole_start, u64 hole_end,
		       unsigned long end_time)
{
	unsigned int sizes[] = { SZ_2M, SZ_1G };
	struct drm_i915_gem_object *purge;
	struct drm_i915_gem_object *explode;
	int err;
	int i;

	/*
	 * Catch the case which shrink_hole seems to miss. The setup here
	 * requires invoking the shrinker as we do the alloc_pt/alloc_pd, while
	 * ensuring that all vma assiocated with the respective pd/pdp are
	 * unpinned at the time.
	 */

	for (i = 0; i < ARRAY_SIZE(sizes); ++i) {
		unsigned int flags = PIN_USER | PIN_OFFSET_FIXED;
		unsigned int size = sizes[i];
		struct i915_vma *vma;

		purge = fake_dma_object(vm->i915, size);
		if (IS_ERR(purge))
			return PTR_ERR(purge);

		vma = i915_vma_instance(purge, vm, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto err_purge;
		}

		/* Needed for Wa_1409502670:xehpsdv, with vma node starting at 64K */
		if (IS_XEHPSDV(vm->i915))
			flags |=  hole_start;

		err = i915_vma_pin(vma, 0, 0, flags);
		if (err)
			goto err_purge;

		/* Should now be ripe for purging */
		i915_vma_unpin(vma);

		explode = fake_dma_object(vm->i915, size);
		if (IS_ERR(explode)) {
			err = PTR_ERR(explode);
			goto err_purge;
		}

		vm->fault_attr.probability = 100;
		vm->fault_attr.interval = 1;
		atomic_set(&vm->fault_attr.times, -1);

		vma = i915_vma_instance(explode, vm, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto err_explode;
		}

		err = i915_vma_pin(vma, 0, 0, flags | size);
		if (err)
			goto err_explode;

		i915_vma_unpin(vma);

		i915_gem_object_put(purge);
		i915_gem_object_put(explode);

		memset(&vm->fault_attr, 0, sizeof(vm->fault_attr));
		cleanup_freed_objects(vm->i915);
	}

	return 0;

err_explode:
	i915_gem_object_put(explode);
err_purge:
	i915_gem_object_put(purge);
	memset(&vm->fault_attr, 0, sizeof(vm->fault_attr));
	return err;
}

static int exercise_ppgtt(struct drm_i915_private *dev_priv,
			  int (*func)(struct i915_address_space *vm,
				      u64 hole_start, u64 hole_end,
				      unsigned long end_time))
{
	struct i915_ppgtt *ppgtt;
	IGT_TIMEOUT(end_time);
	struct file *file;
	u64 node_start;
	int err;

	if (!HAS_FULL_PPGTT(dev_priv))
		return 0;

	file = mock_file(dev_priv);
	if (IS_ERR(file))
		return PTR_ERR(file);

	ppgtt = i915_ppgtt_create(to_gt(dev_priv), 0);
	if (IS_ERR(ppgtt)) {
		err = PTR_ERR(ppgtt);
		goto out_free;
	}
	GEM_BUG_ON(offset_in_page(ppgtt->vm.total));
	GEM_BUG_ON(!atomic_read(&ppgtt->vm.open));

	/* Needed for Wa_1409502670:xehpsdv, with vma node starting at 64K */
	if (IS_XEHPSDV(ppgtt->vm.i915))
		node_start =  I915_GTT_PAGE_SIZE_64K;
	else
		node_start = 0;

	err = func(&ppgtt->vm, node_start, ppgtt->vm.total, end_time);

	i915_vm_put(&ppgtt->vm);

out_free:
	fput(file);
	return err;
}

static int igt_ppgtt_fill(void *arg)
{
	return exercise_ppgtt(arg, fill_hole);
}

static int igt_ppgtt_walk(void *arg)
{
	return exercise_ppgtt(arg, walk_hole);
}

static int igt_ppgtt_pot(void *arg)
{
	return exercise_ppgtt(arg, pot_hole);
}

static int igt_ppgtt_drunk(void *arg)
{
	return exercise_ppgtt(arg, drunk_hole);
}

static int igt_ppgtt_lowlevel(void *arg)
{
	return exercise_ppgtt(arg, lowlevel_hole);
}

static int igt_ppgtt_shrink(void *arg)
{
	return exercise_ppgtt(arg, shrink_hole);
}

static int igt_ppgtt_shrink_boom(void *arg)
{
	return exercise_ppgtt(arg, shrink_boom);
}

static int igt_ppgtt_flat(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct intel_memory_region *mr = to_gt(i915)->lmem;
	struct drm_i915_gem_object *obj;
	struct i915_address_space *vm;
	struct i915_gem_context *ctx;
	struct i915_vma *vma, *batch;
	struct i915_gem_ww_ctx ww;
	struct intel_context *ce;
	struct i915_request *rq;
	struct drm_mm_node flat;
	I915_RND_STATE(prng);
	struct file *file;
	long timeout;
	u32 *vaddr;
	u32 val;
	int ret;
	u64 va;
	u32 i;

	if (!mr) {
		pr_info("skipping...\n");
		return 0;
	}

	file = mock_file(i915);
	if (IS_ERR(file))
		return PTR_ERR(file);

	ctx = live_context(i915, file);
	if (IS_ERR(ctx)) {
		ret = PTR_ERR(ctx);
		goto out_put;
	}

	memset(&flat, 0, sizeof(flat));
	flat.start = round_down(mr->region.start, SZ_1G);
	flat.size = round_up(mr->region.end, SZ_1G) - flat.start;
	flat.color = I915_COLOR_UNEVICTABLE;

	vm = i915_gem_context_get_eb_vm(ctx);

	ret = intel_flat_lmem_ppgtt_init(vm, &flat);
	if (ret)
		goto out_vm;

	obj = i915_gem_object_create_lmem(i915, SZ_64M,
					  I915_BO_ALLOC_CONTIGUOUS);
	if (IS_ERR(obj)) {
		ret = PTR_ERR(obj);
		goto out_fini;
	}

	vaddr = i915_gem_object_pin_map_unlocked(obj, I915_MAP_WC);
	if (IS_ERR(vaddr)) {
		ret = PTR_ERR(vaddr);
		goto out_unpin;
	}

	val = prandom_u32_state(&prng);
	/* sending one byte of data as per PVC_MEM_SET_CMD in PVC */
	if (HAS_LINK_COPY_ENGINES(i915))
		val = val & 0xff;

	memset32(vaddr, val ^ 0xdeadbeaf, obj->base.size / sizeof(u32));

	va = igt_random_offset(&prng,
			       flat.start + flat.size, vm->total,
			       obj->base.size,
			       I915_GTT_PAGE_SIZE_64K);

	vma = i915_vma_instance(obj, vm, NULL);
	ret = i915_vma_pin(vma, 0, 0,
			   PIN_USER |
			   PIN_OFFSET_FIXED |
			   va);
	if (ret)
		goto out_unpin;

	GEM_BUG_ON(vma->node.start != va);

	ce = i915_gem_context_get_engine(ctx, BCS0);
	GEM_BUG_ON(IS_ERR(ce));

	i915_gem_ww_ctx_init(&ww, false);
	intel_engine_pm_get(ce->engine);
retry:
	ret = intel_context_pin_ww(ce, &ww);
	if (ret)
		goto out_ce;

	batch = intel_emit_vma_fill_blt(ce, vma, &ww, val);
	if (IS_ERR(batch)){
		ret = PTR_ERR(batch);
		goto out_ctx;
	}

	rq = i915_request_create(ce);
	if (IS_ERR(rq)) {
		ret = PTR_ERR(rq);
		goto out_batch;
	}

	ret = intel_emit_vma_mark_active(batch, rq);
	if (unlikely(ret))
		goto out_request;

	if (ce->engine->emit_init_breadcrumb)
		ret = ce->engine->emit_init_breadcrumb(rq);

	if (likely(!ret))
		ret = ce->engine->emit_bb_start(rq,
						i915_vma_offset(batch),
						i915_vma_size(batch),
						0);
out_request:
	if (unlikely(ret))
		i915_request_set_error_once(rq, ret);

	i915_request_get(rq);
	i915_request_add(rq);

	timeout = i915_request_wait(rq, 0, HZ / 2);
	i915_request_put(rq);
	if (timeout < 0) {
		ret = -EIO;
		goto out_batch;
	}

	for (i = 0; i < obj->base.size / sizeof(u32); ++i) {
		if (HAS_LINK_COPY_ENGINES(i915))
			ret = (vaddr[i] != (val << 24 | val << 16 | val << 8 | val));
		else
			ret = (vaddr[i] != val);

		if (ret) {
			pr_err("vaddr[%u]=%u\n", i, vaddr[i]);
			ret = -EINVAL;
			break;
		}
	}

out_batch:
	intel_emit_vma_release(ce, batch);
out_ctx:
	intel_context_unpin(ce);
out_ce:
	if (ret == -EDEADLK) {
		ret = i915_gem_ww_ctx_backoff(&ww);
		if(!ret)
			goto retry;
	}
	i915_gem_ww_ctx_fini(&ww);
	intel_context_put(ce);
	intel_engine_pm_put(ce->engine);
out_unpin:
	i915_gem_object_put(obj);

out_fini:
	intel_flat_lmem_ppgtt_fini(vm, &flat);
out_vm:
	i915_vm_put(vm);
out_put:
	fput(file);

	if (igt_flush_test(i915))
		ret = -EIO;

	return ret;
}

static int sort_holes(void *priv, const struct list_head *A,
		      const struct list_head *B)
{
	struct drm_mm_node *a = list_entry(A, typeof(*a), hole_stack);
	struct drm_mm_node *b = list_entry(B, typeof(*b), hole_stack);

	if (a->start < b->start)
		return -1;
	else
		return 1;
}

static int exercise_ggtt(struct drm_i915_private *i915,
			 int (*func)(struct i915_address_space *vm,
				     u64 hole_start, u64 hole_end,
				     unsigned long end_time))
{
	struct i915_ggtt *ggtt = to_gt(i915)->ggtt;
	u64 hole_start, hole_end, last = 0;
	struct drm_mm_node *node;
	IGT_TIMEOUT(end_time);
	int err = 0;

restart:
	list_sort(NULL, &ggtt->vm.mm.hole_stack, sort_holes);
	drm_mm_for_each_hole(node, &ggtt->vm.mm, hole_start, hole_end) {
		if (hole_start < last)
			continue;

		if (ggtt->vm.mm.color_adjust)
			ggtt->vm.mm.color_adjust(node, 0,
						 &hole_start, &hole_end);
		if (hole_start >= hole_end)
			continue;

		err = func(&ggtt->vm, hole_start, hole_end, end_time);
		if (err)
			break;

		/* As we have manipulated the drm_mm, the list may be corrupt */
		last = hole_end;
		goto restart;
	}

	return err;
}

static int igt_ggtt_fill(void *arg)
{
	return exercise_ggtt(arg, fill_hole);
}

static int igt_ggtt_walk(void *arg)
{
	return exercise_ggtt(arg, walk_hole);
}

static int igt_ggtt_pot(void *arg)
{
	return exercise_ggtt(arg, pot_hole);
}

static int igt_ggtt_drunk(void *arg)
{
	return exercise_ggtt(arg, drunk_hole);
}

static int igt_ggtt_lowlevel(void *arg)
{
	return exercise_ggtt(arg, lowlevel_hole);
}

static int igt_ggtt_page(void *arg)
{
	const unsigned int count = PAGE_SIZE/sizeof(u32);
	I915_RND_STATE(prng);
	struct drm_i915_private *i915 = arg;
	struct i915_ggtt *ggtt = to_gt(i915)->ggtt;
	struct drm_i915_gem_object *obj;
	intel_wakeref_t wakeref;
	struct drm_mm_node tmp;
	unsigned int *order, n;
	int err;

	if (!i915_ggtt_has_aperture(ggtt))
		return 0;

	obj = i915_gem_object_create_internal(i915, PAGE_SIZE);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	err = i915_gem_object_pin_pages_unlocked(obj);
	if (err)
		goto out_free;

	memset(&tmp, 0, sizeof(tmp));
	mutex_lock(&ggtt->vm.mutex);
	err = drm_mm_insert_node_in_range(&ggtt->vm.mm, &tmp,
					  count * PAGE_SIZE, 0,
					  I915_COLOR_UNEVICTABLE,
					  0, ggtt->mappable_end,
					  DRM_MM_INSERT_LOW);
	mutex_unlock(&ggtt->vm.mutex);
	if (err)
		goto out_unpin;

	wakeref = intel_runtime_pm_get(&i915->runtime_pm);

	for (n = 0; n < count; n++) {
		u64 offset = tmp.start + n * PAGE_SIZE;

		ggtt->vm.insert_page(&ggtt->vm,
				     i915_gem_object_get_dma_address(obj, 0),
				     offset,
				     i915_gem_get_pat_index(i915,
							    I915_CACHE_NONE),
				     0);
	}

	order = i915_random_order(count, &prng);
	if (!order) {
		err = -ENOMEM;
		goto out_remove;
	}

	for (n = 0; n < count; n++) {
		u64 offset = tmp.start + order[n] * PAGE_SIZE;
		u32 __iomem *vaddr;

		vaddr = io_mapping_map_atomic_wc(&ggtt->iomap, offset);
		iowrite32(n, vaddr + n);
		io_mapping_unmap_atomic(vaddr);
	}
	intel_gt_flush_ggtt_writes(ggtt->vm.gt);

	i915_random_reorder(order, count, &prng);
	for (n = 0; n < count; n++) {
		u64 offset = tmp.start + order[n] * PAGE_SIZE;
		u32 __iomem *vaddr;
		u32 val;

		vaddr = io_mapping_map_atomic_wc(&ggtt->iomap, offset);
		val = ioread32(vaddr + n);
		io_mapping_unmap_atomic(vaddr);

		if (val != n) {
			pr_err("insert page failed: found %d, expected %d\n",
			       val, n);
			err = -EINVAL;
			break;
		}
	}

	kfree(order);
out_remove:
	ggtt->vm.clear_range(&ggtt->vm, tmp.start, tmp.size);
	intel_runtime_pm_put(&i915->runtime_pm, wakeref);
	mutex_lock(&ggtt->vm.mutex);
	drm_mm_remove_node(&tmp);
	mutex_unlock(&ggtt->vm.mutex);
out_unpin:
	i915_gem_object_unpin_pages(obj);
out_free:
	i915_gem_object_put(obj);
	return err;
}

static void track_vma_bind(struct i915_vma *vma)
{
	struct drm_i915_gem_object *obj = vma->obj;

	__i915_gem_object_pin_pages(obj);

	GEM_BUG_ON(vma->pages);
	atomic_set(&vma->pages_count, I915_VMA_PAGES_ACTIVE);
	__i915_gem_object_pin_pages(obj);
	vma->pages = obj->mm.pages;

	mutex_lock(&vma->vm->mutex);
	list_add_tail(&vma->vm_link, &vma->vm->bound_list);
	mutex_unlock(&vma->vm->mutex);
}

static int exercise_mock(struct drm_i915_private *i915,
			 int (*func)(struct i915_address_space *vm,
				     u64 hole_start, u64 hole_end,
				     unsigned long end_time))
{
	const u64 limit = totalram_pages() << PAGE_SHIFT;
	struct i915_address_space *vm;
	struct i915_gem_context *ctx;
	IGT_TIMEOUT(end_time);
	int err;

	ctx = mock_context(i915, "mock");
	if (!ctx)
		return -ENOMEM;

	vm = i915_gem_context_get_eb_vm(ctx);
	err = func(vm, 0, min(vm->total, limit), end_time);
	i915_vm_put(vm);

	mock_context_close(ctx);
	return err;
}

static int igt_mock_fill(void *arg)
{
	struct i915_ggtt *ggtt = arg;

	return exercise_mock(ggtt->vm.i915, fill_hole);
}

static int igt_mock_walk(void *arg)
{
	struct i915_ggtt *ggtt = arg;

	return exercise_mock(ggtt->vm.i915, walk_hole);
}

static int igt_mock_pot(void *arg)
{
	struct i915_ggtt *ggtt = arg;

	return exercise_mock(ggtt->vm.i915, pot_hole);
}

static int igt_mock_drunk(void *arg)
{
	struct i915_ggtt *ggtt = arg;

	return exercise_mock(ggtt->vm.i915, drunk_hole);
}

static int igt_gtt_reserve(void *arg)
{
	struct i915_ggtt *ggtt = arg;
	struct drm_i915_gem_object *obj, *on;
	I915_RND_STATE(prng);
	LIST_HEAD(objects);
	u64 total;
	int err = -ENODEV;

	/* i915_gem_gtt_reserve() tries to reserve the precise range
	 * for the node, and evicts if it has to. So our test checks that
	 * it can give us the requsted space and prevent overlaps.
	 */

	/* Start by filling the GGTT */
	for (total = 0;
	     total + 2 * I915_GTT_PAGE_SIZE <= ggtt->vm.total;
	     total += 2 * I915_GTT_PAGE_SIZE) {
		struct i915_vma *vma;

		obj = i915_gem_object_create_internal(ggtt->vm.i915,
						      2 * PAGE_SIZE);
		if (IS_ERR(obj)) {
			err = PTR_ERR(obj);
			goto out;
		}

		err = i915_gem_object_pin_pages_unlocked(obj);
		if (err) {
			i915_gem_object_put(obj);
			goto out;
		}

		list_add(&obj->st_link, &objects);

		vma = i915_vma_instance(obj, &ggtt->vm, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto out;
		}

		mutex_lock(&ggtt->vm.mutex);
		err = i915_gem_gtt_reserve(&ggtt->vm, &vma->node,
					   obj->base.size,
					   total,
					   obj->pat_index,
					   0);
		mutex_unlock(&ggtt->vm.mutex);
		if (err) {
			pr_err("i915_gem_gtt_reserve (pass 1) failed at %llu/%llu with err=%d\n",
			       total, ggtt->vm.total, err);
			goto out;
		}
		track_vma_bind(vma);

		GEM_BUG_ON(!drm_mm_node_allocated(&vma->node));
		if (vma->node.start != total ||
		    vma->node.size != 2*I915_GTT_PAGE_SIZE) {
			pr_err("i915_gem_gtt_reserve (pass 1) placement failed, found (%llx + %llx), expected (%llx + %llx)\n",
			       vma->node.start, vma->node.size,
			       total, 2*I915_GTT_PAGE_SIZE);
			err = -EINVAL;
			goto out;
		}
	}

	/* Now we start forcing evictions */
	for (total = I915_GTT_PAGE_SIZE;
	     total + 2 * I915_GTT_PAGE_SIZE <= ggtt->vm.total;
	     total += 2 * I915_GTT_PAGE_SIZE) {
		struct i915_vma *vma;

		obj = i915_gem_object_create_internal(ggtt->vm.i915,
						      2 * PAGE_SIZE);
		if (IS_ERR(obj)) {
			err = PTR_ERR(obj);
			goto out;
		}

		err = i915_gem_object_pin_pages_unlocked(obj);
		if (err) {
			i915_gem_object_put(obj);
			goto out;
		}

		list_add(&obj->st_link, &objects);

		vma = i915_vma_instance(obj, &ggtt->vm, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto out;
		}

		mutex_lock(&ggtt->vm.mutex);
		err = i915_gem_gtt_reserve(&ggtt->vm, &vma->node,
					   obj->base.size,
					   total,
					   obj->pat_index,
					   0);
		mutex_unlock(&ggtt->vm.mutex);
		if (err) {
			pr_err("i915_gem_gtt_reserve (pass 2) failed at %llu/%llu with err=%d\n",
			       total, ggtt->vm.total, err);
			goto out;
		}
		track_vma_bind(vma);

		GEM_BUG_ON(!drm_mm_node_allocated(&vma->node));
		if (vma->node.start != total ||
		    vma->node.size != 2*I915_GTT_PAGE_SIZE) {
			pr_err("i915_gem_gtt_reserve (pass 2) placement failed, found (%llx + %llx), expected (%llx + %llx)\n",
			       vma->node.start, vma->node.size,
			       total, 2*I915_GTT_PAGE_SIZE);
			err = -EINVAL;
			goto out;
		}
	}

	/* And then try at random */
	list_for_each_entry_safe(obj, on, &objects, st_link) {
		struct i915_vma *vma;
		u64 offset;

		vma = i915_vma_instance(obj, &ggtt->vm, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto out;
		}

		err = i915_vma_unbind(vma);
		if (err) {
			pr_err("i915_vma_unbind failed with err=%d!\n", err);
			goto out;
		}

		offset = igt_random_offset(&prng,
					   0, ggtt->vm.total,
					   2 * I915_GTT_PAGE_SIZE,
					   I915_GTT_MIN_ALIGNMENT);

		mutex_lock(&ggtt->vm.mutex);
		err = i915_gem_gtt_reserve(&ggtt->vm, &vma->node,
					   obj->base.size,
					   offset,
					   obj->pat_index,
					   0);
		mutex_unlock(&ggtt->vm.mutex);
		if (err) {
			pr_err("i915_gem_gtt_reserve (pass 3) failed at %llu/%llu with err=%d\n",
			       total, ggtt->vm.total, err);
			goto out;
		}
		track_vma_bind(vma);

		GEM_BUG_ON(!drm_mm_node_allocated(&vma->node));
		if (vma->node.start != offset ||
		    vma->node.size != 2*I915_GTT_PAGE_SIZE) {
			pr_err("i915_gem_gtt_reserve (pass 3) placement failed, found (%llx + %llx), expected (%llx + %llx)\n",
			       vma->node.start, vma->node.size,
			       offset, 2*I915_GTT_PAGE_SIZE);
			err = -EINVAL;
			goto out;
		}
	}

out:
	list_for_each_entry_safe(obj, on, &objects, st_link) {
		i915_gem_object_unpin_pages(obj);
		i915_gem_object_put(obj);
	}
	return err;
}

static int igt_gtt_insert(void *arg)
{
	struct i915_ggtt *ggtt = arg;
	struct drm_i915_gem_object *obj, *on;
	struct drm_mm_node tmp = {};
	const struct invalid_insert {
		u64 size;
		u64 alignment;
		u64 start, end;
	} invalid_insert[] = {
		{
			ggtt->vm.total + I915_GTT_PAGE_SIZE, 0,
			0, ggtt->vm.total,
		},
		{
			2*I915_GTT_PAGE_SIZE, 0,
			0, I915_GTT_PAGE_SIZE,
		},
		{
			-(u64)I915_GTT_PAGE_SIZE, 0,
			0, 4*I915_GTT_PAGE_SIZE,
		},
		{
			-(u64)2*I915_GTT_PAGE_SIZE, 2*I915_GTT_PAGE_SIZE,
			0, 4*I915_GTT_PAGE_SIZE,
		},
		{
			I915_GTT_PAGE_SIZE, I915_GTT_MIN_ALIGNMENT << 1,
			I915_GTT_MIN_ALIGNMENT, I915_GTT_MIN_ALIGNMENT << 1,
		},
		{}
	}, *ii;
	LIST_HEAD(objects);
	u64 total;
	int err = -ENODEV;

	/* i915_gem_gtt_insert() tries to allocate some free space in the GTT
	 * to the node, evicting if required.
	 */

	/* Check a couple of obviously invalid requests */
	for (ii = invalid_insert; ii->size; ii++) {
		mutex_lock(&ggtt->vm.mutex);
		err = i915_gem_gtt_insert(&ggtt->vm, &tmp,
					  ii->size, ii->alignment,
					  I915_COLOR_UNEVICTABLE,
					  ii->start, ii->end,
					  0);
		mutex_unlock(&ggtt->vm.mutex);
		if (err != -ENOSPC) {
			pr_err("Invalid i915_gem_gtt_insert(.size=%llx, .alignment=%llx, .start=%llx, .end=%llx) succeeded (err=%d)\n",
			       ii->size, ii->alignment, ii->start, ii->end,
			       err);
			return -EINVAL;
		}
	}

	/* Start by filling the GGTT */
	for (total = 0;
	     total + I915_GTT_PAGE_SIZE <= ggtt->vm.total;
	     total += I915_GTT_PAGE_SIZE) {
		struct i915_vma *vma;

		obj = i915_gem_object_create_internal(ggtt->vm.i915,
						      I915_GTT_PAGE_SIZE);
		if (IS_ERR(obj)) {
			err = PTR_ERR(obj);
			goto out;
		}

		err = i915_gem_object_pin_pages_unlocked(obj);
		if (err) {
			i915_gem_object_put(obj);
			goto out;
		}

		list_add(&obj->st_link, &objects);

		vma = i915_vma_instance(obj, &ggtt->vm, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto out;
		}

		mutex_lock(&ggtt->vm.mutex);
		err = i915_gem_gtt_insert(&ggtt->vm, &vma->node,
					  obj->base.size, 0, obj->pat_index,
					  0, ggtt->vm.total,
					  0);
		mutex_unlock(&ggtt->vm.mutex);
		if (err == -ENOSPC) {
			/* maxed out the GGTT space */
			i915_gem_object_put(obj);
			break;
		}
		if (err) {
			pr_err("i915_gem_gtt_insert (pass 1) failed at %llu/%llu with err=%d\n",
			       total, ggtt->vm.total, err);
			goto out;
		}
		track_vma_bind(vma);
		__i915_vma_pin(vma);

		GEM_BUG_ON(!drm_mm_node_allocated(&vma->node));
	}

	list_for_each_entry(obj, &objects, st_link) {
		struct i915_vma *vma;

		vma = i915_vma_instance(obj, &ggtt->vm, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto out;
		}

		if (!drm_mm_node_allocated(&vma->node)) {
			pr_err("VMA was unexpectedly evicted!\n");
			err = -EINVAL;
			goto out;
		}

		__i915_vma_unpin(vma);
	}

	/* If we then reinsert, we should find the same hole */
	list_for_each_entry_safe(obj, on, &objects, st_link) {
		struct i915_vma *vma;
		u64 offset;

		vma = i915_vma_instance(obj, &ggtt->vm, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto out;
		}

		GEM_BUG_ON(!drm_mm_node_allocated(&vma->node));
		offset = vma->node.start;

		err = i915_vma_unbind(vma);
		if (err) {
			pr_err("i915_vma_unbind failed with err=%d!\n", err);
			goto out;
		}

		mutex_lock(&ggtt->vm.mutex);
		err = i915_gem_gtt_insert(&ggtt->vm, &vma->node,
					  obj->base.size, 0, obj->pat_index,
					  0, ggtt->vm.total,
					  0);
		mutex_unlock(&ggtt->vm.mutex);
		if (err) {
			pr_err("i915_gem_gtt_insert (pass 2) failed at %llu/%llu with err=%d\n",
			       total, ggtt->vm.total, err);
			goto out;
		}
		track_vma_bind(vma);

		GEM_BUG_ON(!drm_mm_node_allocated(&vma->node));
		if (vma->node.start != offset) {
			pr_err("i915_gem_gtt_insert did not return node to its previous location (the only hole), expected address %llx, found %llx\n",
			       offset, vma->node.start);
			err = -EINVAL;
			goto out;
		}
	}

	/* And then force evictions */
	for (total = 0;
	     total + 2 * I915_GTT_PAGE_SIZE <= ggtt->vm.total;
	     total += 2 * I915_GTT_PAGE_SIZE) {
		struct i915_vma *vma;

		obj = i915_gem_object_create_internal(ggtt->vm.i915,
						      2 * I915_GTT_PAGE_SIZE);
		if (IS_ERR(obj)) {
			err = PTR_ERR(obj);
			goto out;
		}

		err = i915_gem_object_pin_pages_unlocked(obj);
		if (err) {
			i915_gem_object_put(obj);
			goto out;
		}

		list_add(&obj->st_link, &objects);

		vma = i915_vma_instance(obj, &ggtt->vm, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto out;
		}

		mutex_lock(&ggtt->vm.mutex);
		err = i915_gem_gtt_insert(&ggtt->vm, &vma->node,
					  obj->base.size, 0, obj->pat_index,
					  0, ggtt->vm.total,
					  0);
		mutex_unlock(&ggtt->vm.mutex);
		if (err) {
			pr_err("i915_gem_gtt_insert (pass 3) failed at %llu/%llu with err=%d\n",
			       total, ggtt->vm.total, err);
			goto out;
		}
		track_vma_bind(vma);

		GEM_BUG_ON(!drm_mm_node_allocated(&vma->node));
	}

out:
	list_for_each_entry_safe(obj, on, &objects, st_link) {
		i915_gem_object_unpin_pages(obj);
		i915_gem_object_put(obj);
	}
	return err;
}

int i915_gem_gtt_mock_selftests(void)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_mock_drunk),
		SUBTEST(igt_mock_walk),
		SUBTEST(igt_mock_pot),
		SUBTEST(igt_mock_fill),
		SUBTEST(igt_gtt_reserve),
		SUBTEST(igt_gtt_insert),
	};
	struct drm_i915_private *i915;
	int err;

	i915 = mock_gem_device();
	if (!i915)
		return -ENOMEM;

	err = i915_subtests(tests, to_gt(i915)->ggtt);

	mock_device_flush(i915);
	i915_gem_drain_freed_objects(i915);
	mock_destroy_device(i915);
	return err;
}

static int context_sync(struct intel_context *ce)
{
	struct i915_request *rq;
	long timeout;

	rq = intel_context_create_request(ce);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	i915_request_get(rq);
	i915_request_add(rq);

	timeout = i915_request_wait(rq, 0, HZ / 5);
	i915_request_put(rq);

	return timeout < 0 ? -EIO : 0;
}

static struct i915_request *
submit_batch(struct intel_context *ce, u64 addr)
{
	struct i915_request *rq;
	int err;

	rq = intel_context_create_request(ce);
	if (IS_ERR(rq))
		return rq;

	err = 0;
	if (rq->engine->emit_init_breadcrumb) /* detect a hang */
		err = rq->engine->emit_init_breadcrumb(rq);
	if (err == 0)
		err = rq->engine->emit_bb_start(rq, addr, 0, 0);

	if (err == 0)
		i915_request_get(rq);
	i915_request_add(rq);

	return err ? ERR_PTR(err) : rq;
}

static u32 *spinner(u32 *batch, int i)
{
	return batch + i * 64 / sizeof(*batch) + 4;
}

static void end_spin(u32 *batch, int i)
{
	*spinner(batch, i) = MI_BATCH_BUFFER_END;
	wmb();
}

static u64 address_limit(struct intel_context *ce, bool mi_bb_start)
{
	u64 limit;

	limit = min(ce->vm->total, BIT_ULL(ce->engine->ppgtt_size));
	if (mi_bb_start) /* Batch buffers are constrained to low 48b */
		limit = min(limit, BIT_ULL(48));

	return limit;
}

static int igt_cs_tlb(void *arg)
{
	const unsigned int count = PAGE_SIZE / 64;
	const unsigned int chunk_size = count * PAGE_SIZE;
	struct drm_i915_private *i915 = arg;
	struct drm_i915_gem_object *bbe, *act, *out;
	struct i915_gem_engines_iter it;
	struct i915_address_space *vm;
	struct i915_gem_context *ctx;
	struct intel_context *ce;
	struct i915_vma *dst;
	struct i915_vma *vma;
	I915_RND_STATE(prng);
	struct file *file;
	unsigned int i;
	u32 *result;
	u32 *batch;
	int err = 0;

	/*
	 * Our mission here is to fool the hardware to execute something
	 * from scratch as it has not seen the batch move (due to missing
	 * the TLB invalidate).
	 */

	file = mock_file(i915);
	if (IS_ERR(file))
		return PTR_ERR(file);

	ctx = live_context(i915, file);
	if (IS_ERR(ctx)) {
		err = PTR_ERR(ctx);
		goto out_unlock;
	}

	vm = i915_gem_context_get_eb_vm(ctx);
	if (i915_is_ggtt(vm))
		goto out_vm;

	/* Create two pages; dummy we prefill the TLB, and intended */
	bbe = i915_gem_object_create_internal(i915, PAGE_SIZE);
	if (IS_ERR(bbe)) {
		err = PTR_ERR(bbe);
		goto out_vm;
	}

	batch = i915_gem_object_pin_map_unlocked(bbe, I915_MAP_WC);
	if (IS_ERR(batch)) {
		err = PTR_ERR(batch);
		goto out_put_bbe;
	}
	memset32(batch, MI_BATCH_BUFFER_END, PAGE_SIZE / sizeof(u32));
	i915_gem_object_flush_map(bbe);
	i915_gem_object_unpin_map(bbe);

	act = i915_gem_object_create_internal(i915, PAGE_SIZE);
	if (IS_ERR(act)) {
		err = PTR_ERR(act);
		goto out_put_bbe;
	}

	/* Track the execution of each request by writing into different slot */
	batch = i915_gem_object_pin_map_unlocked(act, I915_MAP_WC);
	if (IS_ERR(batch)) {
		err = PTR_ERR(batch);
		goto out_put_act;
	}

	out = i915_gem_object_create_internal(i915, PAGE_SIZE);
	if (IS_ERR(out)) {
		err = PTR_ERR(out);
		goto out_put_batch;
	}
	i915_gem_object_set_cache_coherency(out, I915_CACHING_CACHED);

	dst = i915_vma_instance(out, vm, NULL);
	if (IS_ERR(dst)) {
		err = PTR_ERR(dst);
		goto out_put_out;
	}

	result = i915_gem_object_pin_map_unlocked(out, I915_MAP_WB);
	if (IS_ERR(result)) {
		err = PTR_ERR(result);
		goto out_put_out;
	}

	for_each_gem_engine(ce, i915_gem_context_lock_engines(ctx), it) {
		u64 addr = address_limit(ce, true) - PAGE_SIZE;
		IGT_TIMEOUT(end_time);
		unsigned long pass = 0;

		if (!intel_engine_can_store_dword(ce->engine))
			continue;

		err = i915_vma_unbind(dst);
		if (err)
			goto end;

		err = i915_vma_pin(dst, 0, 0,
				   PIN_USER |
				   PIN_OFFSET_FIXED |
				   addr);
		if (err)
			goto end;
		GEM_BUG_ON(dst->node.start != addr);

		for (i = 0; i < count; i++) {
			u32 *cs = batch + i * 64 / sizeof(*cs);

			GEM_BUG_ON(GRAPHICS_VER(i915) < 6);
			cs[0] = MI_STORE_DWORD_IMM_GEN4;
			if (GRAPHICS_VER(i915) >= 8) {
				cs[1] = lower_32_bits(addr + i * sizeof(u32));
				cs[2] = upper_32_bits(addr);
				cs[3] = i;
				cs[4] = MI_NOOP;
				cs[5] = MI_BATCH_BUFFER_START_GEN8;
			} else {
				cs[1] = 0;
				cs[2] = lower_32_bits(addr + i * sizeof(u32));
				cs[3] = i;
				cs[4] = MI_NOOP;
				cs[5] = MI_BATCH_BUFFER_START;
			}
		}

		while (!__igt_timeout(end_time, NULL)) {
			struct i915_vm_pt_stash stash = {};
			struct i915_request *rq;
			struct i915_gem_ww_ctx ww;
			u64 offset;

			offset = igt_random_offset(&prng,
						   0, addr,
						   chunk_size, PAGE_SIZE);

			memset32(result, STACK_MAGIC, PAGE_SIZE / sizeof(u32));

			vma = i915_vma_instance(bbe, vm, NULL);
			if (IS_ERR(vma)) {
				err = PTR_ERR(vma);
				goto end;
			}

			err = vma->ops->set_pages(vma);
			if (err)
				goto end;

			i915_gem_ww_ctx_init(&ww, false);
retry:
			err = i915_vm_lock_objects(vm, &ww);
			if (err)
				goto end_ww;

			err = i915_vm_alloc_pt_stash(vm, &stash, chunk_size);
			if (err)
				goto end_ww;

			err = i915_vm_map_pt_stash(vm, &stash);
			if (!err)
				vm->allocate_va_range(vm, &stash, offset, chunk_size);
			i915_vm_free_pt_stash(vm, &stash);
end_ww:
			if (err == -EDEADLK) {
				err = i915_gem_ww_ctx_backoff(&ww);
				if (!err)
					goto retry;
			}
			i915_gem_ww_ctx_fini(&ww);
			if (err)
				goto end;

			/* Prime the TLB with the dummy pages */
			__set_bit(DRM_MM_NODE_ALLOCATED_BIT, &vma->node.flags);
			for (i = 0; i < count; i++) {
				vma->node.start = offset + i * PAGE_SIZE;
				vm->insert_entries(vm, vma,
					i915_gem_get_pat_index(i915,
							       I915_CACHE_NONE),
					0);

				rq = submit_batch(ce, vma->node.start);
				if (IS_ERR(rq)) {
					err = PTR_ERR(rq);
					goto end;
				}
				i915_request_put(rq);
			}
			vma->ops->clear_pages(vma);
			__clear_bit(DRM_MM_NODE_ALLOCATED_BIT,
				    &vma->node.flags);

			err = context_sync(ce);
			if (err) {
				pr_err("%s: dummy setup timed out\n",
				       ce->engine->name);
				goto end;
			}

			vma = i915_vma_instance(act, vm, NULL);
			if (IS_ERR(vma)) {
				err = PTR_ERR(vma);
				goto end;
			}

			err = vma->ops->set_pages(vma);
			if (err)
				goto end;

			/* Replace the TLB with target batches */
			__set_bit(DRM_MM_NODE_ALLOCATED_BIT, &vma->node.flags);
			for (i = 0; i < count; i++) {
				struct i915_request *rq;
				u32 *cs = batch + i * 64 / sizeof(*cs);
				u64 addr;

				vma->node.start = offset + i * PAGE_SIZE;
				vm->insert_entries(vm, vma,
					i915_gem_get_pat_index(i915,
							       I915_CACHE_NONE),
					0);

				addr = vma->node.start + i * 64;
				cs[4] = MI_NOOP;
				cs[6] = lower_32_bits(addr);
				cs[7] = upper_32_bits(addr);
				wmb();

				rq = submit_batch(ce, addr);
				if (IS_ERR(rq)) {
					err = PTR_ERR(rq);
					goto end;
				}

				/* Wait until the context chain has started */
				if (i == 0) {
					while (READ_ONCE(result[i]) &&
					       !i915_request_completed(rq))
						cond_resched();
				} else {
					end_spin(batch, i - 1);
				}

				i915_request_put(rq);
			}
			end_spin(batch, count - 1);
			vma->ops->clear_pages(vma);
			__clear_bit(DRM_MM_NODE_ALLOCATED_BIT,
				    &vma->node.flags);

			err = context_sync(ce);
			if (err) {
				pr_err("%s: writes timed out\n",
				       ce->engine->name);
				goto end;
			}

			for (i = 0; i < count; i++) {
				if (result[i] != i) {
					pr_err("%s: Write lost on pass %lu, at offset %llx, index %d, found %x, expected %x\n",
					       ce->engine->name, pass,
					       offset, i, result[i], i);
					err = -EINVAL;
					goto end;
				}
			}

			vm->clear_range(vm, offset, chunk_size);
			pass++;
		}

		i915_vma_unpin(dst);
	}
end:
	if (igt_flush_test(i915))
		err = -EIO;
	i915_gem_context_unlock_engines(ctx);
	i915_gem_object_unpin_map(out);
out_put_out:
	i915_gem_object_put(out);
out_put_batch:
	i915_gem_object_unpin_map(act);
out_put_act:
	i915_gem_object_put(act);
out_put_bbe:
	i915_gem_object_put(bbe);
out_vm:
	i915_vm_put(vm);
out_unlock:
	fput(file);
	return err;
}

int i915_gem_gtt_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_ppgtt_alloc),
		SUBTEST(igt_ppgtt_lowlevel),
		SUBTEST(igt_ppgtt_drunk),
		SUBTEST(igt_ppgtt_walk),
		SUBTEST(igt_ppgtt_pot),
		SUBTEST(igt_ppgtt_fill),
		SUBTEST(igt_ppgtt_shrink),
		SUBTEST(igt_ppgtt_shrink_boom),
		SUBTEST(igt_ppgtt_flat),
		SUBTEST(igt_ggtt_lowlevel),
		SUBTEST(igt_ggtt_drunk),
		SUBTEST(igt_ggtt_walk),
		SUBTEST(igt_ggtt_pot),
		SUBTEST(igt_ggtt_fill),
		SUBTEST(igt_ggtt_page),
		SUBTEST(igt_cs_tlb),
	};

	GEM_BUG_ON(offset_in_page(to_gt(i915)->ggtt->vm.total));

	return i915_live_subtests(tests, i915);
}
