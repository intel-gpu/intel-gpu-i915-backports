// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

#include "gem/i915_gem_ioctls.h"
#include "gem/i915_gem_lmem.h"
#include "gem/i915_gem_object.h"
#include "gem/i915_gem_object_blt.h"
#include "gem/i915_gem_region.h"
#include "gt/intel_gt.h"

#include "i915_drv.h"
#include "i915_gem_create.h"
#include "i915_trace.h"
#include "i915_user_extensions.h"

u32 i915_gem_object_max_page_size(struct drm_i915_gem_object *obj)
{
	u32 max_page_size = I915_GTT_PAGE_SIZE_4K;
	int i;

	for (i = 0; i < obj->mm.n_placements; i++) {
		struct intel_memory_region *mr = obj->mm.placements[i];

		GEM_BUG_ON(!is_power_of_2(mr->min_page_size));
		max_page_size = max_t(u32, max_page_size, mr->min_page_size);
	}

	return max_page_size;
}

static void object_set_placements(struct drm_i915_gem_object *obj,
				  struct intel_memory_region **placements,
				  unsigned int n_placements)
{
	GEM_BUG_ON(!n_placements);

	/*
	 * For the common case of one memory region, skip storing an
	 * allocated array and just point at the region directly.
	 */
	if (n_placements == 1) {
		struct intel_memory_region *mr = placements[0];
		struct drm_i915_private *i915 = mr->i915;

		obj->mm.placements = &i915->mm.regions[mr->id];
		obj->mm.n_placements = 1;
	} else {
		obj->mm.placements = placements;
		obj->mm.n_placements = n_placements;
	}
}

static u64 object_size_align(struct intel_memory_region *mr, u64 size, u32 *flags)
{
	unsigned long page_sz_mask = mr->i915->params.page_sz_mask;
	u32 alloc_flags = 0;

	if (mr->type == INTEL_MEMORY_LOCAL && page_sz_mask) {
		unsigned long alignment = 0;

		if (page_sz_mask & BIT(0)) {
			alloc_flags |= I915_BO_ALLOC_CHUNK_4K;
			alignment = SZ_4K;
		} else if (page_sz_mask & BIT(1)) {
			alloc_flags |= I915_BO_ALLOC_CHUNK_64K;
			alignment = SZ_64K;
		} else if (page_sz_mask & BIT(2)) {
			alloc_flags |= I915_BO_ALLOC_CHUNK_2M;
			alignment = SZ_2M;
		} else if (page_sz_mask & BIT(3)) {
			alloc_flags |= I915_BO_ALLOC_CHUNK_1G;
			alignment = SZ_1G;
		}
		size = round_up(size, alignment);
	}

	*flags |= alloc_flags;
	return size;
}

static int i915_gem_publish(struct drm_i915_gem_object *obj,
			    struct drm_file *file,
			    u64 *size_p,
			    u32 *handle_p)
{
	u64 size = obj->base.size;
	int ret;

	ret = drm_gem_handle_create(file, &obj->base, handle_p);
	/* drop reference from allocate - handle holds it now */
	i915_gem_object_put(obj);
	if (ret)
		return ret;

	*size_p = size;
	return 0;
}

static u32 placement_mask(struct intel_memory_region **placements,
			  int n_placements)
{
	u32 mask = 0;
	int i;

	for (i = 0; i < n_placements; i++)
		mask |= BIT(placements[i]->id);

	GEM_BUG_ON(!mask);

	return mask;
}

static int
setup_object(struct drm_i915_gem_object *obj, u64 size)
{
	struct intel_memory_region *mr = obj->mm.placements[0];
	u32 alloc_flags;
	int ret;

	size = round_up(size, i915_gem_object_max_page_size(obj));
	if (size == 0)
		return -EINVAL;

	i915_gem_flush_free_objects(mr->i915);

	/* For most of the ABI (e.g. mmap) we think in system pages */
	GEM_BUG_ON(!IS_ALIGNED(size, PAGE_SIZE));

	if (size >> PAGE_SHIFT > INT_MAX)
		return -E2BIG;

	if (overflows_type(size, obj->base.size))
		return -E2BIG;

	alloc_flags = i915_modparams.force_alloc_contig & ALLOC_CONTIGUOUS_LMEM ?
		I915_BO_ALLOC_CONTIGUOUS : 0;

	size = object_size_align(mr, size, &alloc_flags);
	ret = mr->ops->init_object(mr, obj, size, alloc_flags | I915_BO_ALLOC_USER);
	if (ret)
		return ret;

	GEM_BUG_ON(size != obj->base.size);

	obj->memory_mask = placement_mask(obj->mm.placements, obj->mm.n_placements);

	trace_i915_gem_object_create(obj);

	return ret;
}

