/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2008-2012 Intel Corporation
 */

#include <linux/errno.h>
#include <linux/mutex.h>

#include <drm/drm_mm.h>
#include <drm/i915_drm.h>

#include "gem/i915_gem_lmem.h"
#include "gem/i915_gem_region.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_mcr.h"
#include "gt/intel_gt_regs.h"
#include "gt/intel_region_lmem.h"
#include "i915_drv.h"
#include "i915_gem_stolen.h"
#include "i915_pci.h"
#include "i915_reg.h"
#include "i915_utils.h"
#include "intel_mchbar_regs.h"
#include "intel_pci_config.h"

/*
 * The BIOS typically reserves some of the system's memory for the exclusive
 * use of the integrated graphics. This memory is no longer available for
 * use by the OS and so the user finds that his system has less memory
 * available than he put in. We refer to this memory as stolen.
 *
 * The BIOS will allocate its framebuffer from the stolen memory. Our
 * goal is try to reuse that object for our own fbcon which must always
 * be available for panics. Anything else we can reuse the stolen memory
 * for is a boon.
 */

int i915_gem_stolen_insert_node_in_range(struct drm_i915_private *i915,
					 struct drm_mm_node *node, u64 size,
					 unsigned alignment, u64 start, u64 end)
{
	int ret;

	if (!drm_mm_initialized(&i915->mm.stolen))
		return -ENODEV;

	/* WaSkipStolenMemoryFirstPage:bdw+ */
	if (start < 4096)
		start = 4096;

	mutex_lock(&i915->mm.stolen_lock);
	ret = drm_mm_insert_node_in_range(&i915->mm.stolen, node,
					  size, alignment, 0,
					  start, end, DRM_MM_INSERT_BEST);
	mutex_unlock(&i915->mm.stolen_lock);

	return ret;
}

int i915_gem_stolen_insert_node(struct drm_i915_private *i915,
				struct drm_mm_node *node, u64 size,
				unsigned alignment)
{
	return i915_gem_stolen_insert_node_in_range(i915, node,
						    size, alignment,
						    I915_GEM_STOLEN_BIAS,
						    U64_MAX);
}

void i915_gem_stolen_remove_node(struct drm_i915_private *i915,
				 struct drm_mm_node *node)
{
	mutex_lock(&i915->mm.stolen_lock);
	drm_mm_remove_node(node);
	mutex_unlock(&i915->mm.stolen_lock);
}

static bool is_dsm_invalid(struct drm_i915_private *i915, struct resource *dsm)
{
	if (!HAS_BAR2_SMEM_STOLEN(i915)) {
		if (dsm->start == 0)
			return true;
	}

	if (dsm->end <= dsm->start)
		return true;

	return false;
}

static int i915_adjust_stolen(struct intel_memory_region *mem,
			      struct resource *dsm)
{
	struct drm_i915_private *i915 = mem->i915;
	struct resource *r;

	if (is_dsm_invalid(i915, dsm))
		return -EINVAL;

	/*
	 * TODO: We have yet too encounter the case where the GTT wasn't at the
	 * end of stolen. With that assumption we could simplify this.
	 */

	/*
	 * With stolen lmem, we don't need to check if the address range
	 * overlaps with the non-stolen system memory range, since lmem is local
	 * to the gpu.
	 */
	if (HAS_LMEM(i915) || HAS_BAR2_SMEM_STOLEN(i915))
		return 0;

	/*
	 * Verify that nothing else uses this physical address. Stolen
	 * memory should be reserved by the BIOS and hidden from the
	 * kernel. So if the region is already marked as busy, something
	 * is seriously wrong.
	 */
	r = devm_request_mem_region(i915->drm.dev, dsm->start,
				    resource_size(dsm),
				    "Graphics Stolen Memory");
	if (r == NULL) {
		/*
		 * One more attempt but this time requesting region from
		 * start + 1, as we have seen that this resolves the region
		 * conflict with the PCI Bus.
		 * This is a BIOS w/a: Some BIOS wrap stolen in the root
		 * PCI bus, but have an off-by-one error. Hence retry the
		 * reservation starting from 1 instead of 0.
		 * There's also BIOS with off-by-one on the other end.
		 */
		r = devm_request_mem_region(i915->drm.dev, dsm->start + 1,
					    resource_size(dsm) - 2,
					    "Graphics Stolen Memory");
		/*
		 * GEN3 firmware likes to smash pci bridges into the stolen
		 * range. Apparently this works.
		 */
		if (!r && GRAPHICS_VER(i915) != 3) {
			drm_err(&i915->drm,
				"conflict detected with stolen region: %pR\n",
				dsm);

			return -EBUSY;
		}
	}

