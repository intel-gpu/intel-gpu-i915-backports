/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2020 Intel Corporation
 */

#ifndef _INTEL_DPLL_H_
#define _INTEL_DPLL_H_

#include <linux/types.h>

struct dpll;
struct drm_i915_private;
struct intel_atomic_state;
struct intel_crtc;
struct intel_crtc_state;
enum pipe;

void intel_dpll_init_clock_hook(struct drm_i915_private *dev_priv);
int intel_dpll_crtc_compute_clock(struct intel_atomic_state *state,
				  struct intel_crtc *crtc);
int intel_dpll_crtc_get_shared_dpll(struct intel_atomic_state *state,
				    struct intel_crtc *crtc);

void assert_pll_enabled(struct drm_i915_private *i915, enum pipe pipe);
void assert_pll_disabled(struct drm_i915_private *i915, enum pipe pipe);

#endif
