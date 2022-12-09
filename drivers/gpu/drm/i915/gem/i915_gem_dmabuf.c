/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2012 Red Hat Inc
 */

#include <linux/dma-buf.h>
#include <linux/highmem.h>
#include <linux/pci-p2pdma.h>
#include <linux/scatterlist.h>
#ifdef MODULE_IMPORT_NS_SUPPORT
#include <linux/module.h>
#endif
#include <drm/intel_iaf_platform.h>

#include "gem/i915_gem_dmabuf.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_requests.h"

#include "i915_drv.h"
#include "i915_gem_lmem.h"
#include "i915_gem_mman.h"
#include "i915_gem_object.h"
#include "i915_scatterlist.h"
#include "i915_trace.h"
#include "intel_iaf.h"

#ifdef MODULE_IMPORT_NS_SUPPORT
MODULE_IMPORT_NS(DMA_BUF);
#endif

I915_SELFTEST_DECLARE(static bool force_different_devices;)

static const struct drm_i915_gem_object_ops i915_gem_object_dmabuf_ops;
static int update_fabric(struct dma_buf *dma_buf,
			 struct drm_i915_gem_object *obj);

static struct drm_i915_gem_object *dma_buf_to_obj(struct dma_buf *buf)
{
	return to_intel_bo(buf->priv);
}

static void dmabuf_unmap_addr(struct device *dev, struct scatterlist *sgl,
			      int nents, enum dma_data_direction dir,
			      unsigned long attrs)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i)
		dma_unmap_resource(dev, sg_dma_address(sg), sg_dma_len(sg),
				   dir, attrs);
}

/**
 * dmabuf_map_addr - Update LMEM address to a physical address and map the
 * resource.
 * @dev: valid device
 * @obj: valid i915 GEM object
 * @sgt: scatter gather table to apply mapping to
 * @dir: DMA direction
 *
 * The dma_address of the scatter list is the LMEM "address".  From this the
 * actual physical address can be determined.
 *
 */
static int dmabuf_map_addr(struct device *dev, struct drm_i915_gem_object *obj,
			   struct sg_table *sgt, enum dma_data_direction dir,
			   unsigned long attrs)
{
	struct intel_memory_region *mem = obj->mm.region;
	struct scatterlist *sg;
	phys_addr_t addr;
	int i;

	for_each_sg(sgt->sgl, sg, sgt->orig_nents, i) {
		if (obj->pair && i == obj->mm.pages->orig_nents)
			mem = obj->pair->mm.region;
		addr = sg_dma_address(sg) - mem->region.start + mem->io_start;
		sg->dma_address = dma_map_resource(dev, addr, sg->length, dir,
						   attrs);
		if (dma_mapping_error(dev, sg->dma_address))
			goto unmap;
		sg->dma_length = sg->length;
	}

	return 0;

unmap:
	dmabuf_unmap_addr(dev, sgt->sgl, i, dir, attrs);
	return -ENOMEM;
}

static struct sg_table *i915_gem_copy_pages(struct drm_i915_gem_object *obj)
{
	struct scatterlist *src;
	struct scatterlist *dst;
	struct sg_table *sgt;
	unsigned int nents;
	int i;

	/*
	 * Make a copy of the object's sgt, so that we can make an independent
	 * mapping.
	 * NOTE: For LMEM objects the dma entries contain the device specific
	 * address information.  This will get overwritten by dma-buf-map
	 */
	sgt = kmalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return NULL;

	nents = obj->mm.pages->orig_nents;
	if (obj->pair)
		nents += obj->pair->mm.pages->orig_nents;

	if (sg_alloc_table(sgt, nents, GFP_KERNEL)) {
		kfree(sgt);
		return NULL;
	}

	dst = sgt->sgl;
	for_each_sg(obj->mm.pages->sgl, src, obj->mm.pages->orig_nents, i) {
		sg_set_page(dst, sg_page(src), src->length, 0);
		sg_dma_address(dst) = sg_dma_address(src);
		sg_dma_len(dst) = sg_dma_len(src);
		dst = sg_next(dst);
	}

	/* If object is paired, add the pair's page info */
	if (obj->pair) {
		for_each_sg(obj->pair->mm.pages->sgl, src, obj->pair->mm.pages->orig_nents, i) {
			sg_set_page(dst, sg_page(src), src->length, 0);
			sg_dma_address(dst) = sg_dma_address(src);
			sg_dma_len(dst) = sg_dma_len(src);
			dst = sg_next(dst);
		}
	}

