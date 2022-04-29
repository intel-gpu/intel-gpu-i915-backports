/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __I915_GEM_VM_BIND_H
#define __I915_GEM_VM_BIND_H

#include <linux/dma-resv.h>
#include "i915_drv.h"

#define assert_vm_bind_held(vm)   lockdep_assert_held(&(vm)->vm_bind_lock)

static inline void i915_gem_vm_bind_lock(struct i915_address_space *vm)
{
	mutex_lock(&vm->vm_bind_lock);
}

static inline int
i915_gem_vm_bind_lock_interruptible(struct i915_address_space *vm)
{
	return mutex_lock_interruptible(&vm->vm_bind_lock);
}

static inline void i915_gem_vm_bind_unlock(struct i915_address_space *vm)
{
	mutex_unlock(&vm->vm_bind_lock);
}

#define assert_vm_priv_held(vm)   assert_object_held((vm)->root_obj)

static inline int i915_gem_vm_priv_lock(struct i915_address_space *vm,
					struct i915_gem_ww_ctx *ww)
{
	return i915_gem_object_lock(vm->root_obj, ww);
}

static inline int i915_gem_vm_priv_trylock(struct i915_address_space *vm)
{
	return i915_gem_object_trylock(vm->root_obj) ? 0 : -EBUSY;
}

static inline int i915_gem_vm_priv_lock_to_evict(struct i915_address_space *vm,
						 struct i915_gem_ww_ctx *ww)
{
	return i915_gem_object_lock_to_evict(vm->root_obj, ww);
}

static inline void i915_gem_vm_priv_unlock(struct i915_address_space *vm)
{
	i915_gem_object_unlock(vm->root_obj);
}

struct i915_vma *
i915_gem_vm_bind_lookup_vma(struct i915_address_space *vm, u64 va);
void i915_gem_vm_bind_remove(struct i915_vma *vma, bool release_obj);
int i915_gem_vm_bind_obj(struct i915_address_space *vm,
			 struct prelim_drm_i915_gem_vm_bind *va,
			 struct drm_file *file);
int i915_gem_vm_unbind_obj(struct i915_address_space *vm,
			   struct prelim_drm_i915_gem_vm_bind *va);
void i915_gem_vm_bind_init(struct i915_address_space *vm);
void i915_gem_vm_unbind_all(struct i915_address_space *vm);

void i915_vma_metadata_free(struct i915_vma *vma);

#if IS_ENABLED(CPTCFG_DRM_I915_DEBUGGER)
int i915_vma_add_debugger_fence(struct i915_vma *vma);
void i915_vma_signal_debugger_fence(struct i915_vma *vma);
#else
static inline void i915_vma_signal_debugger_fence(struct i915_vma *vma) { };
#endif

#endif /* __I915_GEM_VM_BIND_H */
