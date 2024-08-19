#include "selftests/i915_random.h"

static void check_scatterlist(struct drm_i915_gem_object *obj)
{
	struct scatterlist *sg;
	u64 length = 0;

	for (sg = obj->mm.pages->sgl; sg; sg = __sg_next(sg)) {
		GEM_BUG_ON(!sg_page(sg));
		length += sg->length;
	}

	GEM_BUG_ON(length != obj->base.size);
}

static int igt_shmem_clear(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct drm_i915_gem_object *obj;
	I915_RND_STATE(prng);
	void *map;
	int pfn;
	int err;

	obj = i915_gem_object_create_shmem(i915, SZ_16M);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	obj->flags |= I915_BO_CPU_CLEAR;

	map = i915_gem_object_pin_map_unlocked(obj, I915_MAP_WC);
	if (IS_ERR(map)) {
		err = PTR_ERR(obj);
		goto out;
	}
	check_scatterlist(obj);

	err = 0;
	for (pfn = 0; pfn < obj->base.size >> PAGE_SHIFT; pfn++) {
		u32 x;

		x = igt_random_offset(&prng, 0,
				      PAGE_SIZE, sizeof(x),
				      sizeof(x));
		memcpy(&x, map + x, sizeof(x));
		if (x) {
			pr_err("Found non-clear:%08x page, offset:%d\n",
			       x, pfn);
			err = -EINVAL;
			break;
		}
	}

	i915_gem_object_unpin_map(obj);
out:
	i915_gem_object_put(obj);
	return err;
}

static int __igt_shmem_swap(struct drm_i915_private *i915, bool do_swap)
{
	struct drm_i915_gem_object *obj;
	int err = 0;
	void *map;

	obj = i915_gem_object_create_shmem(i915, SZ_16M);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	i915_gem_object_lock(obj, NULL);

	map = i915_gem_object_pin_map(obj, I915_MAP_WB);
	if (IS_ERR(map)) {
		err = PTR_ERR(obj);
		goto out;
	}
	check_scatterlist(obj);

	memset(map, 0xc5, obj->base.size);
	i915_gem_object_unpin_map(obj);

	if (do_swap) {
		err = __i915_gem_object_put_pages(obj);
		if (err)
			goto out;
	}

	map = i915_gem_object_pin_map(obj, I915_MAP_WB);
	if (IS_ERR(map)) {
		err = PTR_ERR(obj);
		goto out;
	}
	check_scatterlist(obj);

	map = memchr_inv(map, 0xc5, obj->base.size);
	if (map) {
		u32 x;

		memcpy(&x, map, sizeof(x));
		pr_err("Found incorrect value:%08x at %ld\n",
		       x, map - obj->mm.mapping);
		err = -EINVAL;
	}
	i915_gem_object_unpin_map(obj);

out:
	i915_gem_object_unlock(obj);
	i915_gem_object_put(obj);
	return err;
}

static int igt_shmem_fill(void *arg)
{
	return __igt_shmem_swap(arg, false);
}

static int igt_shmem_swap(void *arg)
{
	return __igt_shmem_swap(arg, true);
}

static int igt_shmem_dma(void *arg)
{
	const unsigned long sizes[] = { SZ_4K, SZ_64K, MAX_PAGE, 0 };
	struct drm_i915_private *i915 = arg;
	struct intel_memory_region *mem =i915->mm.regions[0];
	struct i915_dma_engine *de;
	const unsigned long *sz;
	struct page *page;
	dma_addr_t dma;

	de = get_dma_engine(mem_cpu(mem));
	if (!de)
		return 0;

	page = alloc_pages_node(dev_to_node(de->dma->device->dev), GFP_KERNEL, get_order(MAX_PAGE));
	if (!page)
		return 0;

	dma = dma_map_page_attrs(de->dma->device->dev,
				 page, 0, MAX_PAGE,
				 DMA_FROM_DEVICE,
				 DMA_ATTR_SKIP_CPU_SYNC |
				 DMA_ATTR_NO_KERNEL_MAPPING |
				 DMA_ATTR_NO_WARN);
	if (!dma)
		goto out;

	for (sz = sizes; *sz; sz++) {
		struct dma_fence *f[64];
		ktime_t dt;
		int i;

		dt = -ktime_get();
		f[0] = dma_clear(de, dma, *sz);
		if (!f[0])
			goto out;

		dma_fence_wait(f[0], false);
		dt += ktime_get();

		dma_fence_put(f[0]);

		pr_info("Cleared %ld KiB using %s took %lldus [%lldMiB/s]\n",
			*sz >> 10, dma_chan_name(de->dma),
			div_u64(dt, NSEC_PER_USEC),
			div64_u64(mul_u64_u32_shr(*sz, NSEC_PER_SEC, 20), dt));

		dt = -ktime_get();
		for (i = 0; i < ARRAY_SIZE(f); i++) {
			f[i] = dma_clear(de, dma, *sz);
			if (!f[i])
				break;
		}
		for (i = 0; i < ARRAY_SIZE(f); i++) {
			if (!f[i])
				break;
			dma_fence_wait(f[i], false);
		}
		dt += ktime_get();

		pr_info("Cleared %dx%ld KiB using %s took %lldus [%lldMiB/s]\n",
			i, *sz >> 10, dma_chan_name(de->dma),
			div_u64(dt, NSEC_PER_USEC),
			div64_u64(mul_u64_u32_shr(i * *sz, NSEC_PER_SEC, 20), dt));

		while (i--)
			dma_fence_put(f[i]);
	}

out:
	__free_pages(page, get_order(MAX_PAGE));
	return 0;
}

int i915_gem_shmem_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_shmem_dma),
		SUBTEST(igt_shmem_clear),
		SUBTEST(igt_shmem_fill),
		SUBTEST(igt_shmem_swap),
	};

	return i915_live_subtests(tests, i915);
}
