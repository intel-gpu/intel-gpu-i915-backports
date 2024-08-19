/*
 * Copyright © 2008-2015 Intel Corporation
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
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *
 */

#include <linux/dma-fence-array.h>
#include <linux/kthread.h>
#include <linux/dma-resv.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/stop_machine.h>
#include <linux/swap.h>
#include <linux/pci.h>
#include <linux/dma-buf.h>
#include <linux/mman.h>

#include <drm/drm_cache.h>
#include <drm/drm_vma_manager.h>

#include "display/intel_display.h"
#include "display/intel_frontbuffer.h"

#include "gem/i915_gem_context.h"
#include "gem/i915_gem_ioctls.h"
#include "gem/i915_gem_lmem.h"
#include "gem/i915_gem_mman.h"
#include "gem/i915_gem_pm.h"
#include "gem/i915_gem_region.h"
#include "gt/intel_engine_user.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_pm.h"
#include "gt/intel_workarounds.h"
#include "gt/intel_clos.h"

#include "i915_drv.h"
#include "i915_trace.h"

#include "intel_pm.h"

int
i915_gem_get_aperture_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file)
{
	struct drm_i915_private *i915 = to_i915(dev);
	struct i915_ggtt *ggtt = to_gt(i915)->ggtt;
	struct drm_i915_gem_get_aperture *args = data;
	struct i915_vma *vma;
	u64 pinned;

	if (mutex_lock_interruptible(&ggtt->vm.mutex))
		return -ERESTARTSYS;

	pinned = ggtt->vm.reserved;
	list_for_each_entry(vma, &ggtt->vm.bound_list, vm_link)
		if (i915_vma_is_pinned(vma))
			pinned += vma->node.size;

	mutex_unlock(&ggtt->vm.mutex);

	args->aper_size = ggtt->vm.total;
	args->aper_available_size = args->aper_size - pinned;

	return 0;
}

/*
 * For segmented BOs, this routine should be called for just the individual
 * segments and not the parent BO. As only the individual segments have
 * backing store, those per-segment objects are the ones getting linked
 * into the appropriate linked lists for tracking backing store:
 *   eviction: mem_region->objects.[purgeable, list]
 *   shrinker: i915->mm.[purge_list, shrink_list]
 * and likewise i915_gem_object_migrate_region operates on only individual
 * segment BOs.
 */