	return sgt;
}

static struct sg_table *i915_gem_map_dma_buf(struct dma_buf_attachment *attach,
					     enum dma_data_direction dir)
{
	struct drm_i915_gem_object *obj = dma_buf_to_obj(attach->dmabuf);
	struct sg_table *sgt;
	int ret;

	sgt = i915_gem_copy_pages(obj);
	if (!sgt) {
		ret = -ENOMEM;
		goto err;
	}

	if (i915_gem_object_is_lmem(obj))
		ret = dmabuf_map_addr(attach->dev, obj, sgt, dir,
				      DMA_ATTR_SKIP_CPU_SYNC);
	else
		ret = dma_map_sgtable(attach->dev, sgt, dir,
				      DMA_ATTR_SKIP_CPU_SYNC);
	if (ret)
		goto err_free_sgt;

	return sgt;

err_free_sgt:
	sg_free_table(sgt);
	kfree(sgt);
err:
	return ERR_PTR(ret);
}

static void i915_gem_unmap_dma_buf(struct dma_buf_attachment *attach,
				   struct sg_table *sgt,
				   enum dma_data_direction dir)
{
	struct drm_i915_gem_object *obj = dma_buf_to_obj(attach->dmabuf);

	if (i915_gem_object_is_lmem(obj))
		dmabuf_unmap_addr(attach->dev, sgt->sgl, sgt->nents, dir,
				  DMA_ATTR_SKIP_CPU_SYNC);
	else
		dma_unmap_sgtable(attach->dev, sgt, dir,
				  DMA_ATTR_SKIP_CPU_SYNC);

	sg_free_table(sgt);
	kfree(sgt);
}

static int i915_gem_dmabuf_vmap(struct dma_buf *dma_buf,
				struct iosys_map *map)
{
	struct drm_i915_gem_object *obj = dma_buf_to_obj(dma_buf);
	enum i915_map_type type;
	void *vaddr;

	type = i915_coherent_map_type(to_i915(obj->base.dev), obj, true);
	vaddr = i915_gem_object_pin_map_unlocked(obj, type);
	if (IS_ERR(vaddr))
		return PTR_ERR(vaddr);

	iosys_map_set_vaddr(map, vaddr);

	return 0;
}

static void i915_gem_dmabuf_vunmap(struct dma_buf *dma_buf,
				   struct iosys_map *map)
{
	struct drm_i915_gem_object *obj = dma_buf_to_obj(dma_buf);

	i915_gem_object_flush_map(obj);
	i915_gem_object_unpin_map(obj);
}

/**
 * i915_gem_dmabuf_update_vma - Setup VMA information for exported LMEM
 * objects
 * @obj: valid LMEM object
 * @vma: va;od vma
 *
 * NOTE: on success, the final _object_put() will be done by the VMA
 * vm_close() callback.
 */
static int i915_gem_dmabuf_update_vma(struct drm_i915_gem_object *obj,
				      struct vm_area_struct *vma)
{
	struct i915_mmap_offset *mmo;
	int err;

	i915_gem_object_get(obj);
	mmo = i915_gem_mmap_offset_attach(obj, I915_MMAP_TYPE_WC, NULL);
	if (IS_ERR(mmo)) {
		err = PTR_ERR(mmo);
		goto out;
	}

	err = i915_gem_update_vma_info(obj, mmo, vma);
	if (err)
		goto out;

	return 0;

out:
	i915_gem_object_put(obj);
	return err;
}

static int i915_gem_dmabuf_mmap(struct dma_buf *dma_buf,
				struct vm_area_struct *vma)
{
	struct drm_i915_gem_object *obj = dma_buf_to_obj(dma_buf);
	int ret;

	if (obj->base.size < vma->vm_end - vma->vm_start)
		return -EINVAL;

	/* shmem */
	if (obj->base.filp) {
		ret = call_mmap(obj->base.filp, vma);
		if (ret)
			return ret;

		vma_set_file(vma, obj->base.filp);

		return 0;
	}

	if (i915_gem_object_is_lmem(obj))
		return i915_gem_dmabuf_update_vma(obj, vma);

	return -ENODEV;
}

