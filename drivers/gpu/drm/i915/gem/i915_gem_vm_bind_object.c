// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <linux/interval_tree_generic.h>
#include <linux/sched/mm.h>

#include "gem/i915_gem_vm_bind.h"
#include "gt/gen8_engine_cs.h"
#include "i915_debugger.h"
#include "i915_user_extensions.h"

#if IS_ENABLED(CPTCFG_DRM_I915_DEBUGGER)

static const char *get_driver_name(struct dma_fence *fence)
{
	return "[" DRIVER_NAME "]";
}

static const char *get_timeline_name(struct dma_fence *fence)
{
	return "debugger";
}

static const struct dma_fence_ops debugger_fence_ops = {
	.get_driver_name = get_driver_name,
	.get_timeline_name = get_timeline_name,
};

struct debugger_fence {
	struct dma_fence base;
	spinlock_t lock;
};

static struct dma_fence *create_debugger_fence(void)
{
	struct debugger_fence *f;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;

	spin_lock_init(&f->lock);
	dma_fence_init(&f->base, &debugger_fence_ops, &f->lock, 0, 0);

	return &f->base;
}

int i915_vma_add_debugger_fence(struct i915_vma *vma)
{
	struct dma_fence *f;

	GEM_BUG_ON(rcu_access_pointer(vma->debugger.fence));

	f = create_debugger_fence();
	if (!f)
		return -ENOMEM;

	RCU_INIT_POINTER(vma->debugger.fence, f);

	spin_lock(&vma->vm->debugger_lock);
	list_add_rcu(&vma->debugger.link, &vma->vm->debugger_fence_list);
	spin_unlock(&vma->vm->debugger_lock);

	return 0;
}

void i915_vma_signal_debugger_fence(struct i915_vma *vma)
{
	struct dma_fence *f;

	if (!rcu_access_pointer(vma->debugger.fence))
		return;

	spin_lock(&vma->vm->debugger_lock);
	f = rcu_replace_pointer(vma->debugger.fence, NULL, true);
	if (f)
		list_del_rcu(&vma->debugger.link);
	spin_unlock(&vma->vm->debugger_lock);

	if (f) {
		dma_fence_signal(f);
		dma_fence_put(f);
	}
}
#endif /* #if IS_ENABLED(CPTCFG_DRM_I915_DEBUGGER) */

#define START(node) ((node)->start)
#define LAST(node) ((node)->last)

INTERVAL_TREE_DEFINE(struct i915_vma, rb, u64, __subtree_last,
		     START, LAST, static inline, i915_vm_bind_it)

#undef START
#undef LAST

static int vm_bind_sync_fence(struct i915_user_extension __user *base,
			      void *data)
{
	struct prelim_drm_i915_vm_bind_ext_sync_fence ext;
	struct i915_vma *vma = data;

	if (copy_from_user(&ext, base, sizeof(ext)))
		return -EFAULT;

	vma->bind_fence.ptr = u64_to_user_ptr(ext.addr);
	vma->bind_fence.val = ext.val;
	vma->bind_fence.mm = current->mm;
	mmgrab(current->mm);

	return 0;
}

static int vm_bind_user_fence(struct i915_user_extension __user *base,
			      void *data)
{
	struct prelim_drm_i915_vm_bind_ext_user_fence ext;
	struct i915_vma *vma = data;

	if (copy_from_user(&ext, base, sizeof(ext)))
		return -EFAULT;

	vma->bind_fence.ptr = u64_to_user_ptr(ext.addr);
	vma->bind_fence.val = ext.val;
	vma->bind_fence.mm = current->mm;
	mmgrab(current->mm);

	return 0;
}

static int
gem_vm_bind_ext_uuid(struct i915_user_extension __user *base,
		     void *data)
{
	struct prelim_drm_i915_vm_bind_ext_uuid __user *ext =
		container_of_user(base, typeof(*ext), base);
	struct i915_vma *vma = data;
	struct i915_drm_client *client = vma->vm->client;
	struct i915_vma_metadata *metadata;
	struct i915_uuid_resource *uuid;
	u32 handle;

	metadata  = kzalloc(sizeof(*metadata), GFP_KERNEL);
	if (!metadata)
		return -ENOMEM;

	if (get_user(handle, &ext->uuid_handle))
		return -EFAULT;

	xa_lock(&client->uuids_xa);
	uuid = xa_load(&client->uuids_xa, handle);
	if (!uuid) {
		xa_unlock(&client->uuids_xa);
		kfree(metadata);
		return -ENOENT;
	}
	metadata->uuid = uuid;
	i915_uuid_get(uuid);
	atomic_inc(&uuid->bind_count);
	xa_unlock(&client->uuids_xa);

	spin_lock(&vma->metadata_lock);
	list_add_tail(&metadata->vma_link, &vma->metadata_list);
	spin_unlock(&vma->metadata_lock);
	return 0;
}