int i915_gem_object_unbind(struct drm_i915_gem_object *obj,
			   struct i915_gem_ww_ctx *ww,
			   unsigned long flags)
{
	struct intel_runtime_pm *rpm = &to_i915(obj->base.dev)->runtime_pm;
	struct i915_vma *bookmark;
	intel_wakeref_t wakeref = 0;
	LIST_HEAD(still_in_list);
	struct i915_vma *vma;
	int ret;

	if (list_empty(&obj->vma.list))
		return 0;

	bookmark = i915_vma_alloc(GFP_ATOMIC);
	if (!bookmark)
		return -ENOMEM;

try_again:
	ret = 0;
	spin_lock(&obj->vma.lock);
	list_for_each_entry(vma, &obj->vma.list, obj_link) {
		struct i915_address_space *vm = vma->vm;
		struct drm_i915_gem_object *unlock = NULL;

		if (!i915_vma_is_bound(vma, I915_VMA_BIND_MASK))
			continue;

		if (i915_vma_is_pinned(vma)) {
			ret = -EBUSY;
			break;
		}

		if (flags & I915_GEM_OBJECT_UNBIND_KEEP_RESIDENT &&
		    test_bit(I915_VMA_RESIDENT_BIT, __i915_vma_flags(vma))) {
			ret = -EBUSY;
			break;
		}

		if (flags & I915_GEM_OBJECT_UNBIND_TEST) {
			ret = -EBUSY;
			break;
		}

		ret = -EAGAIN;
		if (!i915_vm_tryopen(vm))
			break;

		/* Prevent vma being freed by i915_vma_parked as we unbind */
		list_add(&bookmark->obj_link, &vma->obj_link);
		vma = __i915_vma_get(vma);
		spin_unlock(&obj->vma.lock);
		if (!vma)
			goto close_vm;

		if (!(flags & I915_GEM_OBJECT_UNBIND_ACTIVE) &&
		    i915_vma_is_active(vma)) {
			ret = -EBUSY;
			goto put_vma;
		}

		/*
		 * As some machines use ACPI to handle runtime-resume
		 * callbacks, and ACPI is quite kmalloc happy, we cannot resume
		 * beneath the vm->mutex as they are required by the shrinker.
		 * Ergo, we wake the device up first just in case.
		 */
		if (!wakeref && i915_vma_is_ggtt(vma))
			wakeref = intel_runtime_pm_get(rpm);

		if (i915_vma_is_persistent(vma)) {
			ret = __i915_gem_object_lock_to_evict(vm->root_obj, ww);
			switch (ret) {
			case 0:
				unlock = vm->root_obj;
				break;

			case -EALREADY:
				if (flags & I915_GEM_OBJECT_UNBIND_ACTIVE)
					break;
				fallthrough;
			default:
				goto put_vma;
			}
		}

		ret = -EAGAIN;
		if (mutex_trylock(&vm->mutex)) {
			if (flags & I915_GEM_OBJECT_UNBIND_ACTIVE ||
			    !i915_vma_is_active(vma))
				ret = __i915_vma_unbind(vma);
			mutex_unlock(&vm->mutex);
		}

		if (unlock)
			i915_gem_object_unlock(unlock);
put_vma:
		__i915_vma_put(vma);
close_vm:
		i915_vm_close(vm);
		spin_lock(&obj->vma.lock);
		__list_del_entry(&bookmark->obj_link);
		vma = bookmark;
		if (ret)
			break;
	}
	spin_unlock(&obj->vma.lock);

	if (ret == -EAGAIN && flags & I915_GEM_OBJECT_UNBIND_BARRIER) {
		rcu_barrier(); /* flush the i915_vm_release() */
		goto try_again;
	}

	if (wakeref)
		intel_runtime_pm_put(rpm, wakeref);

	i915_vma_free(bookmark);
	return ret;
}

static int
shmem_pread(struct page *page, int offset, int len, char __user *user_data,
	    bool needs_clflush)
{
	char *vaddr;
	int ret;

	vaddr = kmap(page);

	if (needs_clflush)
		drm_clflush_virt_range(vaddr + offset, len);

	ret = __copy_to_user(user_data, vaddr + offset, len);

	kunmap(page);

	return ret ? -EFAULT : 0;
}

static int
i915_gem_shmem_pread(struct drm_i915_gem_object *obj,
		     struct drm_i915_gem_pread *args)
{
	unsigned int needs_clflush;
	char __user *user_data;
	unsigned long offset;
	pgoff_t idx;
	u64 remain;
	int ret;

	ret = i915_gem_object_lock_interruptible(obj, NULL);
	if (ret)
		return ret;

	ret = i915_gem_object_prepare_read(obj, &needs_clflush);
	if (ret)
		goto err_unlock;

	i915_gem_object_unlock(obj);

	remain = args->size;
	user_data = u64_to_user_ptr(args->data_ptr);
	offset = offset_in_page(args->offset);
	for (idx = args->offset >> PAGE_SHIFT; remain; idx++) {
		struct page *page = i915_gem_object_get_page(obj, idx);
		unsigned int length = min_t(u64, remain, PAGE_SIZE - offset);

		ret = shmem_pread(page, offset, length, user_data,
				  needs_clflush);
		if (ret)
			break;

		remain -= length;
		user_data += length;
		offset = 0;
	}

	i915_gem_object_finish_access(obj);
	return ret;

err_unlock:
	i915_gem_object_unlock(obj);
	return ret;
}

