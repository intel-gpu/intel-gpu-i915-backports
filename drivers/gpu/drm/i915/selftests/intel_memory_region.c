// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <linux/prime_numbers.h>
#include <linux/sort.h>

#include "../i915_selftest.h"

#include "mock_drm.h"
#include "mock_region.h"

#include "gem/i915_gem_context.h"
#include "gem/i915_gem_lmem.h"
#include "gem/i915_gem_region.h"
#include "gem/selftests/igt_gem_utils.h"
#include "gem/selftests/mock_context.h"
#include "gt/intel_engine_user.h"
#include "gt/intel_gt.h"
#include "i915_buddy.h"
#include "i915_memcpy.h"
#include "selftests/igt_flush_test.h"
#include "selftests/i915_random.h"

#ifndef SZ_8G
#define SZ_8G BIT_ULL(33)
#endif

static int igt_gpu_write_dw(struct intel_context *ce,
			    struct i915_vma *vma,
			    u32 dword,
			    u32 value)
{
	return igt_gpu_fill_dw(ce, vma, dword * sizeof(u32),
			       vma->size >> PAGE_SHIFT, value);
}

static int igt_cpu_check(struct drm_i915_gem_object *obj, u32 dword, u32 val)
{
	unsigned long n = obj->base.size >> PAGE_SHIFT;
	u32 *ptr;
	int err;

	err = i915_gem_object_wait(obj, 0, MAX_SCHEDULE_TIMEOUT);
	if (err)
		return err;

	ptr = i915_gem_object_pin_map(obj, I915_MAP_WC);
	if (IS_ERR(ptr))
		return PTR_ERR(ptr);

	ptr += dword;
	while (n--) {
		if (*ptr != val) {
			pr_err("base[%u]=%08x, val=%08x\n",
			       dword, *ptr, val);
			err = -EINVAL;
			break;
		}

		ptr += PAGE_SIZE / sizeof(*ptr);
	}

	i915_gem_object_unpin_map(obj);
	return err;
}

static int igt_gpu_write(struct intel_gt *sdw_gt,
			 struct i915_gem_context *ctx,
			 struct drm_i915_gem_object *obj)
{
	struct i915_gem_engines *engines;
	struct i915_gem_engines_iter it;
	struct i915_address_space *vm;
	struct intel_context *ce;
	I915_RND_STATE(prng);
	IGT_TIMEOUT(end_time);
	unsigned int count;
	struct i915_vma *vma;
	int *order;
	int i, n;
	int err = 0;

	GEM_BUG_ON(!i915_gem_object_has_pinned_pages(obj));

	n = 0;
	count = 0;
	for_each_gem_engine(ce, i915_gem_context_lock_engines(ctx), it) {
		count++;
		if (ce->engine->gt != sdw_gt)
			continue;
		if (!intel_engine_can_store_dword(ce->engine))
			continue;

		vm = ce->vm;
		n++;
	}
	i915_gem_context_unlock_engines(ctx);
	if (!n)
		return 0;

	order = i915_random_order(count * count, &prng);
	if (!order)
		return -ENOMEM;

	vma = i915_vma_instance(obj, vm, NULL);
	if (IS_ERR(vma)) {
		err = PTR_ERR(vma);
		goto out_free;
	}

	err = i915_vma_pin(vma, 0, 0, PIN_USER);
	if (err)
		goto out_free;

	i = 0;
	engines = i915_gem_context_lock_engines(ctx);
	do {
		u32 rng = prandom_u32_state(&prng);
		u32 dword = offset_in_page(rng) / 4;

		ce = engines->engines[order[i] % engines->num_engines];
		i = (i + 1) % (count * count);
		if (!ce ||
		    ce->engine->gt != sdw_gt ||
		    !intel_engine_can_store_dword(ce->engine))
			continue;

		err = igt_gpu_write_dw(ce, vma, dword, rng);
		if (err)
			break;

		i915_gem_object_lock(obj, NULL);
		err = igt_cpu_check(obj, dword, rng);
		i915_gem_object_unlock(obj);
		if (err)
			break;
	} while (!__igt_timeout(end_time, NULL));
	i915_gem_context_unlock_engines(ctx);

out_free:
	kfree(order);

	if (err == -ENOMEM)
		err = 0;

	return err;
}