static int i915_gem_begin_cpu_access(struct dma_buf *dma_buf, enum dma_data_direction direction)
{
	struct drm_i915_gem_object *obj = dma_buf_to_obj(dma_buf);
	bool write = (direction == DMA_BIDIRECTIONAL || direction == DMA_TO_DEVICE);
	struct i915_gem_ww_ctx ww;
	int err;

	i915_gem_ww_ctx_init(&ww, true);
retry:
	err = i915_gem_object_lock(obj, &ww);
	if (!err)
		err = i915_gem_object_pin_pages(obj);
	if (!err) {
		if (i915_gem_object_is_lmem(obj))
			err = i915_gem_object_set_to_wc_domain(obj, write);
		else
			err = i915_gem_object_set_to_cpu_domain(obj, write);
		i915_gem_object_unpin_pages(obj);
	}
	if (err == -EDEADLK) {
		err = i915_gem_ww_ctx_backoff(&ww);
		if (!err)
			goto retry;
	}
	i915_gem_ww_ctx_fini(&ww);
	return err;
}

static int i915_gem_end_cpu_access(struct dma_buf *dma_buf, enum dma_data_direction direction)
{
	struct drm_i915_gem_object *obj = dma_buf_to_obj(dma_buf);
	struct i915_gem_ww_ctx ww;
	int err;

	i915_gem_ww_ctx_init(&ww, true);
retry:
	err = i915_gem_object_lock(obj, &ww);
	if (!err)
		err = i915_gem_object_pin_pages(obj);
	if (!err) {
		err = i915_gem_object_set_to_gtt_domain(obj, false);
		i915_gem_object_unpin_pages(obj);
	}
	if (err == -EDEADLK) {
		err = i915_gem_ww_ctx_backoff(&ww);
		if (!err)
			goto retry;
	}
	i915_gem_ww_ctx_fini(&ww);
	return err;
}

#define I915_P2PDMA_OVERRIDE BIT(0)
#define I915_FABRIC_ONLY BIT(1)

static bool fabric_only(struct drm_i915_private *i915)
{
	return i915->params.prelim_override_p2p_dist & I915_FABRIC_ONLY;
}

static bool p2pdma_override(struct drm_i915_private *i915)
{
	return i915->params.prelim_override_p2p_dist & I915_P2PDMA_OVERRIDE;
}

static int i915_p2p_distance(struct drm_i915_private *i915, struct device *dev)
{
	int distance = 255; /* Override uses an arbitrary > 0 value */

	if (!p2pdma_override(i915))
		distance = pci_p2pdma_distance(to_pci_dev(i915->drm.dev), dev,
					       false);

	return distance;
}

static int object_to_attachment_p2p_distance(struct drm_i915_gem_object *obj,
					     struct dma_buf_attachment *attach)
{
	return i915_p2p_distance(to_i915(obj->base.dev), attach->dev);
}

/*
 * Order of communication path is
 *    fabric
 *    p2p
 *    migrate
 */
static int i915_gem_dmabuf_attach(struct dma_buf *dmabuf,
				  struct dma_buf_attachment *attach)
{
	struct drm_i915_gem_object *obj = dma_buf_to_obj(dmabuf);
	struct intel_gt *gt = obj->mm.region->gt;
	enum intel_engine_id id = gt->rsvd_bcs;
	struct intel_context *ce = gt->engine[id]->blitter_context;
	struct i915_gem_ww_ctx ww;
	int p2p_distance;
	int fabric;
	int err;

	fabric = update_fabric(dmabuf, attach->importer_priv);

	p2p_distance = object_to_attachment_p2p_distance(obj, attach);

	trace_i915_dma_buf_attach(obj, fabric, p2p_distance);

	if (fabric < 0)
		return -EOPNOTSUPP;

	if (!fabric && p2p_distance < 0 &&
	    !i915_gem_object_can_migrate(obj, INTEL_REGION_SMEM))
		return -EOPNOTSUPP;

	pvc_wa_disallow_rc6(ce->engine->i915);
	for_i915_gem_ww(&ww, err, true) {
		err = i915_gem_object_lock(obj, &ww);
		if (err)
			continue;
		if (obj->pair) {
			err = i915_gem_object_lock(obj->pair, &ww);
			if (err) {
				i915_gem_object_unlock(obj);
				continue;
			}
		}

		if (!fabric && p2p_distance < 0) {
			GEM_BUG_ON(obj->pair);
			err = i915_gem_object_migrate(obj, &ww, ce,
						      INTEL_REGION_SMEM, false);
			if (err)
				continue;
		}

		err = i915_gem_object_pin_pages(obj);
		if (!err && obj->pair) {
			err = i915_gem_object_pin_pages(obj->pair);
			if (err)
				i915_gem_object_unpin_pages(obj);
		}
	}

	return err;
}