/*
 * Reads data from the object referenced by handle.
 * @dev: drm device pointer
 * @data: ioctl data blob
 * @file: drm file pointer
 *
 * On error, the contents of *data are undefined.
 */
int
i915_gem_pread_ioctl(struct drm_device *dev, void *data,
		     struct drm_file *file)
{
	struct drm_i915_private *i915 = to_i915(dev);
	struct drm_i915_gem_pread *args = data;
	struct drm_i915_gem_object *obj;
	int ret;

	/* PREAD is disallowed for all platforms after TGL-LP.  This also
	 * covers all platforms with local memory.
	 */
	if (GRAPHICS_VER(i915) >= 12 && !IS_TIGERLAKE(i915))
		return -EOPNOTSUPP;

	if (args->size == 0)
		return 0;

	if (!access_ok(u64_to_user_ptr(args->data_ptr),
		       args->size))
		return -EFAULT;

	obj = i915_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	/* Bounds check source.  */
	if (range_overflows_t(u64, args->offset, args->size, obj->base.size)) {
		ret = -EINVAL;
		goto out;
	}

	trace_i915_gem_object_pread(obj, args->offset, args->size);
	ret = i915_gem_object_wait(obj,
				   I915_WAIT_INTERRUPTIBLE,
				   MAX_SCHEDULE_TIMEOUT);
	if (ret)
		goto out;

	ret = i915_gem_shmem_pread(obj, args);

out:
	i915_gem_object_put(obj);
	return ret;
}

/* Per-page copy function for the shmem pwrite fastpath.
 * Flushes invalid cachelines before writing to the target if
 * needs_clflush_before is set and flushes out any written cachelines after
 * writing if needs_clflush is set.
 */
static int
shmem_pwrite(struct page *page, int offset, int len, char __user *user_data,
	     bool needs_clflush_before,
	     bool needs_clflush_after)
{
	char *vaddr;
	int ret;

	vaddr = kmap(page);

	if (needs_clflush_before)
		drm_clflush_virt_range(vaddr + offset, len);

	ret = __copy_from_user(vaddr + offset, user_data, len);
	if (!ret && needs_clflush_after)
		drm_clflush_virt_range(vaddr + offset, len);

	kunmap(page);

	return ret ? -EFAULT : 0;
}

static int
i915_gem_shmem_pwrite(struct drm_i915_gem_object *obj,
		      const struct drm_i915_gem_pwrite *args)
{
	unsigned int partial_cacheline_write;
	unsigned int needs_clflush;
	void __user *user_data;
	unsigned long offset;
	pgoff_t idx;
	u64 remain;
	int ret;

	ret = i915_gem_object_lock_interruptible(obj, NULL);
	if (ret)
		return ret;

	ret = i915_gem_object_prepare_write(obj, &needs_clflush);
	if (ret)
		goto err_unlock;

	i915_gem_object_unlock(obj);

	/* If we don't overwrite a cacheline completely we need to be
	 * careful to have up-to-date data by first clflushing. Don't
	 * overcomplicate things and flush the entire patch.
	 */
	partial_cacheline_write = 0;
	if (needs_clflush & CLFLUSH_BEFORE)
		partial_cacheline_write = boot_cpu_data.x86_clflush_size - 1;

	user_data = u64_to_user_ptr(args->data_ptr);
	remain = args->size;
	offset = offset_in_page(args->offset);
	for (idx = args->offset >> PAGE_SHIFT; remain; idx++) {
		struct page *page = i915_gem_object_get_page(obj, idx);
		unsigned int length = min_t(u64, remain, PAGE_SIZE - offset);

		ret = shmem_pwrite(page, offset, length, user_data,
				   (offset | length) & partial_cacheline_write,
				   needs_clflush & CLFLUSH_AFTER);
		if (ret)
			break;

		remain -= length;
		user_data += length;
		offset = 0;
	}

	i915_gem_object_flush_frontbuffer(obj, ORIGIN_CPU);

	i915_gem_object_finish_access(obj);
	return ret;

err_unlock:
	i915_gem_object_unlock(obj);
	return ret;
}