	return 0;
}

static void i915_gem_cleanup_stolen(struct drm_i915_private *i915)
{
	if (!drm_mm_initialized(&i915->mm.stolen))
		return;

	drm_mm_takedown(&i915->mm.stolen);
}

static void icl_get_stolen_reserved(struct drm_i915_private *i915,
				    struct intel_uncore *uncore,
				    resource_size_t *base,
				    resource_size_t *size)
{
	u64 reg_val = intel_uncore_read64(uncore, GEN6_STOLEN_RESERVED);

	drm_dbg(&i915->drm, "GEN6_STOLEN_RESERVED = 0x%016llx\n", reg_val);

	switch (reg_val & GEN8_STOLEN_RESERVED_SIZE_MASK) {
	case GEN8_STOLEN_RESERVED_1M:
		*size = 1024 * 1024;
		break;
	case GEN8_STOLEN_RESERVED_2M:
		*size = 2 * 1024 * 1024;
		break;
	case GEN8_STOLEN_RESERVED_4M:
		*size = 4 * 1024 * 1024;
		break;
	case GEN8_STOLEN_RESERVED_8M:
		*size = 8 * 1024 * 1024;
		break;
	default:
		*size = 8 * 1024 * 1024;
		MISSING_CASE(reg_val & GEN8_STOLEN_RESERVED_SIZE_MASK);
	}

	if ((GRAPHICS_VER_FULL(i915) >= IP_VER(12, 70)) && !IS_DGFX(i915))
		/* the base is initialized to stolen top so subtract size to get base */
		*base -= *size;
	else
		*base = reg_val & GEN11_STOLEN_RESERVED_ADDR_MASK;
}

static int i915_gem_init_stolen(struct intel_memory_region *mem)
{
	struct drm_i915_private *i915 = mem->i915;
	struct intel_uncore *uncore = mem->gt->uncore;
	resource_size_t reserved_base, stolen_top;
	resource_size_t reserved_total, reserved_size;

	mutex_init(&i915->mm.stolen_lock);

	if (resource_size(&mem->region) == 0)
		return 0;

	i915->dsm = mem->region;

	if (i915_adjust_stolen(mem, &i915->dsm))
		return 0;

	GEM_BUG_ON(is_dsm_invalid(i915, &i915->dsm));

	stolen_top = i915->dsm.end + 1;
	reserved_base = stolen_top;
	reserved_size = 0;

	icl_get_stolen_reserved(i915, uncore,
				&reserved_base, &reserved_size);

	/*
	 * Our expectation is that the reserved space is at the top of the
	 * stolen region and *never* at the bottom. If we see !reserved_base,
	 * it likely means we failed to read the registers correctly.
	 */
	if (!reserved_base) {
		drm_err(&i915->drm,
			"inconsistent reservation %pa + %pa; ignoring\n",
			&reserved_base, &reserved_size);
		reserved_base = stolen_top;
		reserved_size = 0;
	}

	i915->dsm_reserved =
		(struct resource)DEFINE_RES_MEM(reserved_base, reserved_size);

	if (!resource_contains(&i915->dsm, &i915->dsm_reserved)) {
		drm_err(&i915->drm,
			"Stolen reserved area %pR outside stolen memory %pR\n",
			&i915->dsm_reserved, &i915->dsm);
		return 0;
	}

	/* Exclude the reserved region from driver use */
	mem->region.end = reserved_base - 1;
	mem->io_size = min(mem->io_size, resource_size(&mem->region));

	/* It is possible for the reserved area to end before the end of stolen
	 * memory, so just consider the start. */
	reserved_total = stolen_top - reserved_base;

	i915->stolen_usable_size =
		resource_size(&i915->dsm) - reserved_total;

	drm_dbg(&i915->drm,
		"Memory reserved for graphics device: %lluK, usable: %lluK\n",
		(u64)resource_size(&i915->dsm) >> 10,
		(u64)i915->stolen_usable_size >> 10);

	if (i915->stolen_usable_size == 0)
		return 0;

	/* Basic memrange allocator for stolen space. */
	drm_mm_init(&i915->mm.stolen, 0, i915->stolen_usable_size);

	return 0;
}