static int igt_lmem_create(void *arg)
{
	struct intel_gt *gt = arg;
	struct drm_i915_gem_object *obj;
	int err = 0;

	obj = intel_gt_object_create_lmem(gt, PAGE_SIZE, 0);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	err = i915_gem_object_pin_pages_unlocked(obj);
	if (err)
		goto out_put;

	i915_gem_object_unpin_pages(obj);
out_put:
	i915_gem_object_put(obj);

	return err;
}

static struct intel_engine_cs *
random_engine_class(struct intel_gt *gt,
		    unsigned int class,
		    struct rnd_state *prng)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	unsigned int count;

	count = 0;
	for_each_engine(engine, gt, id) {
		if (engine->class != class)
			continue;
		count++;
	}

	do {
		count = i915_prandom_u32_max_state(count, prng);
		engine = gt->engine_class[class][count];
	} while (!engine);

	return engine;
}

static int
igt_create_migrate(struct intel_gt *gt,
		   struct intel_memory_region *src,
		   struct intel_memory_region *dst)
{
	struct drm_i915_gem_object *obj;
	struct i915_gem_ww_ctx ww;
	int err = 0;

	pr_info("%s: migrating %x->%x\n", __func__, src->id, dst->id);

	/* Switch object backing-store on create */
	obj = i915_gem_object_create_region(src, max(src->min_page_size, dst->min_page_size), 0);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	/* Allow any and all migration [disable compression] */
	obj->memory_mask = -1;

	for_i915_gem_ww(&ww, err, true) {
		err = i915_gem_object_lock(obj, &ww);
		if (err)
			continue;

		err = i915_gem_object_prepare_move(obj, &ww);
		if (err)
			continue;

		err = i915_gem_object_migrate(obj, dst->id, false);
	}

	i915_gem_object_put(obj);

	return err;
}

static int igt_smem_create_migrate(void *arg)
{
	struct intel_gt *gt = arg;
	struct drm_i915_private *i915 = gt->i915;
	struct intel_memory_region *smem = i915->mm.regions[INTEL_REGION_SMEM];

	return igt_create_migrate(gt, smem, gt->lmem);
}

static int igt_lmem_create_migrate(void *arg)
{
	struct intel_gt *gt = arg;
	struct drm_i915_private *i915 = gt->i915;
	struct intel_memory_region *smem = i915->mm.regions[INTEL_REGION_SMEM];

	return igt_create_migrate(gt, gt->lmem, smem);
}

static int igt_smem_create_migrate_cross_tile(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct intel_memory_region *smem = i915->mm.regions[INTEL_REGION_SMEM];
	struct intel_gt *gt;
	unsigned int i;
	int err;

	for_each_gt(gt, i915, i) {
		err = igt_create_migrate(gt, smem, gt->lmem);
		if (err)
			return err;
	}

	return 0;
}

static int igt_lmem_create_cleared_cpu(void *arg)
{
	struct intel_gt *gt = arg;
	struct drm_i915_private *i915 = gt->i915;
	I915_RND_STATE(prng);
	IGT_TIMEOUT(end_time);
	u32 size, i;
	int err;

	i915_gem_drain_freed_objects(i915);

	size = max_t(u32, PAGE_SIZE, i915_prandom_u32_max_state(SZ_32M, &prng));
	size = round_up(size, PAGE_SIZE);
	i = 0;

	do {
		struct drm_i915_gem_object *obj;
		unsigned int flags;
		u32 dword, val;
		void *vaddr;

		/*
		 * Alternate between cleared and uncleared allocations, while
		 * also dirtying the pages each time to check that the pages are
		 * always cleared if requested, since we should get some overlap
		 * of the underlying pages, if not all, since we are the only
		 * user.
		 */

		flags = I915_BO_CPU_CLEAR;
		if (i & 1)
			flags = 0;

		obj = intel_gt_object_create_lmem(gt, size, flags);
		if (IS_ERR(obj))
			return PTR_ERR(obj);

		i915_gem_object_lock(obj, NULL);
		err = i915_gem_object_pin_pages(obj);
		if (err)
			goto out_put;

		dword = i915_prandom_u32_max_state(PAGE_SIZE / sizeof(u32),
						   &prng);

		if (flags & I915_BO_CPU_CLEAR) {
			err = igt_cpu_check(obj, dword, 0);
			if (err) {
				pr_err("%s failed with size=%u, flags=%u\n",
				       __func__, size, flags);
				goto out_unpin;
			}
		}

		vaddr = i915_gem_object_pin_map(obj, I915_MAP_WC);
		if (IS_ERR(vaddr)) {
			err = PTR_ERR(vaddr);
			goto out_unpin;
		}

		val = prandom_u32_state(&prng);

		memset32(vaddr, val, obj->base.size / sizeof(u32));

		i915_gem_object_flush_map(obj);
		i915_gem_object_unpin_map(obj);
out_unpin:
		i915_gem_object_unpin_pages(obj);
		__i915_gem_object_put_pages(obj);
out_put:
		i915_gem_object_unlock(obj);
		i915_gem_object_put(obj);

		if (err)
			break;
		++i;
	} while (!__igt_timeout(end_time, NULL));

	pr_info("%s completed (%u) iterations\n", __func__, i);

	return err;
}

