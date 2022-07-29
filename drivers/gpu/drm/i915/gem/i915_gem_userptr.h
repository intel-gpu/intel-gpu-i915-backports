/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef __I915_GEM_USERPTR_H__
#define __I915_GEM_USERPTR_H__

struct drm_i915_private;

int i915_gem_init_userptr(struct drm_i915_private *dev_priv);
void i915_gem_cleanup_userptr(struct drm_i915_private *dev_priv);

#ifdef CONFIG_MMU_NOTIFIER
void i915_gem_userptr_lock_mmu_notifier(struct drm_i915_private *i915);
void i915_gem_userptr_unlock_mmu_notifier(struct drm_i915_private *i915);
#else
static inline void i915_gem_userptr_lock_mmu_notifier(struct drm_i915_private *i915) {}
static inline void i915_gem_userptr_unlock_mmu_notifier(struct drm_i915_private *i915) {}
#endif

#endif /* __I915_GEM_USERPTR_H__ */