/**
 * handle_clear_errors - handle errors observed while clearing/migrating
 *                       user objects.
 * @obj: object being cleared
 * @errors: return value of the clear/migration operation
 * @locked: used to determine whether the object was already locked
 *          before returning back to userspace.
 *
 * Before returning to userspace, first issue in uninterruptible
 * wait on the object being cleared to let the operation complete
 * in the event of an interrupt when under high memory pressure.
 */
static int
handle_clear_errors(struct drm_i915_gem_object *obj, int errors, bool locked)
{
	int ret;

	ret = i915_gem_object_wait(obj, 0, MAX_SCHEDULE_TIMEOUT);
	if (!ret)
		goto unlock;

	/*
	 * return error code, caller needs to do cleaning
	 * with i915_gem_object_put().
	 */
	if (errors == -EINTR || errors == -ERESTARTSYS) {
		ret = errors;
		goto unlock;
	}

	/*
	 * XXX: Post the error to where we would normally gather
	 * and clear the pages. This better reflects the final
	 * uapi behaviour, once we are at the point where we can
	 * move the clear worker to get_pages().
	 */
	if (!locked)
		i915_gem_object_lock(obj, NULL);
	locked = true;

	i915_gem_object_unbind(obj, NULL,
			       I915_GEM_OBJECT_UNBIND_ACTIVE);

	GEM_WARN_ON(__i915_gem_object_put_pages(obj));

unlock:
	if (locked)
		i915_gem_object_unlock(obj);

	obj->mm.gem_create_posted_err = errors;

	return ret;
}

static int
clear_object(struct drm_i915_gem_object *obj)
{
	if (i915_gem_object_is_lmem(obj)) {
		struct intel_gt *gt = obj->mm.region->gt;
		enum intel_engine_id id = gt->rsvd_bcs;
		struct intel_context *ce = gt->engine[id]->blitter_context;
		int ret;

		/*
		 * Sometimes, the GPU is wedged, blitter_context is not
		 * setup, but driver is claimed to load successfully. If
		 * userland try to allocate lmem object, we should use
		 * CPU to clear the lmem pages.
		 */
		if (intel_gt_is_wedged(gt)) {
			void *ptr;

			ptr = i915_gem_object_pin_map_unlocked(obj,
							       I915_MAP_WC);
			if (IS_ERR(ptr))
				return PTR_ERR(ptr);

			memset(ptr, 0, obj->base.size);

			i915_gem_object_flush_map(obj);
			__i915_gem_object_release_map(obj);

			return 0;
		}

		/*
		 * XXX: We really want to move this to get_pages(), but we
		 * require grabbing the BKL for the blitting operation which is
		 * annoying. In the pipeline is support for async get_pages()
		 * which should fit nicely for this. Also note that the actual
		 * clear should be done async(we currently do an object_wait
		 * which is pure garbage), we just need to take care if
		 * userspace opts of implicit sync for the execbuf, to avoid any
		 * potential info leak.
		 */

		ret = i915_gem_object_fill_blt(obj, ce, 0);
		if (ret)
			return handle_clear_errors(obj, ret, false);

		/*
		 * XXX: Occasionally i915_gem_object_wait() called inside
		 * i915_gem_object_set_to_cpu_domain() get interrupted
		 * and return -ERESTARTSYS.
		 */
		i915_gem_object_lock(obj, NULL);
		ret = i915_gem_object_set_to_cpu_domain(obj, false);
		if (ret)
			return handle_clear_errors(obj, ret, true);
		i915_gem_object_unlock(obj);
	}

	return 0;
}

