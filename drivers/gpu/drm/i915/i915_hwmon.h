/* SPDX-License-Identifier: MIT */

/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef __I915_HWMON_H__
#define __I915_HWMON_H__

#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include "i915_reg.h"

struct drm_i915_private;

void i915_hwmon_register(struct drm_i915_private *i915);
void i915_hwmon_unregister(struct drm_i915_private *i915);

int i915_hwmon_energy_status_get(struct drm_i915_private *i915, long *energy);
#endif /* __I915_HWMON_H__ */
