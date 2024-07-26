/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2016 Intel Corporation
 */

#include "i915_drv.h"
#include "i915_selftest.h"

#include "mock_dmabuf.h"

static struct drm_i915_gem_object *
user_object_create(struct drm_i915_private *i915, size_t sz)
{
	struct drm_i915_gem_object *obj;

	obj = i915_gem_object_create_shmem(i915, sz);
	if (!IS_ERR(obj))
		obj->flags |= I915_BO_ALLOC_USER;

	return obj;
}

static int igt_dmabuf_export(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct drm_i915_gem_object *obj;
	struct dma_buf *dmabuf;

	obj = user_object_create(i915, PAGE_SIZE);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	dmabuf = i915_gem_prime_export(&obj->base, 0);
	i915_gem_object_put(obj);
	if (IS_ERR(dmabuf)) {
		pr_err("i915_gem_prime_export failed with err=%d\n",
		       (int)PTR_ERR(dmabuf));
		return PTR_ERR(dmabuf);
	}

	dma_buf_put(dmabuf);
	return 0;
}

static int igt_dmabuf_import_same_driver_lmem(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct intel_memory_region *lmem = to_gt(i915)->lmem;
	struct drm_i915_gem_object *obj;
	struct drm_gem_object *import;
	struct dma_buf *dmabuf;
	int err;

	if (!lmem)
		return 0;

	/*
	 * Asks about the device - if both sides support p2p,
	 * we can use lmem inplace.
	 */
	if (i915_p2p_distance(i915, i915->drm.dev) >= 0)
		return 0; /* skip */

	force_different_devices = true;

	obj = i915_gem_object_create_lmem(i915, PAGE_SIZE, 0);
	if (IS_ERR(obj)) {
		pr_err("__i915_gem_object_create_user failed with err=%ld\n",
		       PTR_ERR(obj));
		err = PTR_ERR(obj);
		goto out_ret;
	}

	dmabuf = i915_gem_prime_export(&obj->base, 0);
	if (IS_ERR(dmabuf)) {
		pr_err("i915_gem_prime_export failed with err=%ld\n",
		       PTR_ERR(dmabuf));
		err = PTR_ERR(dmabuf);
		goto out;
	}

	/*
	 * We expect an import of an LMEM-only object to fail with
	 * -EOPNOTSUPP because it can't be migrated to SMEM. However,
	 * if both sides support peer2peer access, then it can be used
	 * inplace from lmem.
	 */
	import = i915_gem_prime_import(&i915->drm, dmabuf);
	if (!IS_ERR(import)) {
		struct dma_buf_attachment *attach = obj->base.import_attach;

		/*
		 * Asks about the object/attachment -If both sides support p2p,
		 * we can use lmem inplace.
		 */
		if (object_to_attachment_p2p_distance(obj, attach) >= 0) {
			pr_err("this is unexpected, but ok!\n");
			err = 0;
		} else {
			pr_err("i915_gem_prime_import succeeded when it shouldn't have\n");
			err = -EINVAL;
		}
		drm_gem_object_put(import);
	} else if (PTR_ERR(import) != -EOPNOTSUPP) {
		pr_err("i915_gem_prime_import failed with the wrong err=%ld\n",
		       PTR_ERR(import));
		err = PTR_ERR(import);
	} else {
		err = 0;
	}

	dma_buf_put(dmabuf);
out:
	i915_gem_object_put(obj);
out_ret:
	force_different_devices = false;
	return err;
}

static int igt_dmabuf_import_same_driver(struct drm_i915_private *i915,
					 struct intel_memory_region **regions,
					 unsigned int num_regions)
{
	struct drm_i915_gem_object *obj, *import_obj;
	struct drm_gem_object *import;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *import_attach;
	struct sg_table *st;
	long timeout;
	int err;

	force_different_devices = true;

	obj = i915_gem_object_create_user(i915, PAGE_SIZE,
					  regions, num_regions);
	if (IS_ERR(obj)) {
		pr_err("__i915_gem_object_create_user failed with err=%ld\n",
		       PTR_ERR(obj));
		err = PTR_ERR(obj);
		goto out_ret;
	}

	dmabuf = i915_gem_prime_export(&obj->base, 0);
	if (IS_ERR(dmabuf)) {
		pr_err("i915_gem_prime_export failed with err=%ld\n",
		       PTR_ERR(dmabuf));
		err = PTR_ERR(dmabuf);
		goto out;
	}

	import = i915_gem_prime_import(&i915->drm, dmabuf);
	if (IS_ERR(import)) {
		pr_err("i915_gem_prime_import failed with err=%ld\n",
		       PTR_ERR(import));
		err = PTR_ERR(import);
		goto out_dmabuf;
	}
	import_obj = to_intel_bo(import);

	if (import == &obj->base) {
		pr_err("i915_gem_prime_import reused gem object!\n");
		err = -EINVAL;
		goto out_import;
	}

	i915_gem_object_lock(import_obj, NULL);
	err = __i915_gem_object_get_pages(import_obj);
	if (err) {
		pr_err("Different objects dma-buf get_pages failed!\n");
		i915_gem_object_unlock(import_obj);
		goto out_import;
	}

	i915_gem_object_unlock(import_obj);

	/* Now try a fake an importer */
	import_attach = dma_buf_attach(dmabuf, obj->base.dev->dev);
	if (IS_ERR(import_attach)) {
		err = PTR_ERR(import_attach);
		goto out_import;
	}

	st = dma_buf_map_attachment(import_attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(st)) {
		err = PTR_ERR(st);
		goto out_detach;
	}

	timeout = dma_resv_wait_timeout(dmabuf->resv, false, true, 5 * HZ);
	if (!timeout) {
		pr_err("dmabuf wait for exclusive fence timed out.\n");
		timeout = -ETIME;
	}
	err = timeout > 0 ? 0 : timeout;
	dma_buf_unmap_attachment(import_attach, st, DMA_BIDIRECTIONAL);
out_detach:
	dma_buf_detach(dmabuf, import_attach);
out_import:
	i915_gem_object_put(import_obj);
out_dmabuf:
	dma_buf_put(dmabuf);
out:
	i915_gem_object_put(obj);
out_ret:
	force_different_devices = false;
	return err;
}

static int igt_dmabuf_import_same_driver_smem(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct intel_memory_region *smem = i915->mm.regions[INTEL_REGION_SMEM];

	return igt_dmabuf_import_same_driver(i915, &smem, 1);
}

static int igt_dmabuf_import_same_driver_lmem_smem(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct intel_memory_region *regions[2];

	if (!to_gt(i915)->lmem)
		return 0;

	regions[0] = to_gt(i915)->lmem;
	regions[1] = i915->mm.regions[INTEL_REGION_SMEM];
	return igt_dmabuf_import_same_driver(i915, regions, 2);
}

int i915_gem_dmabuf_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_dmabuf_export),
		SUBTEST(igt_dmabuf_import_same_driver_lmem),
		SUBTEST(igt_dmabuf_import_same_driver_smem),
		SUBTEST(igt_dmabuf_import_same_driver_lmem_smem),
	};

	return i915_live_subtests(tests, i915);
}