int
i915_gem_dumb_create(struct drm_file *file,
		     struct drm_device *dev,
		     struct drm_mode_create_dumb *args)
{
	struct drm_i915_gem_object *obj;
	struct intel_memory_region *mr;
	enum intel_memory_type mem_type;
	int cpp = DIV_ROUND_UP(args->bpp, 8);
	u32 format;
	int ret;

	switch (cpp) {
	case 1:
		format = DRM_FORMAT_C8;
		break;
	case 2:
		format = DRM_FORMAT_RGB565;
		break;
	case 4:
		format = DRM_FORMAT_XRGB8888;
		break;
	default:
		return -EINVAL;
	}

	/* have to work out size/pitch and return them */
	args->pitch = ALIGN(args->width * cpp, 64);

	/* align stride to page size so that we can remap */
	if (args->pitch > intel_plane_fb_max_stride(to_i915(dev), format,
						    DRM_FORMAT_MOD_LINEAR))
		args->pitch = ALIGN(args->pitch, 4096);

	if (args->pitch < args->width)
		return -EINVAL;

	args->size = mul_u32_u32(args->pitch, args->height);

	mem_type = INTEL_MEMORY_SYSTEM;
	if (HAS_LMEM(to_i915(dev)))
		mem_type = INTEL_MEMORY_LOCAL;

	obj = i915_gem_object_alloc();
	if (!obj)
		return -ENOMEM;

	mr = intel_memory_region_by_type(to_i915(dev), mem_type);
	object_set_placements(obj, &mr, 1);

	ret = setup_object(obj, args->size);
	if (ret)
		goto object_free;

	ret = clear_object(obj);
	if (ret)
		goto object_put;

	return i915_gem_publish(obj, file, &args->size, &args->handle);

object_put:
	i915_gem_object_put(obj);
	return ret;

object_free:
	i915_gem_object_free(obj);
	return ret;
}

struct create_ext {
	struct drm_i915_private *i915;
	struct drm_i915_gem_object *vanilla_object;
	u32 vm_id;
};

static void repr_placements(char *buf, size_t size,
			    struct intel_memory_region **placements,
			    int n_placements)
{
	int i;

	buf[0] = '\0';

	for (i = 0; i < n_placements; i++) {
		struct intel_memory_region *mr = placements[i];
		int r;

		r = snprintf(buf, size, "\n  %s -> { class: %d, inst: %d }",
			     mr->name, mr->type, mr->instance);
		if (r >= size)
			return;

		buf += r;
		size -= r;
	}
}

static int prelim_set_placements(struct prelim_drm_i915_gem_object_param *args,
			  struct create_ext *ext_data)
{
	struct drm_i915_private *i915 = ext_data->i915;
	struct prelim_drm_i915_gem_memory_class_instance __user *uregions =
		u64_to_user_ptr(args->data);
	struct drm_i915_gem_object *obj = ext_data->vanilla_object;
	struct intel_memory_region **placements;
	u32 mask;
	int i, ret = 0;

	if (args->handle) {
		DRM_DEBUG("Handle should be zero\n");
		ret = -EINVAL;
	}

	if (!args->size) {
		DRM_DEBUG("Size is zero\n");
		ret = -EINVAL;
	}

	if (args->size > ARRAY_SIZE(i915->mm.regions)) {
		DRM_DEBUG("Too many placements\n");
		ret = -EINVAL;
	}

	if (ret)
		return ret;

	placements = kmalloc_array(args->size,
				   sizeof(struct intel_memory_region *),
				   GFP_KERNEL);
	if (!placements)
		return -ENOMEM;