static int vm_bind_set_pat(struct i915_user_extension __user *base,
                              void *data)
{
	struct prelim_drm_i915_vm_bind_ext_set_pat ext;
	struct i915_vma *vma = data;

	if (copy_from_user(&ext, base, sizeof(ext)))
		return -EFAULT;

	/*
	 * Convert pat index to cache type
	 *
	 * Only do the conversion for the default shared pat index (0..3) so that
	 * the existing code will not be affected.
	 *
	 * For other pat index, there is no need to convert.
	 *
	 */
	switch (ext.pat_index){
		case 0:
			vma->obj->cache_level = I915_CACHE_NONE;
			break;
		case 1:
			vma->obj->cache_level = I915_CACHE_L3_LLC; /*TBD: WC type*/
			break;
		case 2:
			vma->obj->cache_level = I915_CACHE_WT;
			break;
		case 3:
			vma->obj->cache_level = I915_CACHE_L3_LLC; /* WB */
			break;
		default:
			vma->obj->cache_level = ext.pat_index;
			break;
	}
	DRM_DEBUG("vm_bind_set_pat: pat_index = %lld cache = %d \n",
			ext.pat_index,  vma->obj->cache_level);
	return 0;
}

static const i915_user_extension_fn vm_bind_extensions[] = {
	[PRELIM_I915_USER_EXT_MASK(PRELIM_I915_VM_BIND_EXT_SYNC_FENCE)] = vm_bind_sync_fence,
	[PRELIM_I915_USER_EXT_MASK(PRELIM_I915_VM_BIND_EXT_USER_FENCE)] = vm_bind_user_fence,
	[PRELIM_I915_USER_EXT_MASK(PRELIM_I915_VM_BIND_EXT_UUID)] = gem_vm_bind_ext_uuid,
	[PRELIM_I915_USER_EXT_MASK(PRELIM_I915_VM_BIND_EXT_SET_PAT)] = vm_bind_set_pat,
};

void i915_vma_metadata_free(struct i915_vma *vma)
{
	struct list_head *list = &vma->metadata_list;
	struct i915_vma_metadata *metadata, *next;

	if (!vma || list_empty(&vma->metadata_list))
		return;

	spin_lock(&vma->metadata_lock);
	list_for_each_entry_safe(metadata, next, list, vma_link) {
		list_del_init(&metadata->vma_link);
		atomic_dec(&metadata->uuid->bind_count);
		i915_uuid_put(metadata->uuid);
		kfree(metadata);
	}
	INIT_LIST_HEAD(&vma->metadata_list);
	spin_unlock(&vma->metadata_lock);
}

struct i915_vma *
i915_gem_vm_bind_lookup_vma(struct i915_address_space *vm, u64 va)
{
	struct i915_vma *vma, *temp;

	assert_vm_bind_held(vm);

	vma = i915_vm_bind_it_iter_first(&vm->va, va, va);
	/* Working around compiler error, remove later */
	if (vma)
		temp = i915_vm_bind_it_iter_next(vma, va + vma->size, -1);
	return vma;
}

void i915_gem_vm_bind_remove(struct i915_vma *vma, bool release_obj)
{
	assert_vm_bind_held(vma->vm);

	spin_lock(&vma->vm->vm_capture_lock);
	if (!list_empty(&vma->vm_capture_link))
		list_del_init(&vma->vm_capture_link);
	spin_unlock(&vma->vm->vm_capture_lock);

	spin_lock(&vma->vm->vm_rebind_lock);
	if (!list_empty(&vma->vm_rebind_link))
		list_del_init(&vma->vm_rebind_link);
	i915_vma_set_purged(vma);
	i915_vma_set_freed(vma);
	spin_unlock(&vma->vm->vm_rebind_lock);

	if (!list_empty(&vma->vm_bind_link)) {
		list_del_init(&vma->vm_bind_link);
		list_del_init(&vma->non_priv_vm_bind_link);
		i915_vm_bind_it_remove(vma, &vma->vm->va);

		/* Release object */
		if (release_obj)
			i915_vma_put(vma);
	}
}

static void __i915_gem_free_persistent_vmas(struct llist_node *freed)
{
	struct i915_vma *vma, *vn;

	llist_for_each_entry_safe(vma, vn, freed, freed) {
		struct drm_i915_gem_object *obj = vma->obj;

		/* Release vma and then object */
		__i915_vma_put(vma);
		i915_gem_object_put(obj);
		cond_resched();
	}
}

