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
#include "gem/selftests/mock_context.h"
#include "gt/intel_context.h"
#include "gt/intel_gpu_commands.h"
#include "gt/gen8_ppgtt.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_gt.h"

#include "i915_random.h"
#include "i915_selftest.h"

#include "mock_drm.h"
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
	return 0;
}

static const struct drm_i915_gem_object_ops fake_ops = {
	.name = "fake-gem",
	.get_pages = fake_get_pages,
	.put_pages = fake_put_pages,
};

static struct drm_i915_gem_object *
fake_dma_object(struct drm_i915_private *i915, u64 size)
{
	struct drm_i915_gem_object *obj;

	GEM_BUG_ON(!size);
	GEM_BUG_ON(!IS_ALIGNED(size, I915_GTT_PAGE_SIZE));

	if (overflows_type(size, obj->base.size))
		return ERR_PTR(-E2BIG);

	obj = i915_gem_object_alloc();
	if (!obj)
		goto err;

	drm_gem_private_object_init(&i915->drm, &obj->base, size);
	i915_gem_object_init(obj, &fake_ops, 0);

	i915_gem_object_set_volatile(obj);
	i915_gem_object_set_cache_coherency(obj, I915_CACHE_NONE);

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

			mock_vma->vm = vm;
			mock_vma->size = BIT_ULL(size);
			mock_vma->pages = obj->mm.pages;
			mock_vma->node.size = BIT_ULL(aligned_size);
			mock_vma->node.start = addr;

			with_intel_gt_pm(vm->gt, wakeref)
				vm->insert_entries(vm, mock_vma, NULL,
						   i915_gem_get_pat_index(vm->i915, I915_CACHE_NONE),
						   0);
		}
		count = n;

		i915_random_reorder(order, count, &prng);
		for (n = 0; n < count; n++) {
			u64 addr = hole_start + order[n] * BIT_ULL(aligned_size);
			intel_wakeref_t wakeref;

			GEM_BUG_ON(addr + BIT_ULL(size) > vm->total);
			with_intel_gt_pm(vm->gt, wakeref)
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
	int err;

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

	err = func(&ppgtt->vm, 0, ppgtt->vm.total, end_time);

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

int i915_gem_gtt_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_ppgtt_lowlevel),
		SUBTEST(igt_ppgtt_drunk),
		SUBTEST(igt_ppgtt_walk),
		SUBTEST(igt_ppgtt_pot),
		SUBTEST(igt_ppgtt_fill),
		SUBTEST(igt_ppgtt_shrink),
		SUBTEST(igt_ppgtt_shrink_boom),
		SUBTEST(igt_ggtt_lowlevel),
		SUBTEST(igt_ggtt_drunk),
		SUBTEST(igt_ggtt_walk),
		SUBTEST(igt_ggtt_pot),
		SUBTEST(igt_ggtt_fill),
	};

	GEM_BUG_ON(offset_in_page(to_gt(i915)->ggtt->vm.total));

	return i915_live_subtests(tests, i915);
}