	mask = 0;
	for (i = 0; i < args->size; i++) {
		struct prelim_drm_i915_gem_memory_class_instance region;
		struct intel_memory_region *mr;

		if (copy_from_user(&region, uregions, sizeof(region))) {
			ret = -EFAULT;
			goto out_free;
		}

		mr = intel_memory_region_lookup(i915,
						region.memory_class,
						region.memory_instance);
		if (!mr || mr->private) {
			DRM_DEBUG("Device is missing region { class: %d, inst: %d } at index = %d\n",
				  region.memory_class, region.memory_instance, i);
			ret = -EINVAL;
			goto out_dump;
		}

		if (mask & BIT(mr->id)) {
			DRM_DEBUG("Found duplicate placement %s -> { class: %d, inst: %d } at index = %d\n",
				  mr->name, region.memory_class,
				  region.memory_instance, i);
			ret = -EINVAL;
			goto out_dump;
		}

		placements[i] = mr;
		mask |= BIT(mr->id);

		++uregions;
	}

	if (obj->mm.placements) {
		ret = -EINVAL;
		goto out_dump;
	}

	object_set_placements(obj, placements, args->size);
	if (args->size == 1)
		kfree(placements);

	return 0;

out_dump:
	if (1) {
		char buf[256];

		if (obj->mm.placements) {
			repr_placements(buf,
					sizeof(buf),
					obj->mm.placements,
					obj->mm.n_placements);
			DRM_DEBUG("Placements were already set in previous SETPARAM. Existing placements: %s\n",
				  buf);
		}

		repr_placements(buf, sizeof(buf), placements, i);
		DRM_DEBUG("New placements(so far validated): %s\n", buf);
	}

out_free:
	kfree(placements);
	return ret;
}

static int __create_setparam(struct prelim_drm_i915_gem_object_param *args,
			     struct create_ext *ext_data)
{
	if (!(args->param & PRELIM_I915_OBJECT_PARAM)) {
		DRM_DEBUG("Missing I915_OBJECT_PARAM namespace\n");
		return -EINVAL;
	}

	switch (lower_32_bits(args->param)) {
	case PRELIM_I915_PARAM_MEMORY_REGIONS:
		return prelim_set_placements(args, ext_data);
	}

	return -EINVAL;
}

static int create_setparam(struct i915_user_extension __user *base, void *data)
{
	struct prelim_drm_i915_gem_create_ext_setparam ext;

	if (copy_from_user(&ext, base, sizeof(ext)))
		return -EFAULT;

	return __create_setparam(&ext.param, data);
}

static int ext_set_vm_private(struct i915_user_extension __user *base,
			      void *data)
{
	struct prelim_drm_i915_gem_create_ext_vm_private ext;
	struct create_ext *ext_data = data;

	if (copy_from_user(&ext, base, sizeof(ext)))
		return -EFAULT;

	ext_data->vm_id = ext.vm_id;

	return 0;
}

static const i915_user_extension_fn prelim_create_extensions[] = {
	[PRELIM_I915_USER_EXT_MASK(PRELIM_I915_GEM_CREATE_EXT_SETPARAM)] = create_setparam,
	[PRELIM_I915_USER_EXT_MASK(PRELIM_I915_GEM_CREATE_EXT_VM_PRIVATE)] = ext_set_vm_private,
};

/**
 * Creates a new mm object and returns a handle to it.
 * @dev: drm device pointer
 * @data: ioctl data blob
 * @file: drm file pointer
 */
int
i915_gem_create_ioctl(struct drm_device *dev, void *data,
		      struct drm_file *file)
{
	struct drm_i915_private *i915 = to_i915(dev);
	struct prelim_drm_i915_gem_create_ext *args = data;
	struct create_ext ext_data = { .i915 = i915 };
	struct intel_memory_region **placements_ext;
	struct intel_memory_region *stack[1];
	struct drm_i915_gem_object *obj;
	int ret;

	i915_gem_flush_free_objects(i915);

	obj = i915_gem_object_alloc();
	if (!obj)
		return -ENOMEM;

	ext_data.vanilla_object = obj;
	ret = i915_user_extensions(u64_to_user_ptr(args->extensions),
				   prelim_create_extensions,
				   ARRAY_SIZE(prelim_create_extensions),
				   &ext_data);
	placements_ext = obj->mm.placements;
	if (ret)
		goto object_free;

	if (ext_data.vm_id) {
		obj->vm = i915_address_space_lookup(file->driver_priv,
						    ext_data.vm_id);
		if (unlikely(!obj->vm)) {
			ret = -ENOENT;
			goto object_free;
		}
	}