static void i915_gem_dmabuf_detach(struct dma_buf *dmabuf,
				   struct dma_buf_attachment *attach)
{
	struct drm_i915_gem_object *obj = dma_buf_to_obj(dmabuf);
	struct drm_i915_private *i915 = to_i915(obj->base.dev);

	if (obj->pair)
		i915_gem_object_unpin_pages(obj->pair);

	i915_gem_object_unpin_pages(obj);
	pvc_wa_allow_rc6(i915);
}

static const struct dma_buf_ops i915_dmabuf_ops =  {
	.attach = i915_gem_dmabuf_attach,
	.detach = i915_gem_dmabuf_detach,
	.map_dma_buf = i915_gem_map_dma_buf,
	.unmap_dma_buf = i915_gem_unmap_dma_buf,
	.release = drm_gem_dmabuf_release,
	.mmap = i915_gem_dmabuf_mmap,
	.vmap = i915_gem_dmabuf_vmap,
	.vunmap = i915_gem_dmabuf_vunmap,
	.begin_cpu_access = i915_gem_begin_cpu_access,
	.end_cpu_access = i915_gem_end_cpu_access,
};

struct dma_buf *i915_gem_prime_export(struct drm_gem_object *gem_obj, int flags)
{
	struct drm_i915_gem_object *obj = to_intel_bo(gem_obj);
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);

	if (obj->vm) {
		drm_dbg(obj->base.dev,
			"Exporting VM private objects is not allowed\n");
		return ERR_PTR(-EINVAL);
	}

	exp_info.ops = &i915_dmabuf_ops;
	exp_info.size = gem_obj->size;
	if (obj->pair)
		exp_info.size += obj->pair->base.size;
	exp_info.flags = flags;
	exp_info.priv = gem_obj;
	exp_info.resv = obj->base.resv;

	if (obj->ops->dmabuf_export) {
		int ret = obj->ops->dmabuf_export(obj);
		if (ret)
			return ERR_PTR(ret);
	}

	return drm_gem_dmabuf_export(gem_obj->dev, &exp_info);
}

/*
 * update_fabric - check for fabric connectivity if available
 * @obj: object to check fabric connectivity
 *
 * If the imported object is a i915 dma-buf, and LMEM based, query to see if
 * there is a fabric, and if the fabric is connected set the fabric bit.
 *
 * 0 no connectivity, use P2P if available
 * 1 fabric is available
 * -1 fabric only is requested, and there is no fabric
 *
 */
static int update_fabric(struct dma_buf *dma_buf,
			 struct drm_i915_gem_object *obj)
{
	struct drm_i915_gem_object *import;
	struct drm_i915_private *src;
	struct drm_i915_private *dst;
	struct query_info *qi;
	int connected;
	int i;
	int n;

	/* Verify that both sides are i915s */
	if (dma_buf->ops != &i915_dmabuf_ops ||
	    !obj || obj->ops != &i915_gem_object_dmabuf_ops)
		return 0;

	import = dma_buf_to_obj(dma_buf);
	if (!i915_gem_object_is_lmem(import))
		return 0;

	src = to_i915(obj->base.dev);
	dst = to_i915(import->base.dev);

	qi = src->intel_iaf.ops->connectivity_query(src->intel_iaf.handle,
						    dst->intel_iaf.fabric_id);
	if (IS_ERR(qi))
		return fabric_only(src) ? -1 : 0;

	/*
	 * Examine the query information.  A zero bandwidth link indicates we
	 * are NOT connected.
	 */
	connected = 1;
	for (i = 0, n = qi->src_cnt * qi->dst_cnt; i < n && connected; i++)
		if (!qi->sd2sd[i].bandwidth)
			connected = 0;

	/* we are responsible for freeing qi */
	kfree(qi);

	if (connected) {
		if (intel_iaf_mapping_get(src))
			return 0;
		if (intel_iaf_mapping_get(dst)) {
			intel_iaf_mapping_put(src);
			return 0;
		}
		i915_gem_object_set_fabric(obj);
	}

	/* Object can use fabric or P2P, check for fabric only request */
	if (!connected && fabric_only(src))
		return -1;

	return connected;
}

/**
 * map_fabric_connectivity - check for fabric and create a mappable sgt if
 * available
 * @obj: object to check fabric connectivity
 *
 * NULL indicates no fabric connectivity.
 *
 */
static struct sg_table *map_fabric_connectivity(struct drm_i915_gem_object *obj)
{
	struct dma_buf *dma_buf = obj->base.import_attach->dmabuf;
	struct drm_i915_gem_object *import;