static struct sg_table *
i915_pages_create_for_stolen(struct drm_device *dev,
			     resource_size_t offset, resource_size_t size)
{
	struct drm_i915_private *i915 = to_i915(dev);
	struct sg_table *st;
	struct scatterlist *sg;

	GEM_BUG_ON(range_overflows(offset, size, resource_size(&i915->dsm)));

	/* We hide that we have no struct page backing our stolen object
	 * by wrapping the contiguous physical allocation with a fake
	 * dma mapping in a single scatterlist.
	 */

	st = kmalloc(sizeof(*st), GFP_KERNEL);
	if (st == NULL)
		return ERR_PTR(-ENOMEM);

	if (sg_alloc_table(st, 1, GFP_KERNEL)) {
		kfree(st);
		return ERR_PTR(-ENOMEM);
	}

	sg = st->sgl;
	sg->offset = 0;
	sg->length = size;

	sg_dma_address(sg) = (dma_addr_t)i915->dsm.start + offset;
	sg_dma_len(sg) = size;

	return st;
}

static int i915_gem_object_get_pages_stolen(struct drm_i915_gem_object *obj)
{
	struct sg_table *pages =
		i915_pages_create_for_stolen(obj->base.dev,
					     obj->stolen->start,
					     obj->stolen->size);
	if (IS_ERR(pages))
		return PTR_ERR(pages);

	__i915_gem_object_set_pages(obj, pages, obj->stolen->size);
	return 0;
}

static int i915_gem_object_put_pages_stolen(struct drm_i915_gem_object *obj,
					     struct sg_table *pages)
{
	/* Should only be called from i915_gem_object_release_stolen() */

	sg_free_table(pages);
	kfree(pages);
	return 0;
}

static void
i915_gem_object_release_stolen(struct drm_i915_gem_object *obj)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct drm_mm_node *stolen = fetch_and_zero(&obj->stolen);

	GEM_BUG_ON(!stolen);
	i915_gem_stolen_remove_node(i915, stolen);
	kfree(stolen);

	i915_gem_object_release_memory_region(obj);
}

static const struct drm_i915_gem_object_ops i915_gem_object_stolen_ops = {
	.name = "i915_gem_object_stolen",
	.get_pages = i915_gem_object_get_pages_stolen,
	.put_pages = i915_gem_object_put_pages_stolen,
	.release = i915_gem_object_release_stolen,
};

static int __i915_gem_object_create_stolen(struct intel_memory_region *mem,
					   struct drm_i915_gem_object *obj,
					   struct drm_mm_node *stolen)
{
	unsigned int cache_level;
	unsigned int flags;
	int err;

	/*
	 * Stolen objects are always physically contiguous since we just
	 * allocate one big block underneath using the drm_mm range allocator.
	 */
	flags = I915_BO_ALLOC_CONTIGUOUS;

	drm_gem_private_object_init(&mem->i915->drm, &obj->base, stolen->size);
	i915_gem_object_init(obj, &i915_gem_object_stolen_ops, flags);

	obj->stolen = stolen;
	cache_level = HAS_LLC(mem->i915) ? I915_CACHE_LLC : I915_CACHE_NONE;
	i915_gem_object_set_cache_coherency(obj, cache_level);

	if (WARN_ON(!i915_gem_object_trylock(obj)))
		return -EBUSY;

	i915_gem_object_init_memory_region(obj, mem);

	err = i915_gem_object_pin_pages(obj);
	if (err) {
		i915_gem_object_release_memory_region(obj);
		i915_gem_object_unlock(obj);
		return err;
	}

	i915_gem_object_unlock(obj);

	return 0;
}

static int _i915_gem_object_stolen_init(struct intel_memory_region *mem,
					struct drm_i915_gem_object *obj,
					resource_size_t size,
					unsigned int flags)
{
	struct drm_i915_private *i915 = mem->i915;
	struct drm_mm_node *stolen;
	int ret;

	if (!drm_mm_initialized(&i915->mm.stolen))
		return -ENODEV;

	if (size == 0)
		return -EINVAL;

	stolen = kzalloc(sizeof(*stolen), GFP_KERNEL);
	if (!stolen)
		return -ENOMEM;

	ret = i915_gem_stolen_insert_node(i915, stolen, size,
					  mem->min_page_size);
	if (ret)
		goto err_free;

	ret = __i915_gem_object_create_stolen(mem, obj, stolen);
	if (ret)
		goto err_remove;

	return 0;

err_remove:
	i915_gem_stolen_remove_node(i915, stolen);
err_free:
	kfree(stolen);
	return ret;
}