static int igt_lmem_create_migrate_cross_tile(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct intel_memory_region *smem = i915->mm.regions[INTEL_REGION_SMEM];
	struct intel_gt *gt;
	unsigned int i;
	int err;

	for_each_gt(gt, i915, i) {
		err = igt_create_migrate(gt, gt->lmem, smem);
		if (err)
			return err;
	}

	return 0;
}

static int
__igt_lmem_write_gpu(struct intel_gt *gt,
		     struct intel_gt *sdw_gt,
		     struct intel_gt *vm_gt)
{
	struct drm_i915_private *i915 = gt->i915;
	struct drm_i915_gem_object *obj;
	struct i915_gem_context *ctx;
	struct file *file;
	I915_RND_STATE(prng);
	u32 sz;
	int err;

	pr_info("%s: writting to gt%u from gt%u, ppgtt at gt%u...\n",
		__func__, gt->info.id, sdw_gt->info.id, vm_gt->info.id);

	file = mock_file(i915);
	if (IS_ERR(file))
		return PTR_ERR(file);

	ctx = live_gt_context(vm_gt, file);
	if (IS_ERR(ctx)) {
		err = PTR_ERR(ctx);
		goto out_file;
	}

	sz = round_up(prandom_u32_state(&prng) % SZ_32M, PAGE_SIZE);

	obj = intel_gt_object_create_lmem(gt, sz, 0);
	if (IS_ERR(obj)) {
		err = PTR_ERR(obj);
		goto out_file;
	}

	err = i915_gem_object_pin_pages_unlocked(obj);
	if (err)
		goto out_put;

	err = igt_gpu_write(sdw_gt, ctx, obj);
	if (err)
		pr_err("igt_gpu_write failed(%d)\n", err);

	i915_gem_object_unpin_pages(obj);
out_put:
	i915_gem_object_put(obj);
out_file:
	fput(file);
	return err;
}

static int igt_lmem_write_gpu(void *arg)
{
	struct intel_gt *gt = arg;

	return __igt_lmem_write_gpu(gt, gt, gt);
}

