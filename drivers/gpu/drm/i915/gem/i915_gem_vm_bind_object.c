// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <linux/interval_tree_generic.h>
#include <linux/sched/mm.h>

#include "gt/gen8_engine_cs.h"

#include "i915_drv.h"
#include "i915_gem_gtt.h"
#include "i915_gem_userptr.h"
#include "i915_gem_vm_bind.h"
#include "i915_sw_fence_work.h"
#include "i915_user_extensions.h"

struct user_fence {
	struct mm_struct *mm;
	void __user *ptr;
	u64 val;
};

struct vm_bind_user_ext {
	struct user_fence bind_fence;
	struct i915_address_space *vm;
	struct list_head metadata_list;
	struct drm_i915_gem_object *obj;
};

#define START(node) ((node)->start)
#define LAST(node) ((node)->last)

INTERVAL_TREE_DEFINE(struct i915_vma, rb, u64, __subtree_last,
		     START, LAST, static inline, i915_vm_bind_it)

#undef START
#undef LAST

struct vm_bind_user_fence {
	struct dma_fence_work base;
	struct i915_sw_dma_fence_cb cb;
	struct user_fence user_fence;
	struct wait_queue_head *wq;
};

static int ufence_work(struct dma_fence_work *work)
{
	struct vm_bind_user_fence *vb =
		container_of(work, struct vm_bind_user_fence, base);
	struct user_fence *ufence = &vb->user_fence;
	int ret = -EFAULT;

	if (!ufence->mm)
		return 0;

	kthread_use_mm(ufence->mm);
	if (copy_to_user(ufence->ptr, &ufence->val, sizeof(ufence->val)) == 0) {
		wake_up_all(vb->wq);
		ret = 0;
	}
	kthread_unuse_mm(ufence->mm);

	return ret;
}

static void ufence_release(struct dma_fence_work *work)
{
	struct user_fence *ufence =
		&container_of(work, struct vm_bind_user_fence, base)->user_fence;

	if (ufence->mm)
		mmdrop(ufence->mm);
}

static const struct dma_fence_work_ops ufence_ops = {
	.name = "ufence",
	.work = ufence_work,
	.release = ufence_release,
};

static struct i915_sw_fence *
ufence_create(struct i915_address_space *vm, struct vm_bind_user_ext *arg)
{
	struct vm_bind_user_fence *vb;
	struct dma_fence *prev;

	vb = kzalloc(sizeof(*vb), GFP_KERNEL | __GFP_NOWARN);
	if (!vb)
		return NULL;

	dma_fence_work_init(&vb->base, &ufence_ops);
	if (arg->bind_fence.mm) {
		vb->user_fence = arg->bind_fence;
		mmgrab(vb->user_fence.mm);
	}
	vb->wq = &vm->i915->user_fence_wq;

	i915_sw_fence_await(&vb->base.chain); /* signaled by vma_bind */

	/* Preserve the user's write ordering of the user fence seqno */
	rcu_read_lock();
	prev = __i915_active_fence_set(&vm->user_fence, &vb->base.dma);
	if (prev)
		__i915_sw_fence_await_dma_fence(&vb->base.chain, prev, &vb->cb);
	rcu_read_unlock();

	dma_fence_work_commit(&vb->base);
	return &vb->base.chain;
}

static int __vm_bind_fence(struct vm_bind_user_ext *arg, u64 addr, u64 val)
{
	u64 x, * __user ptr = u64_to_user_ptr(addr);

	if (arg->bind_fence.mm)
		return -EINVAL;

	if (!access_ok(ptr, sizeof(x)))
		return -EFAULT;

	/* Natural alignment, no page crossings */
	if (!IS_ALIGNED(addr, sizeof(val)))
		return -EINVAL;

	arg->bind_fence.ptr = ptr;
	arg->bind_fence.val = val;
	arg->bind_fence.mm = current->mm;

	return get_user(x, ptr);
}