static int init_stolen_smem(struct intel_memory_region *mem)
{
	/*
	 * Initialise stolen early so that we may reserve preallocated
	 * objects for the BIOS to KMS transition.
	 */
	return i915_gem_init_stolen(mem);
}

static void release_stolen_smem(struct intel_memory_region *mem)
{
	i915_gem_cleanup_stolen(mem->i915);
}

static const struct intel_memory_region_ops i915_region_stolen_smem_ops = {
	.init = init_stolen_smem,
	.release = release_stolen_smem,
	.init_object = _i915_gem_object_stolen_init,
};

static int init_stolen_lmem(struct intel_memory_region *mem)
{
	int err;

	if (GEM_WARN_ON(resource_size(&mem->region) == 0))
		return -ENODEV;

	/*
	 * TODO: For stolen lmem we mostly just care about populating the dsm
	 * related bits and setting up the drm_mm allocator for the range.
	 * Perhaps split up i915_gem_init_stolen() for this.
	 */
	err = i915_gem_init_stolen(mem);
	if (err)
		return err;

	if (mem->io_size && !io_mapping_init_wc(&mem->iomap,
						mem->io_start,
						mem->io_size)) {
		err = -EIO;
		goto err_cleanup;
	}

	return 0;

err_cleanup:
	i915_gem_cleanup_stolen(mem->i915);
	return err;
}

static void release_stolen_lmem(struct intel_memory_region *mem)
{
	if (mem->io_size)
		io_mapping_fini(&mem->iomap);
	i915_gem_cleanup_stolen(mem->i915);
}

static const struct intel_memory_region_ops i915_region_stolen_lmem_ops = {
	.init = init_stolen_lmem,
	.release = release_stolen_lmem,
	.init_object = _i915_gem_object_stolen_init,
};

static int get_mtl_gms_size(struct intel_uncore *uncore)
{
	u16 ggc, gms;

	ggc = intel_uncore_read16(uncore, _MMIO(0x108040));

	/* check GGMS, should be fixed 0x3 (8MB) */
	if ((ggc & 0xc0) != 0xc0)
		return -EIO;

	/* return valid GMS value, -EIO if invalid */
	gms = ggc >> 8;
	switch (gms) {
	case 0x0 ... 0x10:
		return gms * 32;
	case 0x20:
		return 1024;
	case 0x30:
		return 1536;
	case 0x40:
		return 2048;
	case 0xf0 ... 0xfe:
		return (gms - 0xf0 + 1) * 4;
	default:
		return -EIO;
	}
}

static struct intel_memory_region *
stolen_lmem_setup(struct intel_gt *gt, u16 type,  u16 instance)
{
	struct intel_uncore *uncore = gt->uncore;
	struct drm_i915_private *i915 = gt->i915;
	struct pci_dev *pdev = to_pci_dev(i915->drm.dev);
	resource_size_t dsm_size, dsm_base, lmem_size, lmem_base;
	struct intel_memory_region *mem;
	resource_size_t io_start, io_size;
	resource_size_t min_page_size;
	int ret;

	if (WARN_ON_ONCE(instance))
		return ERR_PTR(-ENODEV);

	if (!i915_pci_resource_valid(pdev, GFXMEM_BAR))
		return ERR_PTR(-ENXIO);

	ret = intel_get_tile_range(gt, &lmem_base, &lmem_size);
	if (ret)
		return ERR_PTR(ret);

	if (HAS_BAR2_SMEM_STOLEN(i915)) {
		/*
		 * MTL dsm size is in GGC register, not the bar size.
		 * also MTL uses offset to DSMBASE in ptes, so i915
		 * uses dsm_base = 0 to setup stolen region.
		 */
		ret = get_mtl_gms_size(uncore);
		if (ret < 0) {
			drm_err(&i915->drm, "invalid MTL GGC register setting\n");
			return ERR_PTR(ret);
		}

		dsm_base = 0;
		dsm_size = mul_u32_u32(ret, SZ_1M);

		GEM_BUG_ON(pci_resource_len(pdev, GFXMEM_BAR) != SZ_256M);
		GEM_BUG_ON((dsm_size + SZ_8M) > lmem_size);
	} else {
		/* Use DSM base address instead for stolen memory */
		dsm_base = intel_uncore_read64(uncore, GEN12_DSMBASE);
		if (WARN_ON(lmem_size < dsm_base))
			return ERR_PTR(-ENODEV);
		dsm_size = lmem_size - dsm_base;
	}

	io_size = dsm_size;
	if (pci_resource_len(pdev, GFXMEM_BAR) < dsm_size) {
		io_start = 0;
		io_size = 0;
	} else if (HAS_BAR2_SMEM_STOLEN(i915)) {
		io_start = pci_resource_start(pdev, GFXMEM_BAR) + SZ_8M;
	} else {
		io_start = pci_resource_start(pdev, GFXMEM_BAR) + dsm_base;
	}

	min_page_size = HAS_64K_PAGES(i915) ? I915_GTT_PAGE_SIZE_64K :
						I915_GTT_PAGE_SIZE_4K;

	mem = intel_memory_region_create(gt, dsm_base, dsm_size,
					 min_page_size,
					 io_start, io_size,
					 type, instance,
					 &i915_region_stolen_lmem_ops);
	if (IS_ERR(mem))
		return mem;

	/*
	 * TODO: consider creating common helper to just print all the
	 * interesting stuff from intel_memory_region, which we can use for all
	 * our probed regions.
	 */

	drm_dbg(&i915->drm, "Stolen Local memory IO start: %pa\n",
		&mem->io_start);
	drm_dbg(&i915->drm,
		"Local Memory base: %pa, Stolen Local DSM base: %pa\n",
		&lmem_base, &dsm_base);

	intel_memory_region_set_name(mem, "stolen-local");

	mem->private = true;
	return mem;
}