static int igt_lmem_write_gpu_cross_tile(void *arg)
{

	struct drm_i915_private *i915 = arg;
	struct intel_gt *gt, *gt2;
	unsigned int i, j;

	for_each_gt(gt, i915, i) {
		for_each_gt(gt2, i915, j) {
			int ret;

			if (gt == gt2)
				continue;

			ret = __igt_lmem_write_gpu(gt, gt2, gt2);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int igt_lmem_write_gpu_cross_tile_cross_vm(void *arg)
{

	struct drm_i915_private *i915 = arg;
	struct intel_gt *gt, *gt2;
	unsigned int i, j;

	for_each_gt(gt, i915, i) {
		for_each_gt(gt2, i915, j) {
			int ret;

			if (gt == gt2)
				continue;

			ret = __igt_lmem_write_gpu(gt, gt2, gt);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int __igt_lmem_write_cpu(struct intel_gt *gt, struct intel_gt *bcs_gt)
{
	struct drm_i915_gem_object *obj;
	I915_RND_STATE(prng);
	IGT_TIMEOUT(end_time);
	u32 bytes[] = {
		0, /* rng placeholder */
		sizeof(u32),
		sizeof(u64),
		64, /* cl */
		PAGE_SIZE,
		PAGE_SIZE - sizeof(u32),
		PAGE_SIZE - sizeof(u64),
		PAGE_SIZE - 64,
	};
	struct intel_engine_cs *engine;
	u32 *vaddr;
	u32 sz;
	u32 i;
	int *order;
	int count;
	int err;

	engine = random_engine_class(bcs_gt, COPY_ENGINE_CLASS, &prng);
	if (!engine)
		return 0;

	pr_info("%s: using %s on gt%u\n", __func__, engine->name, gt->info.id);

	sz = round_up(prandom_u32_state(&prng) % SZ_32M, PAGE_SIZE);
	sz = max_t(u32, 2 * PAGE_SIZE, sz);

	obj = intel_gt_object_create_lmem(gt, sz, I915_BO_ALLOC_CONTIGUOUS);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	vaddr = i915_gem_object_pin_map_unlocked(obj, I915_MAP_WC);
	if (IS_ERR(vaddr)) {
		err = PTR_ERR(vaddr);
		goto out_put;
	}

	/* Put the pages into a known state -- from the gpu for added fun */
	err = i915_gem_object_clear_lmem(obj);
	if (err)
		goto out_unpin;

	err = i915_gem_object_set_to_wc_domain(obj, true);
	if (err)
		goto out_unpin;

	count = ARRAY_SIZE(bytes);
	order = i915_random_order(count * count, &prng);
	if (!order) {
		err = -ENOMEM;
		goto out_unpin;
	}

	/* A random multiple of u32, picked between [64, PAGE_SIZE - 64] */
	bytes[0] = igt_random_offset(&prng, 64, PAGE_SIZE - 64, 0, sizeof(u32));
	GEM_BUG_ON(!IS_ALIGNED(bytes[0], sizeof(u32)));

	i = 0;
	do {
		u32 offset;
		u32 align;
		u32 dword;
		u32 size;
		u32 val;

		size = bytes[order[i] % count];
		i = (i + 1) % (count * count);

		align = bytes[order[i] % count];
		i = (i + 1) % (count * count);

		align = max_t(u32, sizeof(u32), rounddown_pow_of_two(align));

		offset = igt_random_offset(&prng, 0, obj->base.size,
					   size, align);

		val = prandom_u32_state(&prng);
		memset32(vaddr + offset / sizeof(u32), val,
			 size / sizeof(u32));

		/*
		 * Sample random dw -- don't waste precious time reading every
		 * single dw.
		 */
		dword = igt_random_offset(&prng, offset,
					  offset + size,
					  sizeof(u32), sizeof(u32));
		dword /= sizeof(u32);
		if (vaddr[dword] != val) {
			pr_err("%s vaddr[%u]=%u, val=%u, size=%u, align=%u, offset=%u\n",
			       __func__, dword, vaddr[dword], val,
			       size, align, offset);
			err = -EINVAL;
			break;
		}
	} while (!__igt_timeout(end_time, NULL));

out_unpin:
	i915_gem_object_unpin_map(obj);
out_put:
	i915_gem_object_put(obj);

	return err;
}

static const char *repr_type(u32 type)
{
	switch (type) {
	case I915_MAP_WB:
		return "WB";
	case I915_MAP_WC:
		return "WC";
	}

	return "";
}

static struct drm_i915_gem_object *
create_region_for_mapping(struct intel_memory_region *mr, u64 size, u32 type,
			  void **out_addr)
{
	struct drm_i915_gem_object *obj;
	void *addr;

	obj = i915_gem_object_create_region(mr, size, 0);
	if (IS_ERR(obj)) {
		if (PTR_ERR(obj) == -ENOSPC) /* Stolen memory */
			return ERR_PTR(-ENODEV);
		return obj;
	}

	addr = i915_gem_object_pin_map_unlocked(obj, type);
	if (IS_ERR(addr)) {
		i915_gem_object_put(obj);
		if (PTR_ERR(addr) == -ENXIO)
			return ERR_PTR(-ENODEV);
		return addr;
	}

	*out_addr = addr;
	return obj;
}

static int wrap_ktime_compare(const void *A, const void *B)
{
	const ktime_t *a = A, *b = B;

	return ktime_compare(*a, *b);
}

static inline void igt_memcpy(void *dst, const void *src, size_t size)
{
	memcpy(dst, src, size);
}

static inline void igt_memcpy_from_wc(void *dst, const void *src, size_t size)
{
	i915_memcpy_from_wc(dst, src, size);
}

static int _perf_memcpy(struct intel_memory_region *src_mr,
			struct intel_memory_region *dst_mr,
			u64 size, u32 src_type, u32 dst_type)
{
	struct drm_i915_private *i915 = src_mr->i915;
	const struct {
		const char *name;
		void (*copy)(void *dst, const void *src, size_t size);
		bool skip;
	} tests[] = {
		{
			"memcpy",
			igt_memcpy,
		},
		{
			"memcpy_from_wc",
			igt_memcpy_from_wc,
			!i915_has_memcpy_from_wc(),
		},
	};
	struct drm_i915_gem_object *src, *dst;
	void *src_addr, *dst_addr;
	int ret = 0;
	int i;

	src = create_region_for_mapping(src_mr, size, src_type, &src_addr);
	if (IS_ERR(src)) {
		ret = PTR_ERR(src);
		goto out;
	}

	dst = create_region_for_mapping(dst_mr, size, dst_type, &dst_addr);
	if (IS_ERR(dst)) {
		ret = PTR_ERR(dst);
		goto out_unpin_src;
	}

	for (i = 0; i < ARRAY_SIZE(tests); ++i) {
		ktime_t t[5];
		int pass;

		if (tests[i].skip)
			continue;

		for (pass = 0; pass < ARRAY_SIZE(t); pass++) {
			ktime_t t0, t1;

			t0 = ktime_get();

			tests[i].copy(dst_addr, src_addr, size);

			t1 = ktime_get();
			t[pass] = ktime_sub(t1, t0);
		}

		sort(t, ARRAY_SIZE(t), sizeof(*t), wrap_ktime_compare, NULL);
		if (t[0] <= 0) {
			/* ignore the impossible to protect our sanity */
			pr_debug("Skipping %s src(%s, %s) -> dst(%s, %s) %14s %4lluKiB copy, unstable measurement [%lld, %lld]\n",
				 __func__,
				 src_mr->name, repr_type(src_type),
				 dst_mr->name, repr_type(dst_type),
				 tests[i].name, size >> 10,
				 t[0], t[4]);
			continue;
		}

		pr_info("%s src(%s, %s) -> dst(%s, %s) %14s %4llu KiB copy: %5lld MiB/s\n",
			__func__,
			src_mr->name, repr_type(src_type),
			dst_mr->name, repr_type(dst_type),
			tests[i].name, size >> 10,
			div64_u64(mul_u32_u32(4 * size,
					      1000 * 1000 * 1000),
				  t[1] + 2 * t[2] + t[3]) >> 20);

		cond_resched();
	}

	i915_gem_object_unpin_map(dst);
	i915_gem_object_put(dst);
out_unpin_src:
	i915_gem_object_unpin_map(src);
	i915_gem_object_put(src);

	i915_gem_drain_freed_objects(i915);
out:
	if (ret == -ENODEV)
		ret = 0;

	return ret;
}

static int perf_memcpy(void *arg)
{
	struct drm_i915_private *i915 = arg;
	static const u32 types[] = {
		I915_MAP_WB,
		I915_MAP_WC,
	};
	static const u32 sizes[] = {
		SZ_4K,
		SZ_64K,
		SZ_4M,
	};
	struct intel_memory_region *smem = i915->mm.regions[INTEL_REGION_SMEM];
	struct intel_memory_region *mr;
	int i, j, id;

	for_each_memory_region(mr, i915, id) {
		for (i = 0; i < ARRAY_SIZE(sizes); ++i) {
			for (j = 0; j < ARRAY_SIZE(types); ++j) {
				int ret;

				ret = _perf_memcpy(smem, mr, sizes[i],
						   I915_MAP_WB, types[j]);
				if (ret)
					return ret;

				ret = _perf_memcpy(mr, smem, sizes[i],
						   types[j], I915_MAP_WB);
				if (ret)
					return ret;
			}
		}
	}

	return 0;
}

static int igt_lmem_write_cpu(void *arg)
{
	struct intel_gt *gt = arg;

	return __igt_lmem_write_cpu(gt, gt);
}

static int igt_lmem_write_cpu_cross_tile(void *arg)
{

	struct drm_i915_private *i915 = arg;
	struct intel_gt *gt, *gt2;
	unsigned int i, j;

	for_each_gt(gt, i915, i) {
		for_each_gt(gt2, i915, j) {
			int ret;

			if (gt == gt2)
				continue;

			ret = __igt_lmem_write_cpu(gt, gt2);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int lmem_pages_migrate_one(struct i915_gem_ww_ctx *ww,
				  struct drm_i915_gem_object *obj,
				  struct intel_gt *gt)
{
	int err;

	err = i915_gem_object_lock(obj, ww);
	if (err)
		return err;

	err = i915_gem_object_wait(obj,
				   I915_WAIT_INTERRUPTIBLE |
				   I915_WAIT_PRIORITY |
				   I915_WAIT_ALL,
				   MAX_SCHEDULE_TIMEOUT);
	if (err)
		return err;

	err = i915_gem_object_prepare_move(obj, ww);
	if (err)
		return err;

	if (i915_gem_object_is_lmem(obj)) {
		err = i915_gem_object_migrate(obj, INTEL_REGION_SMEM, false);
		if (err)
			return err;

		if (i915_gem_object_is_lmem(obj)) {
			pr_err("object still backed by lmem\n");
			err = -EINVAL;
		}

		if (!list_empty(&obj->mm.blocks)) {
			pr_err("object leaking memory region\n");
			err = -EINVAL;
		}

		if (!i915_gem_object_has_struct_page(obj)) {
			pr_err("object not backed by struct page\n");
			err = -EINVAL;
		}
	} else {
		err = i915_gem_object_migrate(obj, gt->lmem->id, false);
		if (err)
			return err;

		if (i915_gem_object_has_struct_page(obj)) {
			pr_err("object still backed by struct page\n");
			err = -EINVAL;
		}

		if (!i915_gem_object_is_lmem(obj)) {
			pr_err("object not backed by lmem\n");
			err = -EINVAL;
		}
	}

	return err;
}

static int
__igt_lmem_pages_migrate(struct intel_gt *gt, struct intel_gt *bcs_gt)
{
	struct drm_i915_gem_object *obj;
	struct i915_gem_ww_ctx ww;
	int err;
	int i;

	/* From LMEM to shmem and back again */

	obj = intel_gt_object_create_lmem(gt, SZ_2M, 0);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	/* Allow any and all migration [disable compression] */
	obj->memory_mask = -1;

	err = i915_gem_object_clear_lmem(obj);
	if (err)
		goto out_put;

	for (i = 1; i <= 4; ++i) {
		for_i915_gem_ww(&ww, err, true)
			err = lmem_pages_migrate_one(&ww, obj, gt);
		if (err)
			break;
	}
out_put:
	i915_gem_object_put(obj);

	return err;
}

static int igt_lmem_pages_migrate(void *arg)
{
	struct intel_gt *gt = arg;

	return __igt_lmem_pages_migrate(gt, gt);
}

static int igt_lmem_pages_migrate_cross_tile(void *arg)
{

	struct drm_i915_private *i915 = arg;
	struct intel_gt *gt, *gt2;
	unsigned int i, j;

	for_each_gt(gt, i915, i) {
		for_each_gt(gt2, i915, j) {
			int ret;

			if (gt == gt2)
				continue;

			ret = __igt_lmem_pages_migrate(gt, gt2);
			if (ret)
				return ret;
		}
	}

	return 0;
}

int intel_memory_region_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_lmem_create),
		SUBTEST(igt_lmem_create_cleared_cpu),
		SUBTEST(igt_lmem_write_cpu),
		SUBTEST(igt_lmem_write_gpu),
		SUBTEST(igt_smem_create_migrate),
		SUBTEST(igt_lmem_create_migrate),
		SUBTEST(igt_lmem_pages_migrate),
	};
	struct intel_gt *gt;
	unsigned int i;
	int ret = 0;

	if (!HAS_LMEM(i915)) {
		pr_info("device lacks LMEM support, skipping\n");
		return 0;
	}

	for_each_gt(gt, i915, i) {
		if (intel_gt_is_wedged(gt))
			continue;

		ret = intel_gt_live_subtests(tests, gt);
		if (ret)
			break;
	}

	return ret;
}

int intel_memory_region_cross_tile_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_smem_create_migrate_cross_tile),
		SUBTEST(igt_lmem_create_migrate_cross_tile),
		SUBTEST(igt_lmem_pages_migrate_cross_tile),
		SUBTEST(igt_lmem_write_cpu_cross_tile),
		SUBTEST(igt_lmem_write_gpu_cross_tile),
		SUBTEST(igt_lmem_write_gpu_cross_tile_cross_vm),
	};
	struct intel_gt *gt;
	unsigned int i;

	if (!HAS_LMEM(i915)) {
		pr_info("device lacks LMEM support, skipping\n");
		return 0;
	}

	for_each_gt(gt, i915, i) {
		if (intel_gt_is_wedged(gt))
			return 0;
	}

	return i915_live_subtests(tests, i915);
}

int intel_memory_region_perf_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(perf_memcpy),
	};

	if (intel_gt_is_wedged(to_gt(i915)))
		return 0;

	return i915_live_subtests(tests, i915);
}