	if (!placements_ext) {
		enum intel_memory_type mem_type = INTEL_MEMORY_SYSTEM;

		stack[0] = intel_memory_region_by_type(i915, mem_type);
		object_set_placements(obj, stack, 1);
	}

	ret = setup_object(obj, args->size);
	if (ret)
		goto vm_put;

	ret = clear_object(obj);
	if (ret)
		goto object_put;

	if (obj->vm) {
		list_add_tail(&obj->priv_obj_link, &obj->vm->priv_obj_list);
		obj->base.resv = obj->vm->root_obj->base.resv;
		i915_vm_put(obj->vm);
	}

	return i915_gem_publish(obj, file, &args->size, &args->handle);

object_put:
	if (obj->vm)
		i915_vm_put(obj->vm);
	i915_gem_object_put(obj);
	return ret;
vm_put:
	if (obj->vm)
		i915_vm_put(obj->vm);
object_free:
	if (obj->mm.n_placements > 1)
		kfree(placements_ext);

	i915_gem_object_free(obj);
	return ret;
}

static int set_placements(struct drm_i915_gem_create_ext_memory_regions *args,
			  struct create_ext *ext_data)
{
	struct drm_i915_private *i915 = ext_data->i915;
	struct drm_i915_gem_memory_class_instance __user *uregions =
		u64_to_user_ptr(args->regions);
	struct drm_i915_gem_object *obj = ext_data->vanilla_object;
	struct intel_memory_region **placements;
	u32 mask;
	int i, ret = 0;

	if (args->pad) {
		drm_dbg(&i915->drm, "pad should be zero\n");
		ret = -EINVAL;
	}

	if (!args->num_regions) {
		drm_dbg(&i915->drm, "num_regions is zero\n");
		ret = -EINVAL;
	}

	if (args->num_regions > ARRAY_SIZE(i915->mm.regions)) {
		drm_dbg(&i915->drm, "num_regions is too large\n");
		ret = -EINVAL;
	}

	if (ret)
		return ret;

	placements = kmalloc_array(args->num_regions,
				   sizeof(struct intel_memory_region *),
				   GFP_KERNEL);
	if (!placements)
		return -ENOMEM;

	mask = 0;
	for (i = 0; i < args->num_regions; i++) {
		struct drm_i915_gem_memory_class_instance region;
		struct intel_memory_region *mr;

		if (copy_from_user(&region, uregions, sizeof(region))) {
			ret = -EFAULT;
			goto out_free;
		}

		mr = intel_memory_region_lookup(i915,
						region.memory_class,
						region.memory_instance);
		if (!mr || mr->private) {
			drm_dbg(&i915->drm, "Device is missing region { class: %d, inst: %d } at index = %d\n",
				region.memory_class, region.memory_instance, i);
			ret = -EINVAL;
			goto out_dump;
		}

		if (mask & BIT(mr->id)) {
			drm_dbg(&i915->drm, "Found duplicate placement %s -> { class: %d, inst: %d } at index = %d\n",
				mr->name, region.memory_class,
				region.memory_instance, i);
			ret = -EINVAL;
			goto out_dump;
		}

		placements[i] = mr;
		mask |= BIT(mr->id);

		++uregions;
	}

	if (obj->mm.placements) {
		ret = -EINVAL;
		goto out_dump;
	}

	object_set_placements(obj, placements, args->num_regions);
	if (args->num_regions == 1)
		kfree(placements);

	return 0;

out_dump:
	if (1) {
		char buf[256];

		if (obj->mm.placements) {
			repr_placements(buf,
					sizeof(buf),
					obj->mm.placements,
					obj->mm.n_placements);
			drm_dbg(&i915->drm,
				"Placements were already set in previous EXT. Existing placements: %s\n",
				buf);
		}

		repr_placements(buf, sizeof(buf), placements, i);
		drm_dbg(&i915->drm, "New placements(so far validated): %s\n", buf);
	}

out_free:
	kfree(placements);
	return ret;
}