static void i915_gem_flush_free_persistent_vmas(struct i915_address_space *vm)
{
	struct llist_node *freed = llist_del_all(&vm->vm_bind_free_list);

	if (unlikely(freed))
		__i915_gem_free_persistent_vmas(freed);
}

static void __i915_gem_vm_bind_free_work(struct work_struct *work)
{
	struct i915_address_space *vm =
		container_of(work, typeof(*vm), vm_bind_free_work);

	i915_gem_flush_free_persistent_vmas(vm);
}

void i915_gem_vm_bind_init(struct i915_address_space *vm)
{
	INIT_WORK(&vm->vm_bind_free_work, __i915_gem_vm_bind_free_work);
}

static void i915_gem_vm_unbind_vma(struct i915_vma *vma, bool enqueue,
				   bool debug_destroy)
{
	struct i915_address_space *vm = vma->vm;

	assert_vm_bind_held(vm);

	i915_gem_vm_bind_remove(vma, false);

	if (debug_destroy)
		i915_debugger_vm_bind_destroy(vm->client, vma);

	if (llist_add(&vma->freed, &vm->vm_bind_free_list) && enqueue)
		queue_work(vm->i915->vm_bind_wq, &vm->vm_bind_free_work);
}

void i915_gem_vm_unbind_all(struct i915_address_space *vm)
{
	struct i915_vma *vma, *vn;

	i915_gem_vm_bind_lock(vm);
	list_for_each_entry_safe(vma, vn, &vm->vm_bind_list, vm_bind_link)
		i915_gem_vm_unbind_vma(vma, false, false);
	list_for_each_entry_safe(vma, vn, &vm->vm_bound_list, vm_bind_link)
		i915_gem_vm_unbind_vma(vma, false, false);
	i915_gem_vm_bind_unlock(vm);

	flush_workqueue(vm->i915->vm_bind_wq);
	i915_gem_flush_free_persistent_vmas(vm);
}

int i915_gem_vm_unbind_obj(struct i915_address_space *vm,
			   struct prelim_drm_i915_gem_vm_bind *va)
{
	struct i915_vma *vma;
	int ret;

	/* Handle is not used and must be 0 */
	if (va->handle)
		return -EINVAL;

	i915_debugger_wait_on_discovery(vm->i915);

	va->start = intel_noncanonical_addr(INTEL_PPGTT_MSB(vm->i915),
					    va->start);
	/* XXX: Support async and delayed unbind */
again:
	ret = i915_gem_vm_bind_lock_interruptible(vm);
	if (ret)
		return ret;

	vma = i915_gem_vm_bind_lookup_vma(vm, va->start);
	if (!vma) {
		ret = -ENOENT;
		goto out_unlock;
	}

	if (vma->size != va->length) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (i915_vma_is_pinned(vma) || atomic_read(&vma->open_count)) {
		ret = -EAGAIN;
		goto out_unlock;
	}

	/* XXX: hide the debug fence wait inside i915_vma.c */
	if (rcu_access_pointer(vma->debugger.fence)) {
		struct dma_fence *f;

		i915_gem_vm_bind_unlock(vm);

		rcu_read_lock();
		f = dma_fence_get_rcu_safe(&vma->debugger.fence);
		rcu_read_unlock();
		if (f) {
			ret = dma_fence_wait(f, true);
			dma_fence_put(f);
			if (ret)
				return ret;
		}

		goto again;
	}

	i915_gem_vm_unbind_vma(vma, true, true);

out_unlock:
	i915_gem_vm_bind_unlock(vm);

	if (!ret && !vm->i915->params.async_vm_unbind)
		flush_workqueue(vm->i915->vm_bind_wq);

	return ret;
}

static struct i915_vma *vm_bind_get_vma(struct i915_address_space *vm,
					struct drm_i915_gem_object *obj,
					struct prelim_drm_i915_gem_vm_bind *va)
{
	struct i915_ggtt_view view;
	struct i915_vma *vma;

	va->start = intel_noncanonical_addr(INTEL_PPGTT_MSB(vm->i915),
					    va->start);
	vma = i915_gem_vm_bind_lookup_vma(vm, va->start);
	if (vma)
		return ERR_PTR(-EEXIST);

	view.type = I915_GGTT_VIEW_PARTIAL;
	view.partial.offset = va->offset >> PAGE_SHIFT;
	view.partial.size = va->length >> PAGE_SHIFT;
	vma = i915_vma_instance(obj, vm, &view);
	if (IS_ERR(vma))
		return vma;

	vma->start = va->start;
	vma->last = va->start + va->length - 1;
	i915_vma_set_persistent(vma);

	return vma;
}

