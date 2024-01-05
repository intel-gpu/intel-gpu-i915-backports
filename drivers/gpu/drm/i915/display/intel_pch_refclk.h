/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef _INTEL_PCH_REFCLK_H_
#define _INTEL_PCH_REFCLK_H_

#include <linux/types.h>

struct drm_i915_private;

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
struct intel_crtc_state;

void lpt_program_iclkip(const struct intel_crtc_state *crtc_state);
void lpt_disable_iclkip(struct drm_i915_private *dev_priv);
int lpt_get_iclkip(struct drm_i915_private *dev_priv);

void intel_init_pch_refclk(struct drm_i915_private *dev_priv);
void lpt_disable_clkout_dp(struct drm_i915_private *dev_priv);
#else
static inline void intel_init_pch_refclk(struct drm_i915_private *dev_priv) { return; }
#endif /* CPTCFG_DRM_I915_DISPLAY */

#endif