/*
 * Writes data to the object referenced by handle.
 * @dev: drm device
 * @data: ioctl data blob
 * @file: drm file
 *
 * On error, the contents of the buffer that were to be modified are undefined.
 */
int
i915_gem_pwrite_ioctl(struct drm_device *dev, void *data,
		      struct drm_file *file)
{
	struct drm_i915_private *i915 = to_i915(dev);
	struct drm_i915_gem_pwrite *args = data;
	struct drm_i915_gem_object *obj;
	int ret;

	/* PWRITE is disallowed for all platforms after TGL-LP.  This also
	 * covers all platforms with local memory.
	 */
	if (GRAPHICS_VER(i915) >= 12 && !IS_TIGERLAKE(i915))
		return -EOPNOTSUPP;

	if (args->size == 0)
		return 0;

	if (!access_ok(u64_to_user_ptr(args->data_ptr), args->size))
		return -EFAULT;

	obj = i915_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	/* Bounds check destination. */
	if (range_overflows_t(u64, args->offset, args->size, obj->base.size)) {
		ret = -EINVAL;
		goto err;
	}

	/* Writes not allowed into this read-only object */
	if (i915_gem_object_is_readonly(obj)) {
		ret = -EINVAL;
		goto err;
	}

	trace_i915_gem_object_pwrite(obj, args->offset, args->size);
	ret = i915_gem_object_wait(obj,
				   I915_WAIT_INTERRUPTIBLE |
				   I915_WAIT_ALL,
				   MAX_SCHEDULE_TIMEOUT);
	if (ret)
		goto err;

	ret = -ENODEV;
	if (i915_gem_object_has_struct_page(obj))
		ret = i915_gem_shmem_pwrite(obj, args);

err:
	i915_gem_object_put(obj);
	return ret;
}

/*
 * Called when user space has done writes to this buffer
 * @dev: drm device
 * @data: ioctl data blob
 * @file: drm file
 */
int
i915_gem_sw_finish_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file)
{
	return 0;
}

void i915_gem_runtime_suspend(struct drm_i915_private *i915)
{
}

static void discard_ggtt_vma(struct i915_vma *vma)
{
	struct drm_i915_gem_object *obj = vma->obj;

	spin_lock(&obj->vma.lock);
	if (!RB_EMPTY_NODE(&vma->obj_node)) {
		rb_erase(&vma->obj_node, &obj->vma.tree);
		RB_CLEAR_NODE(&vma->obj_node);
	}
	spin_unlock(&obj->vma.lock);
}

struct i915_vma *
i915_gem_object_ggtt_pin_ww(struct drm_i915_gem_object *obj,
			    struct i915_gem_ww_ctx *ww,
			    struct i915_ggtt *ggtt,
			    const struct i915_ggtt_view *view,
			    u64 size, u64 alignment, u64 flags)
{
	struct i915_vma *vma;
	int ret;

new_vma:
	vma = i915_vma_instance(obj, &ggtt->vm, view);
	if (IS_ERR(vma))
		return vma;

	if (i915_vma_misplaced(vma, size, alignment, flags)) {
		if (flags & PIN_NONBLOCK) {
			if (i915_vma_is_pinned(vma) || i915_vma_is_active(vma))
				return ERR_PTR(-ENOSPC);
		}

		if (i915_vma_is_pinned(vma) || i915_vma_is_active(vma)) {
			discard_ggtt_vma(vma);
			goto new_vma;
		}
	}

	if (ww)
		ret = i915_vma_pin_ww(vma, ww, size, alignment, flags | PIN_GLOBAL);
	else
		ret = i915_vma_pin(vma, size, alignment, flags | PIN_GLOBAL);

	if (ret)
		return ERR_PTR(ret);

	ret = i915_vma_wait_for_bind(vma);
	if (ret) {
		i915_vma_unpin(vma);
		return ERR_PTR(ret);
	}

	return vma;
}

