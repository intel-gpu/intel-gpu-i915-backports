/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_ACPI_H__
#define __INTEL_ACPI_H__

struct drm_i915_private;

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
#ifdef CONFIG_ACPI
void intel_register_dsm_handler(void);
void intel_unregister_dsm_handler(void);
void intel_dsm_get_bios_data_funcs_supported(struct drm_i915_private *i915);
void intel_acpi_device_id_update(struct drm_i915_private *i915);
#else
static inline void intel_register_dsm_handler(void) { return; }
static inline void intel_unregister_dsm_handler(void) { return; }
static inline
void intel_dsm_get_bios_data_funcs_supported(struct drm_i915_private *i915) { return; }
static inline
void intel_acpi_device_id_update(struct drm_i915_private *i915) { return; }
#endif /* CONFIG_ACPI */
#else
static inline void intel_register_dsm_handler(void){ return; }
static inline void intel_unregister_dsm_handler(void) { return; }
#endif /* CPTCFG_DRM_I915_DISPLAY */

#endif /* __INTEL_ACPI_H__ */