static struct intel_memory_region*
stolen_smem_setup(struct intel_gt *gt, u16 type, u16 instance)
{
	struct intel_memory_region *mem;

	mem = intel_memory_region_create(gt,
					 intel_graphics_stolen_res.start,
					 resource_size(&intel_graphics_stolen_res),
					 PAGE_SIZE, 0, 0, type, instance,
					 &i915_region_stolen_smem_ops);
	if (IS_ERR(mem))
		return mem;

	intel_memory_region_set_name(mem, "stolen-system");

	mem->private = true;
	return mem;
}

struct intel_memory_region*
i915_gem_stolen_setup(struct intel_gt *gt, u16 type, u16 instance)
{
	if (IS_DGFX(gt->i915))
		return stolen_lmem_setup(gt, type, instance);

	return stolen_smem_setup(gt, type, instance);
}

struct drm_i915_gem_object *
i915_gem_object_create_stolen_for_preallocated(struct drm_i915_private *i915,
					       resource_size_t stolen_offset,
					       resource_size_t size)
{
	struct intel_memory_region *mem = i915->mm.stolen_region;
	struct drm_i915_gem_object *obj;
	struct drm_mm_node *stolen;
	int ret;

	if (!drm_mm_initialized(&i915->mm.stolen))
		return ERR_PTR(-ENODEV);

	drm_dbg(&i915->drm,
		"creating preallocated stolen object: stolen_offset=%pa, size=%pa\n",
		&stolen_offset, &size);

	/* KISS and expect everything to be page-aligned */
	if (GEM_WARN_ON(size == 0) ||
	    GEM_WARN_ON(!IS_ALIGNED(size, mem->min_page_size)) ||
	    GEM_WARN_ON(!IS_ALIGNED(stolen_offset, mem->min_page_size)))
		return ERR_PTR(-EINVAL);

	stolen = kzalloc(sizeof(*stolen), GFP_KERNEL);
	if (!stolen)
		return ERR_PTR(-ENOMEM);

	stolen->start = stolen_offset;
	stolen->size = size;
	mutex_lock(&i915->mm.stolen_lock);
	ret = drm_mm_reserve_node(&i915->mm.stolen, stolen);
	mutex_unlock(&i915->mm.stolen_lock);
	if (ret)
		goto err_free;

	obj = i915_gem_object_alloc();
	if (!obj) {
		ret = -ENOMEM;
		goto err_stolen;
	}

	ret = __i915_gem_object_create_stolen(mem, obj, stolen);
	if (ret)
		goto err_object_free;

	i915_gem_object_set_cache_coherency(obj, I915_CACHE_NONE);
	return obj;

err_object_free:
	i915_gem_object_free(obj);
err_stolen:
	i915_gem_stolen_remove_node(i915, stolen);
err_free:
	kfree(stolen);
	return ERR_PTR(ret);
}

bool i915_gem_object_is_stolen(const struct drm_i915_gem_object *obj)
{
	return obj->ops == &i915_gem_object_stolen_ops;
}