static int ext_set_placements(struct i915_user_extension __user *base,
			      void *data)
{
	struct drm_i915_gem_create_ext_memory_regions ext;

	if (copy_from_user(&ext, base, sizeof(ext)))
		return -EFAULT;

	return set_placements(&ext, data);
}

static const i915_user_extension_fn create_extensions[] = {
	[I915_GEM_CREATE_EXT_MEMORY_REGIONS] = ext_set_placements,
};

/**
 * Creates a new mm object and returns a handle to it.
 * @dev: drm device pointer
 * @data: ioctl data blob
 * @file: drm file pointer
 */
int
i915_gem_create_ext_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file)
{
	struct drm_i915_private *i915 = to_i915(dev);
	struct drm_i915_gem_create_ext *args = data;
	struct create_ext ext_data = { .i915 = i915 };
	struct intel_memory_region **placements_ext;
	struct drm_i915_gem_object *obj;
	int ret;

	return -EINVAL;

	if (args->flags)
		return -EINVAL;

	i915_gem_flush_free_objects(i915);

	obj = i915_gem_object_alloc();
	if (!obj)
		return -ENOMEM;

	ext_data.vanilla_object = obj;
	ret = i915_user_extensions(u64_to_user_ptr(args->extensions),
				   create_extensions,
				   ARRAY_SIZE(create_extensions),
				   &ext_data);
	placements_ext = obj->mm.placements;
	if (ret)
		goto object_free;

	if (ext_data.vm_id) {
		obj->vm = i915_address_space_lookup(file->driver_priv,
						    ext_data.vm_id);
		if (unlikely(!obj->vm)) {
			ret = -ENOENT;
			goto object_free;
		}
	}

	if (!placements_ext) {
		struct intel_memory_region *mr =
			intel_memory_region_by_type(i915, INTEL_MEMORY_SYSTEM);

		object_set_placements(obj, &mr, 1);
	}

	ret = setup_object(obj, args->size);
	if (ret)
		goto vm_put;

	ret = clear_object(obj);
	if (ret)
		goto object_put;

	if (obj->vm) {
		list_add_tail(&obj->priv_obj_link, &obj->vm->priv_obj_list);
		obj->base.resv = obj->vm->root_obj->base.resv;
		i915_vm_put(obj->vm);
	}

	return i915_gem_publish(obj, file, &args->size, &args->handle);
object_put:
	if (obj->vm)
		i915_vm_put(obj->vm);
	i915_gem_object_put(obj);
	return ret;
vm_put:
	if (obj->vm)
		i915_vm_put(obj->vm);
object_free:
	if (obj->mm.n_placements > 1)
		kfree(placements_ext);
	i915_gem_object_free(obj);
	return ret;
}

/*
 * Creates a new object using the similar path as DRM_I915_GEM_CREATE_EXT.
 * This function is exposed primarily for selftests
 * It is assumed that the set of placement regions has already been verified
 * to be valid.
 */
struct drm_i915_gem_object *
i915_gem_object_create_user(struct drm_i915_private *i915, u64 size,
			    struct intel_memory_region **placements,
			    unsigned int n_placements)
{
	struct drm_i915_gem_object *obj;
	int ret;

	i915_gem_flush_free_objects(i915);

	obj = i915_gem_object_alloc();
	if (!obj)
		return ERR_PTR(-ENOMEM);

	if (n_placements > 1) {
		struct intel_memory_region **tmp;

		tmp = kmalloc_array(n_placements, sizeof(*tmp), GFP_KERNEL);
		if (!tmp) {
			ret = -ENOMEM;
			goto object_free;
		}

		memcpy(tmp, placements, sizeof(*tmp) * n_placements);
		placements = tmp;
	}

	object_set_placements(obj, placements, n_placements);
	ret = setup_object(obj, size);
	if (ret)
		goto placement_free;

	ret = clear_object(obj);
	if (ret)
		goto object_put;

	return obj;

object_put:
	i915_gem_object_put(obj);
	return ERR_PTR(ret);

placement_free:
	if (n_placements > 1)
		kfree(placements);

object_free:
	i915_gem_object_free(obj);
	return ERR_PTR(ret);
}