static bool
i915_gem_object_madvise(struct drm_i915_gem_object *obj,
		        struct drm_i915_gem_madvise *args)
{
	if (obj->mm.madv != __I915_MADV_PURGED)
		obj->mm.madv = args->madv;

	/* if the object is no longer attached, discard its backing storage */
	if (obj->mm.madv == I915_MADV_DONTNEED &&
	    !i915_gem_object_has_pages(obj))
		i915_gem_object_truncate(obj);

	return obj->mm.madv != __I915_MADV_PURGED;
}

int
i915_gem_madvise_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file_priv)
{
	struct drm_i915_gem_madvise *args = data;
	struct drm_i915_gem_object *obj;
	struct i915_gem_ww_ctx ww;
	int err = 0;

	switch (args->madv) {
	case I915_MADV_DONTNEED:
	case I915_MADV_WILLNEED:
	    break;
	default:
	    return -EINVAL;
	}

	obj = i915_gem_object_lookup(file_priv, args->handle);
	if (!obj)
		return -ENOENT;

	for_i915_gem_ww(&ww, err, true) {
		if (!i915_gem_object_has_segments(obj)) {
			err = i915_gem_object_lock(obj, &ww);
			if (err)
				continue;
			args->retained = i915_gem_object_madvise(obj, args);
		} else {
			struct drm_i915_gem_object *sobj;
			int retained = 1;

			/*
			 * The backing store of the user object (the parent)
			 * is comprised of the backing store of all segments.
			 * Apply madvise to every segment. If any segment is
			 * not retained, then the user object (in its entirety)
			 * is not retained and so we must inform the user if
			 * even a single chunk of their data was discarded.
			 */
			for_each_object_segment(sobj, obj) {
				err = i915_gem_object_lock(sobj, &ww);
				if (err)
					break;
				retained &= i915_gem_object_madvise(sobj, args);
			}
			if (err)
				continue;

			args->retained = retained;
		}
	}

	i915_gem_object_put(obj);
	return err;
}

int i915_gem_init(struct drm_i915_private *dev_priv)
{
	struct intel_gt *gt;
	unsigned int i;
	int ret;

	for_each_gt(gt, dev_priv, i) {
		intel_uc_fetch_firmwares(&gt->uc);
		intel_wopcm_init(&gt->wopcm);
	}

	ret = i915_init_ggtt(dev_priv);
	if (ret) {
		GEM_BUG_ON(ret == -EIO);
		goto err_unlock;
	}

	/*
	 * Despite its name intel_init_clock_gating applies both display
	 * clock gating workarounds; GT mmio workarounds and the occasional
	 * GT power context workaround. Worse, sometimes it includes a context
	 * register workaround which we need to apply before we record the
	 * default HW state for all contexts.
	 *
	 * FIXME: break up the workarounds and apply them at the right time!
	 */
	intel_init_clock_gating(dev_priv);

	if (HAS_UM_QUEUES(dev_priv))
		xa_init_flags(&dev_priv->asid_resv.xa, XA_FLAGS_ALLOC);

	for_each_gt(gt, dev_priv, i) {
		intel_wakeref_t wf;

		with_intel_gt_pm(gt, wf) {
			ret = intel_gt_init(gt);
			if (ret == 0)
				i915_gem_init_lmem(gt);
		}
		if (ret)
			goto err_unlock;
	}

	return 0;

	/*
	 * Unwinding is complicated by that we want to handle -EIO to mean
	 * disable GPU submission but keep KMS alive. We want to mark the
	 * HW as irrevisibly wedged, but keep enough state around that the
	 * driver doesn't explode during runtime.
	 */
err_unlock:
	i915_gem_drain_workqueue(dev_priv);

	if (ret == -EIO) {
		/*
		 * Allow engines or uC initialisation to fail by marking the GPU
		 * as wedged. But we only want to do this when the GPU is angry,
		 * for all other failure, such as an allocation failure, bail.
		 */
		for_each_gt(gt, dev_priv, i)
			/* Make any cross-tile error permanent */
			intel_gt_set_wedged_on_init(gt);

		/* Minimal basic recovery for KMS */
		i915_ggtt_resume(to_gt(dev_priv)->ggtt);
		intel_init_clock_gating(dev_priv);
		ret = 0;
	} else {
		for_each_gt(gt, dev_priv, i) {
			i915_gem_fini_lmem(gt);
			intel_gt_driver_remove(gt);
			intel_gt_driver_release(gt);
			intel_uc_cleanup_firmwares(&gt->uc);
		}
	}

	i915_gem_drain_freed_objects(dev_priv);

	return ret;
}

