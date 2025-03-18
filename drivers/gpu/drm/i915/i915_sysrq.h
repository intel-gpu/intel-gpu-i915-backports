/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef __I915_SYSRQ_H__
#define __I915_SYSRQ_H__

struct drm_i915_private;

int i915_register_sysrq(struct drm_i915_private *i915);
void i915_unregister_sysrq(struct drm_i915_private *i915);

int i915_sysrq_register(void (*show)(void *data), void *data);
void i915_sysrq_unregister(void (*show)(void *data), void *data);

#endif /* __I915_SYSRQ_H__ */
