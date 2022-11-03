// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "i915_selftest.h"

#include "i915_gem_ww.h"
#include "intel_engine_regs.h"
#include "intel_gpu_commands.h"
#include "intel_context.h"
#include "intel_gt.h"
#include "intel_ring.h"

#include "selftests/igt_flush_test.h"

static int direct_op(struct intel_gt *gt, int (*op)(struct intel_context *ce, struct drm_i915_gem_object *obj))
{
	struct intel_engine_cs *engine;
	struct drm_i915_gem_object *obj;
	enum intel_engine_id id;
	int err = 0;
	void *va;

	/*
	 * We create a direct 1:1 mapping of device memory into the
	 * kernel's vm. This allows us to write directly into lmem
	 * without having to bind any vma by simply writing to its
	 * device address.
	 */

	if (!drm_mm_node_allocated(&gt->flat))
		return 0;

	obj = intel_gt_object_create_lmem(gt, rounddown_pow_of_two(gt->lmem->total - 1), 0);
	if (IS_ERR(obj))
		return 0;

	pr_info("Created an %zd MiB lmem object on gt%d\n",
		obj->base.size >> 20, gt->info.id);

	va = i915_gem_object_pin_map_unlocked(obj, I915_MAP_WC);
	if (IS_ERR(va)) {
		err = PTR_ERR(va);
		goto out;
	}

	for_each_engine(engine, gt, id) {
		struct intel_context *ce;
		struct i915_gem_ww_ctx ww;

		ce = intel_context_create(engine);
		if (IS_ERR(ce))
			return PTR_ERR(ce);

		if (ce->vm != gt->vm) {
			intel_context_put(ce);
			return -ENXIO;
		}

		for_i915_gem_ww(&ww, err, true)
			err = intel_context_pin_ww(ce, &ww);
		if (err == 0) {
			err = op(ce, obj);
			intel_context_unpin(ce);
		}
		intel_context_put(ce);
		if (err)
			break;
	}

out:
	i915_gem_object_put(obj);
	if (igt_flush_test(gt->i915))
		err = -EIO;
	return err;
}

static int __direct_store(struct intel_context *ce,
			  struct drm_i915_gem_object *obj)
{
	u32 * const va = page_mask_bits(obj->mm.mapping);
	const int count = ilog2(obj->base.size) - PAGE_SHIFT;
	struct i915_request *rq;
	long timeout;
	int i, err;
	u32 *cs;

	rq = i915_request_create(ce);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	cs = intel_ring_begin(rq, count * 2 * 4);
	if (IS_ERR(cs)) {
		i915_request_add(rq);
		return PTR_ERR(cs);
	}

	for (i = 0; i <= count; i++) {
		unsigned long page;
		u64 address;

		page = BIT(i) - 1;
		va[page * PAGE_SIZE / sizeof(*va)] = STACK_MAGIC;
		address = i915_gem_object_get_dma_address(obj, page);

		*cs++ = MI_STORE_DWORD_IMM_GEN4;
		*cs++ = lower_32_bits(address);
		*cs++ = upper_32_bits(address);
		*cs++ = i;

		if (i > 0 && i < count) {
			page = BIT(i);
			va[page * PAGE_SIZE / sizeof(*va)] = STACK_MAGIC;
			address = i915_gem_object_get_dma_address(obj, page);

			*cs++ = MI_STORE_DWORD_IMM_GEN4;
			*cs++ = lower_32_bits(address);
			*cs++ = upper_32_bits(address);
			*cs++ = ~i;
		}
	}

	intel_ring_advance(rq, cs);

	i915_request_get(rq);
	i915_request_add(rq);
	timeout = i915_request_wait(rq, I915_WAIT_INTERRUPTIBLE, HZ);
	i915_request_put(rq);
	if (timeout < 0)
		return timeout;

	err = 0;
	for (i = 0; i <= count; i++) {
		unsigned long page;
		u32 value;

		page = BIT(i) - 1;
		value = va[page * PAGE_SIZE / sizeof(*va)];
		if (value != i) {
			pr_err("%s: Invalid found:%x, expected:%x at page:%lx, dma-address:%llx\n",
			       ce->engine->name,
			       value, i, page,
			       (u64)i915_gem_object_get_dma_address(obj, page));
			err = -EINVAL;
		}

		if (i > 0 && i < count) {
			page = BIT(i);
			value = va[page * PAGE_SIZE / sizeof(*va)];
			if (value != ~i) {
				pr_err("%s: Invalid found:%x, expected:%x found at page:%lx, dma-address:%llx\n",
						ce->engine->name,
						value, ~i, page,
						(u64)i915_gem_object_get_dma_address(obj, page));
				err = -EINVAL;
			}
		}
	}

