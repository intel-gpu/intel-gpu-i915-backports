// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

#include <linux/kernel.h>
#include <linux/string_helpers.h>

#include "intel_crtc.h"
#include "intel_cx0_phy.h"
#include "intel_de.h"
#include "intel_display.h"
#include "intel_display_types.h"
#include "intel_dpll.h"
#include "intel_panel.h"
#include "intel_pps.h"
#include "intel_snps_phy.h"

struct intel_dpll_funcs {
	int (*crtc_compute_clock)(struct intel_atomic_state *state,
				  struct intel_crtc *crtc);
	int (*crtc_get_shared_dpll)(struct intel_atomic_state *state,
				    struct intel_crtc *crtc);
};

struct intel_limit {
	struct {
		int min, max;
	} dot, vco, n, m, m1, m2, p, p1;

	struct {
		int dot_limit;
		int p2_slow, p2_fast;
	} p2;
};

static int hsw_crtc_compute_clock(struct intel_atomic_state *state,
				  struct intel_crtc *crtc)
{
	struct intel_crtc_state *crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	struct intel_encoder *encoder =
		intel_get_crtc_new_encoder(state, crtc_state);

	return intel_compute_shared_dplls(state, crtc, encoder);
}

static int hsw_crtc_get_shared_dpll(struct intel_atomic_state *state,
				    struct intel_crtc *crtc)
{
	struct intel_crtc_state *crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	struct intel_encoder *encoder =
		intel_get_crtc_new_encoder(state, crtc_state);

	return intel_reserve_shared_dplls(state, crtc, encoder);
}

static int dg2_crtc_compute_clock(struct intel_atomic_state *state,
				  struct intel_crtc *crtc)
{
	struct intel_crtc_state *crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	struct intel_encoder *encoder =
		intel_get_crtc_new_encoder(state, crtc_state);

	return intel_mpllb_calc_state(crtc_state, encoder);
}

static int mtl_crtc_compute_clock(struct intel_atomic_state *state,
				  struct intel_crtc *crtc)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	struct intel_crtc_state *crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	struct intel_encoder *encoder =
		intel_get_crtc_new_encoder(state, crtc_state);
	enum phy phy = intel_port_to_phy(i915, encoder->port);
	int ret;

	ret = intel_cx0pll_calc_state(crtc_state, encoder);
	if (ret)
		return ret;

	/* TODO: Do the readback via intel_compute_shared_dplls() */
	if (intel_is_c10phy(i915, phy))
		crtc_state->port_clock = intel_c10pll_calc_port_clock(encoder, &crtc_state->cx0pll_state.c10);
	else
		crtc_state->port_clock = intel_c20pll_calc_port_clock(encoder, &crtc_state->cx0pll_state.c20);

	crtc_state->hw.adjusted_mode.crtc_clock = intel_crtc_dotclock(crtc_state);

	return 0;
}

static const struct intel_dpll_funcs mtl_dpll_funcs = {
	.crtc_compute_clock = mtl_crtc_compute_clock,
};

static const struct intel_dpll_funcs dg2_dpll_funcs = {
	.crtc_compute_clock = dg2_crtc_compute_clock,
};

static const struct intel_dpll_funcs hsw_dpll_funcs = {
	.crtc_compute_clock = hsw_crtc_compute_clock,
	.crtc_get_shared_dpll = hsw_crtc_get_shared_dpll,
};

int intel_dpll_crtc_compute_clock(struct intel_atomic_state *state,
				  struct intel_crtc *crtc)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	struct intel_crtc_state *crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	int ret;

	drm_WARN_ON(&i915->drm, !intel_crtc_needs_modeset(crtc_state));

	if (drm_WARN_ON(&i915->drm, crtc_state->shared_dpll))
		return 0;

	memset(&crtc_state->dpll_hw_state, 0,
	       sizeof(crtc_state->dpll_hw_state));

	if (!crtc_state->hw.enable)
		return 0;

	ret = i915->dpll_funcs->crtc_compute_clock(state, crtc);
	if (ret) {
		drm_dbg_kms(&i915->drm, "[CRTC:%d:%s] Couldn't calculate DPLL settings\n",
			    crtc->base.base.id, crtc->base.name);
		return ret;
	}

	return 0;
}

int intel_dpll_crtc_get_shared_dpll(struct intel_atomic_state *state,
				    struct intel_crtc *crtc)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	struct intel_crtc_state *crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	int ret;

	drm_WARN_ON(&i915->drm, !intel_crtc_needs_modeset(crtc_state));

	if (drm_WARN_ON(&i915->drm, crtc_state->shared_dpll))
		return 0;

	if (!crtc_state->hw.enable)
		return 0;

	if (!i915->dpll_funcs->crtc_get_shared_dpll)
		return 0;

	ret = i915->dpll_funcs->crtc_get_shared_dpll(state, crtc);
	if (ret) {
		drm_dbg_kms(&i915->drm, "[CRTC:%d:%s] Couldn't get a shared DPLL\n",
			    crtc->base.base.id, crtc->base.name);
		return ret;
	}

	return 0;
}

void
intel_dpll_init_clock_hook(struct drm_i915_private *dev_priv)
{
	if (DISPLAY_VER(dev_priv) >= 14)
		dev_priv->dpll_funcs = &mtl_dpll_funcs;
	else if (IS_DG2(dev_priv))
		dev_priv->dpll_funcs = &dg2_dpll_funcs;
	else
		dev_priv->dpll_funcs = &hsw_dpll_funcs;
}