void i915_gem_driver_register(struct drm_i915_private *i915)
{
	i915_gem_driver_register__shrinker(i915);

	intel_engines_driver_register(i915);
}

void i915_gem_driver_unregister(struct drm_i915_private *i915)
{
	i915_gem_driver_unregister__shrinker(i915);
}

void i915_gem_driver_remove(struct drm_i915_private *dev_priv)
{
	struct intel_gt *gt;
	unsigned int i;

	for_each_gt(gt, dev_priv, i) {
		intel_gt_suspend_late(gt);
		i915_gem_fini_lmem(gt);
		intel_gt_driver_remove(gt);
	}
	dev_priv->uabi_engines = RB_ROOT;

	/* Finish any generated work, and free all leftover objects. */
	i915_gem_drain_workqueue(dev_priv);
}

void i915_gem_driver_release(struct drm_i915_private *dev_priv)
{
	struct intel_gt *gt;
	unsigned int i;

	for_each_gt(gt, dev_priv, i) {
		i915_gem_fini_lmem(gt);
		intel_gt_driver_release(gt);
		intel_uc_cleanup_firmwares(&gt->uc);
	}

	i915_gem_drain_freed_objects(dev_priv);

	GEM_BUG_ON(!list_empty(&dev_priv->gem.contexts.list));
}

static void i915_gem_init__mm(struct drm_i915_private *i915)
{
	init_llist_head(&i915->mm.free_list);

	i915_gem_init__objects(i915);
}

void i915_gem_init_early(struct drm_i915_private *dev_priv)
{
	i915_gem_init__mm(dev_priv);
	i915_gem_init__contexts(dev_priv);

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
	spin_lock_init(&dev_priv->fb_tracking.lock);
#endif
}

void i915_gem_cleanup_early(struct drm_i915_private *dev_priv)
{
	i915_gem_drain_workqueue(dev_priv);
	GEM_BUG_ON(!llist_empty(&dev_priv->mm.free_list));
	GEM_BUG_ON(atomic_read(&dev_priv->mm.free_count));
}

int i915_gem_open(struct drm_i915_private *i915, struct drm_file *file)
{
	struct drm_i915_file_private *file_priv;
	struct i915_drm_client *client;
	int ret = -ENOMEM;

	file_priv = kzalloc(sizeof(*file_priv), GFP_KERNEL);
	if (!file_priv)
		goto err_alloc;

	client = i915_drm_client_add(&i915->clients, current, file_priv);
	if (IS_ERR(client)) {
		ret = PTR_ERR(client);
		goto err_client;
	}

	file->driver_priv = file_priv;
	file_priv->dev_priv = i915;
	file_priv->file = file;
	file_priv->client = client;

	file_priv->bsd_engine = -1;
	file_priv->hang_timestamp = jiffies;

	ret = i915_gem_context_open(i915, file);
	if (ret)
		goto err_context;

	init_client_clos(file_priv);
	return 0;

err_context:
	i915_drm_client_close(client);
err_client:
	kfree(file_priv);
err_alloc:
	return ret;
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftests/mock_gem_device.c"
#include "selftests/i915_gem.c"
#include "selftests/intel_remote_tiles.c"
#endif