	return err;
}

static int direct_store(void *arg)
{
	return direct_op(arg, __direct_store);
}

static int __direct_mov(struct intel_context *ce,
			struct drm_i915_gem_object *obj)
{
	u32 * const va = page_mask_bits(obj->mm.mapping);
	const int count = ilog2(obj->base.size) - PAGE_SHIFT;
	struct i915_request *rq;
	long timeout;
	int i, err;
	u32 *cs;

	rq = i915_request_create(ce);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	cs = intel_ring_begin(rq, count * 2 * 6);
	if (IS_ERR(cs)) {
		i915_request_add(rq);
		return PTR_ERR(cs);
	}

	for (i = 0; i <= count; i++) {
		unsigned long page;
		u64 address;

		page = BIT(i) - 1;
		va[page * PAGE_SIZE / sizeof(*va)] = STACK_MAGIC;
		address = i915_gem_object_get_dma_address(obj, page);

		*cs++ = MI_LOAD_REGISTER_IMM(1) | MI_LRI_LRM_CS_MMIO;
		*cs++ = i915_mmio_reg_offset(GEN8_RING_CS_GPR(0, 0));
		*cs++ = i;

		*cs++ = MI_ATOMIC | MI_ATOMIC_MOVE;
		*cs++ = lower_32_bits(address);
		*cs++ = upper_32_bits(address);

		if (i > 0 && i < count) {
			page = BIT(i);
			va[page * PAGE_SIZE / sizeof(*va)] = STACK_MAGIC;
			address = i915_gem_object_get_dma_address(obj, page);

			*cs++ = MI_LOAD_REGISTER_IMM(1) | MI_LRI_LRM_CS_MMIO;
			*cs++ = i915_mmio_reg_offset(GEN8_RING_CS_GPR(0, 0));
			*cs++ = ~i;

			*cs++ = MI_ATOMIC | MI_ATOMIC_MOVE;
			*cs++ = lower_32_bits(address);
			*cs++ = upper_32_bits(address);
		}
	}

	intel_ring_advance(rq, cs);

	i915_request_get(rq);
	i915_request_add(rq);
	timeout = i915_request_wait(rq, I915_WAIT_INTERRUPTIBLE, HZ);
	i915_request_put(rq);
	if (timeout < 0)
		return timeout;

	err = 0;
	for (i = 0; i <= count; i++) {
		unsigned long page;
		u32 value;

		page = BIT(i) - 1;
		value = va[page * PAGE_SIZE / sizeof(*va)];
		if (value != i) {
			pr_err("%s: Invalid found:%x, expected:%x at page:%lx, dma-address:%llx\n",
			       ce->engine->name,
			       value, i, page,
			       (u64)i915_gem_object_get_dma_address(obj, page));
			err = -EINVAL;
		}

		if (i > 0 && i < count) {
			page = BIT(i);
			value = va[page * PAGE_SIZE / sizeof(*va)];
			if (value != ~i) {
				pr_err("%s: Invalid found:%x, expected:%x found at page:%lx, dma-address:%llx\n",
						ce->engine->name,
						value, ~i, page,
						(u64)i915_gem_object_get_dma_address(obj, page));
				err = -EINVAL;
			}
		}
	}

	return err;
}

static int direct_mov(void *arg)
{
	return direct_op(arg, __direct_mov);
}

static int __direct_inc(struct intel_context *ce,
			struct drm_i915_gem_object *obj)
{
	u32 * const va = page_mask_bits(obj->mm.mapping);
	const int count = ilog2(obj->base.size) - PAGE_SHIFT;
	struct i915_request *rq;
	long timeout;
	int i, err;
	u32 *cs;

	rq = i915_request_create(ce);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	cs = intel_ring_begin(rq, count * 2 * 3);
	if (IS_ERR(cs)) {
		i915_request_add(rq);
		return PTR_ERR(cs);
	}

	for (i = 0; i <= count; i++) {
		unsigned long page;
		u64 address;

		page = BIT(i) - 1;
		va[page * PAGE_SIZE / sizeof(*va)] = i;
		address = i915_gem_object_get_dma_address(obj, page);

		*cs++ = MI_ATOMIC | MI_ATOMIC_INC;
		*cs++ = lower_32_bits(address);
		*cs++ = upper_32_bits(address);

		if (i > 0 && i < count) {
			page = BIT(i);
			va[page * PAGE_SIZE / sizeof(*va)] = i;
			address = i915_gem_object_get_dma_address(obj, page);

			*cs++ = MI_ATOMIC | MI_ATOMIC_INC;
			*cs++ = lower_32_bits(address);
			*cs++ = upper_32_bits(address);
		}
	}

