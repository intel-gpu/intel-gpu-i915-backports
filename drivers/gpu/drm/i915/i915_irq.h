/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __I915_IRQ_H__
#define __I915_IRQ_H__

#include <linux/ktime.h>
#include <linux/types.h>

#include "i915_reg.h"

enum pipe;
struct drm_crtc;
struct drm_device;
struct drm_display_mode;
struct drm_i915_private;
struct intel_crtc;
struct intel_uncore;

void intel_irq_init(struct drm_i915_private *dev_priv);
void intel_irq_fini(struct drm_i915_private *dev_priv);
int intel_irq_install(struct drm_i915_private *dev_priv);
void intel_irq_uninstall(struct drm_i915_private *dev_priv);

void intel_hpd_irq_setup(struct drm_i915_private *i915);
void i915_hotplug_interrupt_update(struct drm_i915_private *dev_priv,
				   u32 mask,
				   u32 bits);

void bdw_enable_pipe_irq(struct drm_i915_private *i915, enum pipe pipe, u32 bits);
void bdw_disable_pipe_irq(struct drm_i915_private *i915, enum pipe pipe, u32 bits);

void ibx_enable_display_interrupt(struct drm_i915_private *i915, u32 bits);
void ibx_disable_display_interrupt(struct drm_i915_private *i915, u32 bits);

void intel_runtime_pm_disable_interrupts(struct drm_i915_private *dev_priv);
void intel_runtime_pm_enable_interrupts(struct drm_i915_private *dev_priv);
bool intel_irqs_enabled(struct drm_i915_private *dev_priv);
void intel_synchronize_irq(struct drm_i915_private *i915);
void intel_synchronize_hardirq(struct drm_i915_private *i915);

void gen8_irq_power_well_post_enable(struct drm_i915_private *dev_priv,
				     u8 pipe_mask);
void gen8_irq_power_well_pre_disable(struct drm_i915_private *dev_priv,
				     u8 pipe_mask);

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
int intel_get_crtc_scanline(struct intel_crtc *crtc);
bool intel_crtc_get_vblank_timestamp(struct drm_crtc *crtc, int *max_error,
				     ktime_t *vblank_time, bool in_vblank_irq);
u32 g4x_get_vblank_counter(struct drm_crtc *crtc);
#else
static inline int intel_get_crtc_scanline(struct intel_crtc *crtc) { return 0; }
static inline bool intel_crtc_get_vblank_timestamp(struct drm_crtc *crtc, int *max_error,
				     ktime_t *vblank_time, bool in_vblank_irq) { return 0; }
static inline u32 g4x_get_vblank_counter(struct drm_crtc *crtc) { return 0; }
#endif

int bdw_enable_vblank(struct drm_crtc *crtc);
void bdw_disable_vblank(struct drm_crtc *crtc);

void gen3_irq_reset(struct intel_uncore *uncore, i915_reg_t imr,
		    i915_reg_t iir, i915_reg_t ier);
void gen3_irq_init(struct intel_uncore *uncore,
		   i915_reg_t imr, u32 imr_val,
		   i915_reg_t ier, u32 ier_val,
		   i915_reg_t iir);

#define GEN8_IRQ_RESET_NDX(uncore, type, which) \
({ \
	unsigned int which_ = which; \
	gen3_irq_reset((uncore), GEN8_##type##_IMR(which_), \
		       GEN8_##type##_IIR(which_), GEN8_##type##_IER(which_)); \
})

#define GEN3_IRQ_RESET(uncore, type) \
	gen3_irq_reset((uncore), type##IMR, type##IIR, type##IER)

#define GEN8_IRQ_INIT_NDX(uncore, type, which, imr_val, ier_val) \
({ \
	unsigned int which_ = which; \
	gen3_irq_init((uncore), \
		      GEN8_##type##_IMR(which_), imr_val, \
		      GEN8_##type##_IER(which_), ier_val, \
		      GEN8_##type##_IIR(which_)); \
})

#define GEN3_IRQ_INIT(uncore, type, imr_val, ier_val) \
	gen3_irq_init((uncore), \
		      type##IMR, imr_val, \
		      type##IER, ier_val, \
		      type##IIR)

#endif /* __I915_IRQ_H__ */