static int vm_bind_sync_fence(struct i915_user_extension __user *base,
                              void *data)
{
	struct prelim_drm_i915_vm_bind_ext_sync_fence ext;

	if (copy_from_user(&ext, base, sizeof(ext)))
		return -EFAULT;

	return __vm_bind_fence(data, ext.addr, ext.val);
}

static int vm_bind_user_fence(struct i915_user_extension __user *base,
                              void *data)
{
	struct prelim_drm_i915_vm_bind_ext_user_fence ext;

	if (copy_from_user(&ext, base, sizeof(ext)))
		return -EFAULT;

	return __vm_bind_fence(data, ext.addr, ext.val);
}

static int vm_bind_ext_uuid(struct i915_user_extension __user *base,
			    void *data)
{
	struct prelim_drm_i915_vm_bind_ext_uuid __user *ext =
		container_of_user(base, typeof(*ext), base);
	struct vm_bind_user_ext *arg = data;
	struct i915_drm_client *client = arg->vm->client;
	struct i915_vma_metadata *metadata;
	struct i915_uuid_resource *uuid;
	u32 handle;

	if (get_user(handle, &ext->uuid_handle))
		return -EFAULT;

	metadata  = kzalloc(sizeof(*metadata), GFP_KERNEL);
	if (!metadata)
		return -ENOMEM;

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

	list_add_tail(&metadata->vma_link, &arg->metadata_list);
	return 0;
}

#define TGL_MAX_PAT_INDEX	3
#define PVC_MAX_PAT_INDEX	7
#define MTL_MAX_PAT_INDEX	4

static int vm_bind_set_pat(struct i915_user_extension __user *base,
			   void *data)
{
	struct prelim_drm_i915_vm_bind_ext_set_pat ext;
	struct vm_bind_user_ext *arg = data;
	struct drm_i915_private *i915 = arg->vm->i915;
	__u64 max_pat_index;

	if (copy_from_user(&ext, base, sizeof(ext)))
		return -EFAULT;

	if (IS_METEORLAKE(i915))
		max_pat_index = MTL_MAX_PAT_INDEX;
	else if (IS_PONTEVECCHIO(i915))
		max_pat_index = PVC_MAX_PAT_INDEX;
	else if (GRAPHICS_VER(i915) >= 12)
		max_pat_index = TGL_MAX_PAT_INDEX;
	else
		/* For legacy platforms pat_index is a value of
		 * enum i915_cache_level
		 */
		max_pat_index = I915_CACHE_WT;

	if (ext.pat_index > max_pat_index)
		return -EINVAL;

	/*
	 * FIXME: Object should be locked here. And if the ioctl fails,
	 * we probably should revert the change made here.
	 */

	/*
	 * By design, the UMD's are passing in the PAT indices which can
	 * be directly used to set the corresponding bits in PTE.
	 */
	arg->obj->pat_index = ext.pat_index;

	return 0;
}

static const i915_user_extension_fn vm_bind_extensions[] = {
	[PRELIM_I915_USER_EXT_MASK(PRELIM_I915_VM_BIND_EXT_SYNC_FENCE)] = vm_bind_sync_fence,
	[PRELIM_I915_USER_EXT_MASK(PRELIM_I915_VM_BIND_EXT_USER_FENCE)] = vm_bind_user_fence,
	[PRELIM_I915_USER_EXT_MASK(PRELIM_I915_VM_BIND_EXT_UUID)] = vm_bind_ext_uuid,
	[PRELIM_I915_USER_EXT_MASK(PRELIM_I915_VM_BIND_EXT_SET_PAT)] = vm_bind_set_pat,
};

static void metadata_list_free(struct list_head *list)
{
	struct i915_vma_metadata *metadata, *next;

	list_for_each_entry_safe(metadata, next, list, vma_link) {
		list_del_init(&metadata->vma_link);
		atomic_dec(&metadata->uuid->bind_count);
		i915_uuid_put(metadata->uuid);
		kfree(metadata);
	}
}