	intel_ring_advance(rq, cs);

	i915_request_get(rq);
	i915_request_add(rq);
	timeout = i915_request_wait(rq, I915_WAIT_INTERRUPTIBLE, HZ);
	i915_request_put(rq);
	if (timeout < 0)
		return timeout;

	err = 0;
	for (i = 0; i <= count; i++) {
		unsigned long page;
		u32 value;

		page = BIT(i) - 1;
		value = va[page * PAGE_SIZE / sizeof(*va)];
		if (value != i + 1) {
			pr_err("%s: Invalid found:%x, expected:%x at page:%lx, dma-address:%llx\n",
			       ce->engine->name,
			       value, i + 1, page,
			       (u64)i915_gem_object_get_dma_address(obj, page));
			err = -EINVAL;
		}

		if (i > 0 && i < count) {
			page = BIT(i);
			value = va[page * PAGE_SIZE / sizeof(*va)];
			if (value != i + 1) {
				pr_err("%s: Invalid found:%x, expected:%x found at page:%lx, dma-address:%llx\n",
						ce->engine->name,
						value, i + 1, page,
						(u64)i915_gem_object_get_dma_address(obj, page));
				err = -EINVAL;
			}
		}
	}

	return err;
}

static int direct_inc(void *arg)
{
	return direct_op(arg, __direct_inc);
}

static int __direct_dec(struct intel_context *ce,
			struct drm_i915_gem_object *obj)
{
	u32 * const va = page_mask_bits(obj->mm.mapping);
	const int count = ilog2(obj->base.size) - PAGE_SHIFT;
	struct i915_request *rq;
	long timeout;
	int i, err;
	u32 *cs;

	rq = i915_request_create(ce);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	cs = intel_ring_begin(rq, count * 2 * 3);
	if (IS_ERR(cs)) {
		i915_request_add(rq);
		return PTR_ERR(cs);
	}

	for (i = 0; i <= count; i++) {
		unsigned long page;
		u64 address;

		page = BIT(i) - 1;
		va[page * PAGE_SIZE / sizeof(*va)] = i;
		address = i915_gem_object_get_dma_address(obj, page);

		*cs++ = MI_ATOMIC | MI_ATOMIC_DEC;
		*cs++ = lower_32_bits(address);
		*cs++ = upper_32_bits(address);

		if (i > 0 && i < count) {
			page = BIT(i);
			va[page * PAGE_SIZE / sizeof(*va)] = i;
			address = i915_gem_object_get_dma_address(obj, page);

			*cs++ = MI_ATOMIC | MI_ATOMIC_DEC;
			*cs++ = lower_32_bits(address);
			*cs++ = upper_32_bits(address);
		}
	}

	intel_ring_advance(rq, cs);

	i915_request_get(rq);
	i915_request_add(rq);
	timeout = i915_request_wait(rq, I915_WAIT_INTERRUPTIBLE, HZ);
	i915_request_put(rq);
	if (timeout < 0)
		return timeout;

	err = 0;
	for (i = 0; i <= count; i++) {
		unsigned long page;
		u32 value;

		page = BIT(i) - 1;
		value = va[page * PAGE_SIZE / sizeof(*va)];
		if (value != i - 1) {
			pr_err("%s: Invalid found:%x, expected:%x at page:%lx, dma-address:%llx\n",
			       ce->engine->name,
			       value, i - 1, page,
			       (u64)i915_gem_object_get_dma_address(obj, page));
			err = -EINVAL;
		}

		if (i > 0 && i < count) {
			page = BIT(i);
			value = va[page * PAGE_SIZE / sizeof(*va)];
			if (value != i - 1) {
				pr_err("%s: Invalid found:%x, expected:%x found at page:%lx, dma-address:%llx\n",
						ce->engine->name,
						value, i - 1, page,
						(u64)i915_gem_object_get_dma_address(obj, page));
				err = -EINVAL;
			}
		}
	}

	return err;
}

static int direct_dec(void *arg)
{
	return direct_op(arg, __direct_dec);
}

int intel_gtt_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(direct_store),
		SUBTEST(direct_mov),
		SUBTEST(direct_inc),
		SUBTEST(direct_dec),
	};
	struct intel_gt *gt;
	unsigned int i;

	for_each_gt(gt, i915, i) {
		int err;

		if (intel_gt_is_wedged(gt))
			continue;

		err = intel_gt_live_subtests(tests, gt);
		if (err)
			return err;
	}

	return 0;
}