int i915_gem_vm_bind_obj(struct i915_address_space *vm,
			 struct prelim_drm_i915_gem_vm_bind *va,
			 struct drm_file *file)
{
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma = NULL;
	struct i915_gem_ww_ctx ww;
	int ret = 0;

	obj = i915_gem_object_lookup(file, va->handle);
	if (!obj)
		return -ENOENT;

	if (!va->length ||
	    !IS_ALIGNED(va->offset | va->length,
			i915_gem_object_max_page_size(obj)) ||
	    range_overflows_t(u64, va->offset, va->length, obj->base.size)) {
		ret = -EINVAL;
		goto put_obj;
	}

	if (obj->vm && obj->vm != vm) {
		ret = -EPERM;
		goto put_obj;
	}

	i915_debugger_wait_on_discovery(vm->i915);

	if (i915_gem_object_is_userptr(obj)) {
		ret = i915_gem_object_userptr_submit_init(obj);
		if (ret)
			goto put_obj;
	}

	ret = i915_gem_vm_bind_lock_interruptible(vm);
	if (ret)
		goto put_obj;

	vma = vm_bind_get_vma(vm, obj, va);
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		vma = NULL;
		goto unlock_vm;
	}

	ret = i915_user_extensions(u64_to_user_ptr(va->extensions),
				   vm_bind_extensions,
				   ARRAY_SIZE(vm_bind_extensions),
				   vma);
	if (ret)
		goto put_vma;

	i915_gem_ww_ctx_init(&ww, true);
	set_bit(I915_VM_HAS_PERSISTENT_BINDS, &vm->flags);
retry:
	if (va->flags & PRELIM_I915_GEM_VM_BIND_IMMEDIATE) {
		u64 pin_flags = va->start | PIN_OFFSET_FIXED | PIN_USER;

		if (i915_vm_page_fault_enabled(vm)) {
			if (va->flags & PRELIM_I915_GEM_VM_BIND_MAKE_RESIDENT) {
				pin_flags |= PIN_RESIDENT;
			} else if (vma->bind_fence.ptr) {
				ret = -EINVAL;
				goto out_ww;
			}
		}

		/* Always take vm_priv lock here (just like execbuff path) even
		 * for shared BOs, this will prevent the eviction/shrinker logic
		 * from evicint private BOs of the VM.
		 */
		ret = i915_gem_vm_priv_lock(vm, &ww);
		if (ret)
			goto out_ww;

		ret = i915_gem_object_lock(vma->obj, &ww);
		if (ret)
			goto out_ww;

		i915_vma_set_active_bind(vma);
		ret = i915_vma_pin_ww(vma, &ww, 0, 0, pin_flags);
		if (ret) {
			i915_vma_unset_active_bind(vma);
			goto out_ww;
		}

		if (i915_gem_object_is_userptr(obj)) {
			read_lock(&vm->i915->mm.notifier_lock);
			ret = i915_gem_object_userptr_submit_done(obj);
			read_unlock(&vm->i915->mm.notifier_lock);
			if (ret)
				goto out_ww;
		}

		list_add_tail(&vma->vm_bind_link, &vm->vm_bound_list);
	} else {
		/* bind during next execbuff, user fence here is invalid */
		if (vma->bind_fence.ptr) {
			ret = -EINVAL;
			goto out_ww;
		}

		list_add_tail(&vma->vm_bind_link, &vm->vm_bind_list);
	}

	if (va->flags & PRELIM_I915_GEM_VM_BIND_CAPTURE) {
		spin_lock(&vm->vm_capture_lock);
		list_add_tail(&vma->vm_capture_link, &vm->vm_capture_list);
		spin_unlock(&vm->vm_capture_lock);
	}

	i915_vm_bind_it_insert(vma, &vm->va);
	if (!obj->vm)
		list_add_tail(&vma->non_priv_vm_bind_link,
			      &vm->non_priv_vm_bind_list);

	/* Hold object reference until vm_unbind */
	i915_gem_object_get(vma->obj);
out_ww:
	if (ret == -EDEADLK) {
		ret = i915_gem_ww_ctx_backoff(&ww);
		if (!ret)
			goto retry;
	}
	i915_gem_ww_ctx_fini(&ww);

	if (ret && vma->bind_fence.mm) {
		mmdrop(vma->bind_fence.mm);
		vma->bind_fence.mm = NULL;
	}
put_vma:
	if (ret)
		i915_vma_metadata_free(vma);
	else
		i915_debugger_vm_bind_create(vm->client, vma, va);
unlock_vm:
	i915_gem_vm_bind_unlock(vm);
	if (ret && vma) {
		/* Release vma upon error outside the lock */
		i915_vma_set_purged(vma);
		__i915_vma_put(vma);
	}
put_obj:
	i915_gem_object_put(obj);
	return ret;
}