void i915_vma_metadata_free(struct i915_vma *vma)
{
	if (list_empty(&vma->metadata_list))
		return;

	spin_lock(&vma->metadata_lock);
	metadata_list_free(&vma->metadata_list);
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

static void i915_gem_vm_bind_release(struct i915_vma *vma)
{
	struct drm_i915_gem_object *obj = vma->obj;

	__i915_vma_put(vma);
	i915_gem_object_put(obj);
}

static void i915_gem_vm_bind_remove(struct i915_vma *vma)
{
	assert_vm_bind_held(vma->vm);
	GEM_BUG_ON(list_empty(&vma->vm_bind_link));

	i915_vma_set_purged(vma);

	spin_lock(&vma->vm->vm_capture_lock);
	if (!list_empty(&vma->vm_capture_link))
		list_del_init(&vma->vm_capture_link);
	spin_unlock(&vma->vm->vm_capture_lock);

	spin_lock(&vma->vm->vm_rebind_lock);
	if (!list_empty(&vma->vm_rebind_link))
		list_del_init(&vma->vm_rebind_link);
	spin_unlock(&vma->vm->vm_rebind_lock);

	list_del_init(&vma->vm_bind_link);
	list_del_init(&vma->non_priv_vm_bind_link);
	i915_vm_bind_it_remove(vma, &vma->vm->va);
}

void i915_gem_vm_unbind_all(struct i915_address_space *vm)
{
	struct i915_vma *vma, *vn;

	i915_gem_vm_bind_lock(vm);
	list_for_each_entry_safe(vma, vn, &vm->vm_bind_list, vm_bind_link) {
		i915_gem_vm_bind_remove(vma);
		i915_gem_vm_bind_release(vma);
	}
	list_for_each_entry_safe(vma, vn, &vm->vm_bound_list, vm_bind_link) {
		i915_gem_vm_bind_remove(vma);
		i915_gem_vm_bind_release(vma);
	}
	i915_gem_vm_bind_unlock(vm);
}

struct unbind_work {
	struct dma_fence_work base;
	struct i915_vma *vma;
	struct i915_sw_dma_fence_cb cb;
};

static int unbind(struct dma_fence_work *work)
{
	struct unbind_work *w = container_of(work, typeof(*w), base);
	struct i915_vma *vma = w->vma;
	struct i915_address_space *vm = vma->vm;

	mutex_lock_nested(&vm->mutex, SINGLE_DEPTH_NESTING);
	if (drm_mm_node_allocated(&vma->node)) {
		__i915_vma_evict(vma);
		drm_mm_remove_node(&vma->node);
	}
	mutex_unlock(&vm->mutex);

	return 0;
}

static void release(struct dma_fence_work *work)
{
	struct unbind_work *w = container_of(work, typeof(*w), base);

	i915_gem_vm_bind_release(w->vma);
}

static const struct dma_fence_work_ops unbind_ops = {
	.name = "unbind",
	.work = unbind,
	.release = release,
};

static struct dma_fence_work *queue_unbind(struct i915_vma *vma)
{
	struct dma_fence *prev;
	struct unbind_work *w;

	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return NULL;

	dma_fence_work_init(&w->base, &unbind_ops);
	w->vma = vma;

	prev = i915_active_set_exclusive(&vma->active, &w->base.dma);
	if (!IS_ERR_OR_NULL(prev)) {
		__i915_sw_fence_await_dma_fence(&w->base.chain, prev, &w->cb);
		dma_fence_put(prev);
	}

	return &w->base;
}

int i915_gem_vm_unbind_obj(struct i915_address_space *vm,
			   struct prelim_drm_i915_gem_vm_bind *va)
{
	struct dma_fence_work *work = NULL;
	struct i915_vma *vma;
	int ret;

	/* Handle is not used and must be 0 */
	if (va->handle)
		return -EINVAL;

	va->start = intel_noncanonical_addr(INTEL_PPGTT_MSB(vm->i915),
					    va->start);
	/* XXX: Support async and delayed unbind */
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

	ret = i915_active_acquire(&vma->active);
	if (ret)
		goto out_unlock;

	work = queue_unbind(vma);
	i915_active_release(&vma->active);
	if (!work) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	i915_gem_vm_bind_remove(vma);

out_unlock:
	i915_gem_vm_bind_unlock(vm);

	if (work) {
		dma_fence_get(&work->dma);
		dma_fence_work_commit_imm(work);
		if (!vm->i915->params.async_vm_unbind)
			dma_fence_wait(&work->dma, false);
		dma_fence_put(&work->dma);
	}

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
	struct vm_bind_user_ext ext = {
		.metadata_list = LIST_HEAD_INIT(ext.metadata_list),
		.vm = vm,
	};
	struct drm_i915_gem_object *obj;
	struct i915_gem_ww_ctx ww;
	struct i915_vma *vma;
	int ret;

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

	if (i915_gem_object_is_userptr(obj)) {
		ret = i915_gem_object_userptr_submit_init(obj);
		if (ret)
			goto put_obj;
	}

	ext.obj = obj;
	ret = i915_user_extensions(u64_to_user_ptr(va->extensions),
				   vm_bind_extensions,
				   ARRAY_SIZE(vm_bind_extensions),
				   &ext);
	if (ret)
		goto put_obj;

	ret = i915_gem_vm_bind_lock_interruptible(vm);
	if (ret)
		goto put_obj;

	vma = vm_bind_get_vma(vm, obj, va);
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto unlock_vm;
	}

	if (va->flags & PRELIM_I915_GEM_VM_BIND_IMMEDIATE) {
		vma->bind_fence = ufence_create(vm, &ext);
		if (!vma->bind_fence) {
			ret = -ENOMEM;
			goto unlock_vm;
		}
	} else {
		/* bind during next execbuf, user fence here is invalid */
		if (ext.bind_fence.mm) {
			ret = -EINVAL;
			goto unlock_vm;
		}
	}

	/* Hold object reference until vm_unbind */
	i915_gem_object_get(vma->obj);

	if (!list_empty(&ext.metadata_list)) {
		spin_lock(&vma->metadata_lock);
		list_splice_tail_init(&ext.metadata_list, &vma->metadata_list);
		spin_unlock(&vma->metadata_lock);
	}

	i915_gem_ww_ctx_init(&ww, true);
	set_bit(I915_VM_HAS_PERSISTENT_BINDS, &vm->flags);
retry:
	if (va->flags & PRELIM_I915_GEM_VM_BIND_IMMEDIATE) {
		u64 pin_flags = va->start | PIN_OFFSET_FIXED | PIN_USER;

		if (va->flags & PRELIM_I915_GEM_VM_BIND_MAKE_RESIDENT)
			pin_flags |= PIN_RESIDENT;

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

		if (i915_gem_object_is_userptr(obj)) {
			i915_gem_userptr_lock_mmu_notifier(vm->i915);
			ret = i915_gem_object_userptr_submit_done(obj);
			i915_gem_userptr_unlock_mmu_notifier(vm->i915);
			if (ret)
				goto out_ww;
		}

		ret = i915_vma_pin_ww(vma, &ww, 0, 0, pin_flags);
		if (ret)
			goto out_ww;

		__i915_vma_unpin(vma);
	}

	if (va->flags & PRELIM_I915_GEM_VM_BIND_CAPTURE) {
		spin_lock(&vm->vm_capture_lock);
		list_add_tail(&vma->vm_capture_link, &vm->vm_capture_list);
		spin_unlock(&vm->vm_capture_lock);
	}

	list_add_tail(&vma->vm_bind_link, &vm->vm_bind_list);
	i915_vm_bind_it_insert(vma, &vm->va);
	if (!obj->vm)
		list_add_tail(&vma->non_priv_vm_bind_link,
			      &vm->non_priv_vm_bind_list);

out_ww:
	if (ret == -EDEADLK) {
		ret = i915_gem_ww_ctx_backoff(&ww);
		if (!ret)
			goto retry;
	}
	i915_gem_ww_ctx_fini(&ww);

	if (ret)
		i915_gem_vm_bind_release(vma);
unlock_vm:
	i915_gem_vm_bind_unlock(vm);
put_obj:
	i915_gem_object_put(obj);
	metadata_list_free(&ext.metadata_list);
	return ret;
}