	if (!i915_gem_object_has_fabric(obj))
		return NULL;

	import = dma_buf_to_obj(dma_buf);

	/* Make sure the object didn't migrate */
	if (!i915_gem_object_is_lmem(import)) {
		i915_gem_object_clear_fabric(obj);
		return NULL;
	}

	return i915_gem_copy_pages(import);
}

/**
 * i915_gem_object_get_pages_dmabuf - get SG Table of pages from dmabuf
 * @obj: object on import side of dmabuf
 *
 * obj is created int _prime_import().  Determine where the pages need to
 * come from, and go get them.
 *
 */
static int i915_gem_object_get_pages_dmabuf(struct drm_i915_gem_object *obj)
{
	struct sg_table *sgt;
	unsigned int sg_page_sizes;

	assert_object_held(obj);

	/* See if there is a fabric, and set things up. */
	sgt = map_fabric_connectivity(obj);

	if (!sgt)
		sgt = dma_buf_map_attachment(obj->base.import_attach,
					     DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt))
		return PTR_ERR(sgt);

	sg_page_sizes = i915_sg_dma_sizes(sgt->sgl);

	__i915_gem_object_set_pages(obj, sgt, sg_page_sizes);

	return 0;
}

static int i915_gem_object_put_pages_dmabuf(struct drm_i915_gem_object *obj,
					     struct sg_table *sgt)
{
	if (i915_gem_object_has_fabric(obj)) {
		struct drm_i915_gem_object *export;

		export = dma_buf_to_obj(obj->base.import_attach->dmabuf);
		intel_iaf_mapping_put(to_i915(export->base.dev));
		intel_iaf_mapping_put(to_i915(obj->base.dev));

		i915_gem_object_clear_fabric(obj);
		sg_free_table(sgt);
		kfree(sgt);
		return 0;
	}

	dma_buf_unmap_attachment(obj->base.import_attach, sgt,
				 DMA_BIDIRECTIONAL);

	return 0;
}

static const struct drm_i915_gem_object_ops i915_gem_object_dmabuf_ops = {
	.name = "i915_gem_object_dmabuf",
	.get_pages = i915_gem_object_get_pages_dmabuf,
	.put_pages = i915_gem_object_put_pages_dmabuf,
};

struct drm_gem_object *i915_gem_prime_import(struct drm_device *dev,
					     struct dma_buf *dma_buf)
{
	static struct lock_class_key lock_class;
	struct dma_buf_attachment *attach;
	struct drm_i915_gem_object *obj;

	/* is this one of own objects? */
	if (dma_buf->ops == &i915_dmabuf_ops) {
		obj = dma_buf_to_obj(dma_buf);
		/* is it from our device? */
		if (obj->base.dev == dev &&
		    !I915_SELFTEST_ONLY(force_different_devices)) {
			/*
			 * Importing dmabuf exported from out own gem increases
			 * refcount on gem itself instead of f_count of dmabuf.
			 */
			return &i915_gem_object_get(obj)->base;
		}
	}

	if (i915_gem_object_size_2big(dma_buf->size))
		return ERR_PTR(-E2BIG);

	obj = i915_gem_object_alloc();
	if (!obj)
		return ERR_PTR(-ENOMEM);

	drm_gem_private_object_init(dev, &obj->base, dma_buf->size);
	i915_gem_object_init(obj, &i915_gem_object_dmabuf_ops, &lock_class,
			     I915_BO_ALLOC_USER);
	obj->base.resv = dma_buf->resv;

	/*
	 * We use GTT as shorthand for a coherent domain, one that is
	 * neither in the GPU cache nor in the CPU cache, where all
	 * writes are immediately visible in memory. (That's not strictly
	 * true, but it's close! There are internal buffers such as the
	 * write-combined buffer or a delay through the chipset for GTT
	 * writes that do require us to treat GTT as a separate cache domain.)
	 */
	obj->read_domains = I915_GEM_DOMAIN_GTT;
	obj->write_domain = 0;

	/* and attach the object */
	attach = dma_buf_dynamic_attach(dma_buf, dev->dev, NULL, obj);
	if (IS_ERR(attach)) {
		i915_gem_object_put(obj);
		return ERR_CAST(attach);
	}

	get_dma_buf(dma_buf);
	obj->base.import_attach = attach;

	return &obj->base;
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftests/mock_dmabuf.c"
#include "selftests/i915_gem_dmabuf.c"
#endif
