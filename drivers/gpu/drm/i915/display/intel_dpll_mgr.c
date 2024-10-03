/*
 * Copyright © 2006-2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <linux/string_helpers.h>

#include "intel_de.h"
#include "intel_display_types.h"
#include "intel_dpll.h"
#include "intel_dpll_mgr.h"
#include "intel_tc.h"
#include "intel_tc_phy_regs.h"
#ifdef BPM_ABS_DIFF_PRESENT
#include <linux/math.h>
#endif

/**
 * DOC: Display PLLs
 *
 * Display PLLs used for driving outputs vary by platform. While some have
 * per-pipe or per-encoder dedicated PLLs, others allow the use of any PLL
 * from a pool. In the latter scenario, it is possible that multiple pipes
 * share a PLL if their configurations match.
 *
 * This file provides an abstraction over display PLLs. The function
 * intel_shared_dpll_init() initializes the PLLs for the given platform.  The
 * users of a PLL are tracked and that tracking is integrated with the atomic
 * modset interface. During an atomic operation, required PLLs can be reserved
 * for a given CRTC and encoder configuration by calling
 * intel_reserve_shared_dplls() and previously reserved PLLs can be released
 * with intel_release_shared_dplls().
 * Changes to the users are first staged in the atomic state, and then made
 * effective by calling intel_shared_dpll_swap_state() during the atomic
 * commit phase.
 */

/* platform specific hooks for managing DPLLs */
struct intel_shared_dpll_funcs {
	/*
	 * Hook for enabling the pll, called from intel_enable_shared_dpll() if
	 * the pll is not already enabled.
	 */
	void (*enable)(struct drm_i915_private *i915,
		       struct intel_shared_dpll *pll);

	/*
	 * Hook for disabling the pll, called from intel_disable_shared_dpll()
	 * only when it is safe to disable the pll, i.e., there are no more
	 * tracked users for it.
	 */
	void (*disable)(struct drm_i915_private *i915,
			struct intel_shared_dpll *pll);

	/*
	 * Hook for reading the values currently programmed to the DPLL
	 * registers. This is used for initial hw state readout and state
	 * verification after a mode set.
	 */
	bool (*get_hw_state)(struct drm_i915_private *i915,
			     struct intel_shared_dpll *pll,
			     struct intel_dpll_hw_state *hw_state);

	/*
	 * Hook for calculating the pll's output frequency based on its passed
	 * in state.
	 */
	int (*get_freq)(struct drm_i915_private *i915,
			const struct intel_shared_dpll *pll,
			const struct intel_dpll_hw_state *pll_state);
};

struct intel_dpll_mgr {
	const struct dpll_info *dpll_info;

	int (*compute_dplls)(struct intel_atomic_state *state,
			     struct intel_crtc *crtc,
			     struct intel_encoder *encoder);
	int (*get_dplls)(struct intel_atomic_state *state,
			 struct intel_crtc *crtc,
			 struct intel_encoder *encoder);
	void (*put_dplls)(struct intel_atomic_state *state,
			  struct intel_crtc *crtc);
	void (*update_active_dpll)(struct intel_atomic_state *state,
				   struct intel_crtc *crtc,
				   struct intel_encoder *encoder);
	void (*update_ref_clks)(struct drm_i915_private *i915);
	void (*dump_hw_state)(struct drm_i915_private *dev_priv,
			      const struct intel_dpll_hw_state *hw_state);
};

static void
intel_atomic_duplicate_dpll_state(struct drm_i915_private *dev_priv,
				  struct intel_shared_dpll_state *shared_dpll)
{
	enum intel_dpll_id i;

	/* Copy shared dpll state */
	for (i = 0; i < dev_priv->dpll.num_shared_dpll; i++) {
		struct intel_shared_dpll *pll = &dev_priv->dpll.shared_dplls[i];

		shared_dpll[i] = pll->state;
	}
}

static struct intel_shared_dpll_state *
intel_atomic_get_shared_dpll_state(struct drm_atomic_state *s)
{
	struct intel_atomic_state *state = to_intel_atomic_state(s);

	drm_WARN_ON(s->dev, !drm_modeset_is_locked(&s->dev->mode_config.connection_mutex));

	if (!state->dpll_set) {
		state->dpll_set = true;

		intel_atomic_duplicate_dpll_state(to_i915(s->dev),
						  state->shared_dpll);
	}

	return state->shared_dpll;
}

/**
 * intel_get_shared_dpll_by_id - get a DPLL given its id
 * @dev_priv: i915 device instance
 * @id: pll id
 *
 * Returns:
 * A pointer to the DPLL with @id
 */
struct intel_shared_dpll *
intel_get_shared_dpll_by_id(struct drm_i915_private *dev_priv,
			    enum intel_dpll_id id)
{
	return &dev_priv->dpll.shared_dplls[id];
}

/**
 * intel_get_shared_dpll_id - get the id of a DPLL
 * @dev_priv: i915 device instance
 * @pll: the DPLL
 *
 * Returns:
 * The id of @pll
 */
enum intel_dpll_id
intel_get_shared_dpll_id(struct drm_i915_private *dev_priv,
			 struct intel_shared_dpll *pll)
{
	long pll_idx = pll - dev_priv->dpll.shared_dplls;

	if (drm_WARN_ON(&dev_priv->drm,
			pll_idx < 0 ||
			pll_idx >= dev_priv->dpll.num_shared_dpll))
		return -1;

	return pll_idx;
}

/* For ILK+ */
void assert_shared_dpll(struct drm_i915_private *dev_priv,
			struct intel_shared_dpll *pll,
			bool state)
{
	bool cur_state;
	struct intel_dpll_hw_state hw_state;

	if (drm_WARN(&dev_priv->drm, !pll,
		     "asserting DPLL %s with no DPLL\n", str_on_off(state)))
		return;

	cur_state = intel_dpll_get_hw_state(dev_priv, pll, &hw_state);
	I915_STATE_WARN(cur_state != state,
	     "%s assertion failure (expected %s, current %s)\n",
			pll->info->name, str_on_off(state),
			str_on_off(cur_state));
}

static enum tc_port icl_pll_id_to_tc_port(enum intel_dpll_id id)
{
	return TC_PORT_1 + id - DPLL_ID_ICL_MGPLL1;
}

enum intel_dpll_id icl_tc_port_to_pll_id(enum tc_port tc_port)
{
	return tc_port - TC_PORT_1 + DPLL_ID_ICL_MGPLL1;
}

static i915_reg_t
intel_combo_pll_enable_reg(struct drm_i915_private *i915,
			   struct intel_shared_dpll *pll)
{
	if (IS_DG1(i915))
		return DG1_DPLL_ENABLE(pll->info->id);

	return ICL_DPLL_ENABLE(pll->info->id);
}

static i915_reg_t
intel_tc_pll_enable_reg(struct drm_i915_private *i915,
			struct intel_shared_dpll *pll)
{
	const enum intel_dpll_id id = pll->info->id;
	enum tc_port tc_port = icl_pll_id_to_tc_port(id);

	if (IS_ALDERLAKE_P(i915))
		return ADLP_PORTTC_PLL_ENABLE(tc_port);

	return MG_PLL_ENABLE(tc_port);
}

/**
 * intel_enable_shared_dpll - enable a CRTC's shared DPLL
 * @crtc_state: CRTC, and its state, which has a shared DPLL
 *
 * Enable the shared DPLL used by @crtc.
 */
void intel_enable_shared_dpll(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	struct intel_shared_dpll *pll = crtc_state->shared_dpll;
	unsigned int pipe_mask = BIT(crtc->pipe);
	unsigned int old_mask;

	if (drm_WARN_ON(&dev_priv->drm, pll == NULL))
		return;

	mutex_lock(&dev_priv->dpll.lock);
	old_mask = pll->active_mask;

	if (drm_WARN_ON(&dev_priv->drm, !(pll->state.pipe_mask & pipe_mask)) ||
	    drm_WARN_ON(&dev_priv->drm, pll->active_mask & pipe_mask))
		goto out;

	pll->active_mask |= pipe_mask;

	drm_dbg_kms(&dev_priv->drm,
		    "enable %s (active 0x%x, on? %d) for [CRTC:%d:%s]\n",
		    pll->info->name, pll->active_mask, pll->on,
		    crtc->base.base.id, crtc->base.name);

	if (old_mask) {
		drm_WARN_ON(&dev_priv->drm, !pll->on);
		assert_shared_dpll_enabled(dev_priv, pll);
		goto out;
	}
	drm_WARN_ON(&dev_priv->drm, pll->on);

	drm_dbg_kms(&dev_priv->drm, "enabling %s\n", pll->info->name);
	pll->info->funcs->enable(dev_priv, pll);
	pll->on = true;

out:
	mutex_unlock(&dev_priv->dpll.lock);
}

/**
 * intel_disable_shared_dpll - disable a CRTC's shared DPLL
 * @crtc_state: CRTC, and its state, which has a shared DPLL
 *
 * Disable the shared DPLL used by @crtc.
 */
void intel_disable_shared_dpll(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	struct intel_shared_dpll *pll = crtc_state->shared_dpll;
	unsigned int pipe_mask = BIT(crtc->pipe);

	if (pll == NULL)
		return;

	mutex_lock(&dev_priv->dpll.lock);
	if (drm_WARN(&dev_priv->drm, !(pll->active_mask & pipe_mask),
		     "%s not used by [CRTC:%d:%s]\n", pll->info->name,
		     crtc->base.base.id, crtc->base.name))
		goto out;

	drm_dbg_kms(&dev_priv->drm,
		    "disable %s (active 0x%x, on? %d) for [CRTC:%d:%s]\n",
		    pll->info->name, pll->active_mask, pll->on,
		    crtc->base.base.id, crtc->base.name);

	assert_shared_dpll_enabled(dev_priv, pll);
	drm_WARN_ON(&dev_priv->drm, !pll->on);

	pll->active_mask &= ~pipe_mask;
	if (pll->active_mask)
		goto out;

	drm_dbg_kms(&dev_priv->drm, "disabling %s\n", pll->info->name);
	pll->info->funcs->disable(dev_priv, pll);
	pll->on = false;

out:
	mutex_unlock(&dev_priv->dpll.lock);
}

static struct intel_shared_dpll *
intel_find_shared_dpll(struct intel_atomic_state *state,
		       const struct intel_crtc *crtc,
		       const struct intel_dpll_hw_state *pll_state,
		       unsigned long dpll_mask)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	struct intel_shared_dpll *pll, *unused_pll = NULL;
	struct intel_shared_dpll_state *shared_dpll;
	enum intel_dpll_id i;

	shared_dpll = intel_atomic_get_shared_dpll_state(&state->base);

	drm_WARN_ON(&dev_priv->drm, dpll_mask & ~(BIT(I915_NUM_PLLS) - 1));

	for_each_set_bit(i, &dpll_mask, I915_NUM_PLLS) {
		pll = &dev_priv->dpll.shared_dplls[i];

		/* Only want to check enabled timings first */
		if (shared_dpll[i].pipe_mask == 0) {
			if (!unused_pll)
				unused_pll = pll;
			continue;
		}

		if (memcmp(pll_state,
			   &shared_dpll[i].hw_state,
			   sizeof(*pll_state)) == 0) {
			drm_dbg_kms(&dev_priv->drm,
				    "[CRTC:%d:%s] sharing existing %s (pipe mask 0x%x, active 0x%x)\n",
				    crtc->base.base.id, crtc->base.name,
				    pll->info->name,
				    shared_dpll[i].pipe_mask,
				    pll->active_mask);
			return pll;
		}
	}

	/* Ok no matching timings, maybe there's a free one? */
	if (unused_pll) {
		drm_dbg_kms(&dev_priv->drm, "[CRTC:%d:%s] allocated %s\n",
			    crtc->base.base.id, crtc->base.name,
			    unused_pll->info->name);
		return unused_pll;
	}

	return NULL;
}

static void
intel_reference_shared_dpll(struct intel_atomic_state *state,
			    const struct intel_crtc *crtc,
			    const struct intel_shared_dpll *pll,
			    const struct intel_dpll_hw_state *pll_state)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	struct intel_shared_dpll_state *shared_dpll;
	const enum intel_dpll_id id = pll->info->id;

	shared_dpll = intel_atomic_get_shared_dpll_state(&state->base);

	if (shared_dpll[id].pipe_mask == 0)
		shared_dpll[id].hw_state = *pll_state;

	drm_dbg(&i915->drm, "using %s for pipe %c\n", pll->info->name,
		pipe_name(crtc->pipe));

	shared_dpll[id].pipe_mask |= BIT(crtc->pipe);
}

static void intel_unreference_shared_dpll(struct intel_atomic_state *state,
					  const struct intel_crtc *crtc,
					  const struct intel_shared_dpll *pll)
{
	struct intel_shared_dpll_state *shared_dpll;

	shared_dpll = intel_atomic_get_shared_dpll_state(&state->base);
	shared_dpll[pll->info->id].pipe_mask &= ~BIT(crtc->pipe);
}


/**
 * intel_shared_dpll_swap_state - make atomic DPLL configuration effective
 * @state: atomic state
 *
 * This is the dpll version of drm_atomic_helper_swap_state() since the
 * helper does not handle driver-specific global state.
 *
 * For consistency with atomic helpers this function does a complete swap,
 * i.e. it also puts the current state into @state, even though there is no
 * need for that at this moment.
 */
void intel_shared_dpll_swap_state(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	struct intel_shared_dpll_state *shared_dpll = state->shared_dpll;
	enum intel_dpll_id i;

	if (!state->dpll_set)
		return;

	for (i = 0; i < dev_priv->dpll.num_shared_dpll; i++) {
		struct intel_shared_dpll *pll =
			&dev_priv->dpll.shared_dplls[i];

		swap(pll->state, shared_dpll[i]);
	}
}

#define LC_FREQ 2700
#define LC_FREQ_2K U64_C(LC_FREQ * 2000)

#define P_MIN 2
#define P_MAX 64
#define P_INC 2

/* Constraints for PLL good behavior */
#define REF_MIN 48
#define REF_MAX 400
#define VCO_MIN 2400
#define VCO_MAX 4800

struct hsw_wrpll_rnp {
	unsigned p, n2, r2;
};

struct skl_dpll_regs {
	i915_reg_t ctl, cfgcr1, cfgcr2;
};

/* this array is indexed by the *shared* pll id */
struct skl_wrpll_context {
	u64 min_deviation;		/* current minimal deviation */
	u64 central_freq;		/* chosen central freq */
	u64 dco_freq;			/* chosen dco freq */
	unsigned int p;			/* chosen divider */
};

/* DCO freq must be within +1%/-6%  of the DCO central freq */
#define SKL_DCO_MAX_PDEVIATION	100
#define SKL_DCO_MAX_NDEVIATION	600

struct skl_wrpll_params {
	u32 dco_fraction;
	u32 dco_integer;
	u32 qdiv_ratio;
	u32 qdiv_mode;
	u32 kdiv;
	u32 pdiv;
	u32 central_freq;
};

static void icl_wrpll_get_multipliers(int bestdiv, int *pdiv,
				      int *qdiv, int *kdiv)
{
	/* even dividers */
	if (bestdiv % 2 == 0) {
		if (bestdiv == 2) {
			*pdiv = 2;
			*qdiv = 1;
			*kdiv = 1;
		} else if (bestdiv % 4 == 0) {
			*pdiv = 2;
			*qdiv = bestdiv / 4;
			*kdiv = 2;
		} else if (bestdiv % 6 == 0) {
			*pdiv = 3;
			*qdiv = bestdiv / 6;
			*kdiv = 2;
		} else if (bestdiv % 5 == 0) {
			*pdiv = 5;
			*qdiv = bestdiv / 10;
			*kdiv = 2;
		} else if (bestdiv % 14 == 0) {
			*pdiv = 7;
			*qdiv = bestdiv / 14;
			*kdiv = 2;
		}
	} else {
		if (bestdiv == 3 || bestdiv == 5 || bestdiv == 7) {
			*pdiv = bestdiv;
			*qdiv = 1;
			*kdiv = 1;
		} else { /* 9, 15, 21 */
			*pdiv = bestdiv / 3;
			*qdiv = 1;
			*kdiv = 3;
		}
	}
}

static void icl_wrpll_params_populate(struct skl_wrpll_params *params,
				      u32 dco_freq, u32 ref_freq,
				      int pdiv, int qdiv, int kdiv)
{
	u32 dco;

	switch (kdiv) {
	case 1:
		params->kdiv = 1;
		break;
	case 2:
		params->kdiv = 2;
		break;
	case 3:
		params->kdiv = 4;
		break;
	default:
		WARN(1, "Incorrect KDiv\n");
	}

	switch (pdiv) {
	case 2:
		params->pdiv = 1;
		break;
	case 3:
		params->pdiv = 2;
		break;
	case 5:
		params->pdiv = 4;
		break;
	case 7:
		params->pdiv = 8;
		break;
	default:
		WARN(1, "Incorrect PDiv\n");
	}

	WARN_ON(kdiv != 2 && qdiv != 1);

	params->qdiv_ratio = qdiv;
	params->qdiv_mode = (qdiv == 1) ? 0 : 1;

	dco = div_u64((u64)dco_freq << 15, ref_freq);

	params->dco_integer = dco >> 15;
	params->dco_fraction = dco & 0x7fff;
}

/*
 * Display WA #22010492432: ehl, tgl, adl-s, adl-p
 * Program half of the nominal DCO divider fraction value.
 */
static bool
ehl_combo_pll_div_frac_wa_needed(struct drm_i915_private *i915)
{
	return (IS_TIGERLAKE(i915) || IS_ALDERLAKE_S(i915) || IS_ALDERLAKE_P(i915)) &&
		 i915->dpll.ref_clks.nssc == 38400;
}

struct icl_combo_pll_params {
	int clock;
	struct skl_wrpll_params wrpll;
};

/*
 * These values alrea already adjusted: they're the bits we write to the
 * registers, not the logical values.
 */
static const struct icl_combo_pll_params icl_dp_combo_pll_24MHz_values[] = {
	{ 540000,
	  { .dco_integer = 0x151, .dco_fraction = 0x4000,		/* [0]: 5.4 */
	    .pdiv = 0x2 /* 3 */, .kdiv = 1, .qdiv_mode = 0, .qdiv_ratio = 0, }, },
	{ 270000,
	  { .dco_integer = 0x151, .dco_fraction = 0x4000,		/* [1]: 2.7 */
	    .pdiv = 0x2 /* 3 */, .kdiv = 2, .qdiv_mode = 0, .qdiv_ratio = 0, }, },
	{ 162000,
	  { .dco_integer = 0x151, .dco_fraction = 0x4000,		/* [2]: 1.62 */
	    .pdiv = 0x4 /* 5 */, .kdiv = 2, .qdiv_mode = 0, .qdiv_ratio = 0, }, },
	{ 324000,
	  { .dco_integer = 0x151, .dco_fraction = 0x4000,		/* [3]: 3.24 */
	    .pdiv = 0x4 /* 5 */, .kdiv = 1, .qdiv_mode = 0, .qdiv_ratio = 0, }, },
	{ 216000,
	  { .dco_integer = 0x168, .dco_fraction = 0x0000,		/* [4]: 2.16 */
	    .pdiv = 0x1 /* 2 */, .kdiv = 2, .qdiv_mode = 1, .qdiv_ratio = 2, }, },
	{ 432000,
	  { .dco_integer = 0x168, .dco_fraction = 0x0000,		/* [5]: 4.32 */
	    .pdiv = 0x1 /* 2 */, .kdiv = 2, .qdiv_mode = 0, .qdiv_ratio = 0, }, },
	{ 648000,
	  { .dco_integer = 0x195, .dco_fraction = 0x0000,		/* [6]: 6.48 */
	    .pdiv = 0x2 /* 3 */, .kdiv = 1, .qdiv_mode = 0, .qdiv_ratio = 0, }, },
	{ 810000,
	  { .dco_integer = 0x151, .dco_fraction = 0x4000,		/* [7]: 8.1 */
	    .pdiv = 0x1 /* 2 */, .kdiv = 1, .qdiv_mode = 0, .qdiv_ratio = 0, }, },
};


/* Also used for 38.4 MHz values. */
static const struct icl_combo_pll_params icl_dp_combo_pll_19_2MHz_values[] = {
	{ 540000,
	  { .dco_integer = 0x1A5, .dco_fraction = 0x7000,		/* [0]: 5.4 */
	    .pdiv = 0x2 /* 3 */, .kdiv = 1, .qdiv_mode = 0, .qdiv_ratio = 0, }, },
	{ 270000,
	  { .dco_integer = 0x1A5, .dco_fraction = 0x7000,		/* [1]: 2.7 */
	    .pdiv = 0x2 /* 3 */, .kdiv = 2, .qdiv_mode = 0, .qdiv_ratio = 0, }, },
	{ 162000,
	  { .dco_integer = 0x1A5, .dco_fraction = 0x7000,		/* [2]: 1.62 */
	    .pdiv = 0x4 /* 5 */, .kdiv = 2, .qdiv_mode = 0, .qdiv_ratio = 0, }, },
	{ 324000,
	  { .dco_integer = 0x1A5, .dco_fraction = 0x7000,		/* [3]: 3.24 */
	    .pdiv = 0x4 /* 5 */, .kdiv = 1, .qdiv_mode = 0, .qdiv_ratio = 0, }, },
	{ 216000,
	  { .dco_integer = 0x1C2, .dco_fraction = 0x0000,		/* [4]: 2.16 */
	    .pdiv = 0x1 /* 2 */, .kdiv = 2, .qdiv_mode = 1, .qdiv_ratio = 2, }, },
	{ 432000,
	  { .dco_integer = 0x1C2, .dco_fraction = 0x0000,		/* [5]: 4.32 */
	    .pdiv = 0x1 /* 2 */, .kdiv = 2, .qdiv_mode = 0, .qdiv_ratio = 0, }, },
	{ 648000,
	  { .dco_integer = 0x1FA, .dco_fraction = 0x2000,		/* [6]: 6.48 */
	    .pdiv = 0x2 /* 3 */, .kdiv = 1, .qdiv_mode = 0, .qdiv_ratio = 0, }, },
	{ 810000,
	  { .dco_integer = 0x1A5, .dco_fraction = 0x7000,		/* [7]: 8.1 */
	    .pdiv = 0x1 /* 2 */, .kdiv = 1, .qdiv_mode = 0, .qdiv_ratio = 0, }, },
};

static const struct skl_wrpll_params tgl_tbt_pll_19_2MHz_values = {
	.dco_integer = 0x54, .dco_fraction = 0x3000,
	/* the following params are unused */
	.pdiv = 0, .kdiv = 0, .qdiv_mode = 0, .qdiv_ratio = 0,
};

static const struct skl_wrpll_params tgl_tbt_pll_24MHz_values = {
	.dco_integer = 0x43, .dco_fraction = 0x4000,
	/* the following params are unused */
};

static int icl_calc_dp_combo_pll(struct intel_crtc_state *crtc_state,
				 struct skl_wrpll_params *pll_params)
{
	struct drm_i915_private *dev_priv = to_i915(crtc_state->uapi.crtc->dev);
	const struct icl_combo_pll_params *params =
		dev_priv->dpll.ref_clks.nssc == 24000 ?
		icl_dp_combo_pll_24MHz_values :
		icl_dp_combo_pll_19_2MHz_values;
	int clock = crtc_state->port_clock;
	int i;

	for (i = 0; i < ARRAY_SIZE(icl_dp_combo_pll_24MHz_values); i++) {
		if (clock == params[i].clock) {
			*pll_params = params[i].wrpll;
			return 0;
		}
	}

	MISSING_CASE(clock);
	return -EINVAL;
}

static int icl_calc_tbt_pll(struct intel_crtc_state *crtc_state,
			    struct skl_wrpll_params *pll_params)
{
	struct drm_i915_private *dev_priv = to_i915(crtc_state->uapi.crtc->dev);

	switch (dev_priv->dpll.ref_clks.nssc) {
	default:
		MISSING_CASE(dev_priv->dpll.ref_clks.nssc);
		fallthrough;
	case 19200:
	case 38400:
		*pll_params = tgl_tbt_pll_19_2MHz_values;
		break;
	case 24000:
		*pll_params = tgl_tbt_pll_24MHz_values;
		break;
	}

	return 0;
}

static int icl_ddi_tbt_pll_get_freq(struct drm_i915_private *i915,
				    const struct intel_shared_dpll *pll,
				    const struct intel_dpll_hw_state *pll_state)
{
	/*
	 * The PLL outputs multiple frequencies at the same time, selection is
	 * made at DDI clock mux level.
	 */
	drm_WARN_ON(&i915->drm, 1);

	return 0;
}

static int icl_wrpll_ref_clock(struct drm_i915_private *i915)
{
	int ref_clock = i915->dpll.ref_clks.nssc;

	/*
	 * For ICL+, the spec states: if reference frequency is 38.4,
	 * use 19.2 because the DPLL automatically divides that by 2.
	 */
	if (ref_clock == 38400)
		ref_clock = 19200;

	return ref_clock;
}

static int
icl_calc_wrpll(struct intel_crtc_state *crtc_state,
	       struct skl_wrpll_params *wrpll_params)
{
	struct drm_i915_private *i915 = to_i915(crtc_state->uapi.crtc->dev);
	int ref_clock = icl_wrpll_ref_clock(i915);
	u32 afe_clock = crtc_state->port_clock * 5;
	u32 dco_min = 7998000;
	u32 dco_max = 10000000;
	u32 dco_mid = (dco_min + dco_max) / 2;
	static const int dividers[] = {  2,  4,  6,  8, 10, 12,  14,  16,
					 18, 20, 24, 28, 30, 32,  36,  40,
					 42, 44, 48, 50, 52, 54,  56,  60,
					 64, 66, 68, 70, 72, 76,  78,  80,
					 84, 88, 90, 92, 96, 98, 100, 102,
					  3,  5,  7,  9, 15, 21 };
	u32 dco, best_dco = 0, dco_centrality = 0;
	u32 best_dco_centrality = U32_MAX; /* Spec meaning of 999999 MHz */
	int d, best_div = 0, pdiv = 0, qdiv = 0, kdiv = 0;

	for (d = 0; d < ARRAY_SIZE(dividers); d++) {
		dco = afe_clock * dividers[d];

		if (dco <= dco_max && dco >= dco_min) {
			dco_centrality = abs(dco - dco_mid);

			if (dco_centrality < best_dco_centrality) {
				best_dco_centrality = dco_centrality;
				best_div = dividers[d];
				best_dco = dco;
			}
		}
	}

	if (best_div == 0)
		return -EINVAL;

	icl_wrpll_get_multipliers(best_div, &pdiv, &qdiv, &kdiv);
	icl_wrpll_params_populate(wrpll_params, best_dco, ref_clock,
				  pdiv, qdiv, kdiv);

	return 0;
}

static int icl_ddi_combo_pll_get_freq(struct drm_i915_private *i915,
				      const struct intel_shared_dpll *pll,
				      const struct intel_dpll_hw_state *pll_state)
{
	int ref_clock = icl_wrpll_ref_clock(i915);
	u32 dco_fraction;
	u32 p0, p1, p2, dco_freq;

	p0 = pll_state->cfgcr1 & DPLL_CFGCR1_PDIV_MASK;
	p2 = pll_state->cfgcr1 & DPLL_CFGCR1_KDIV_MASK;

	if (pll_state->cfgcr1 & DPLL_CFGCR1_QDIV_MODE(1))
		p1 = (pll_state->cfgcr1 & DPLL_CFGCR1_QDIV_RATIO_MASK) >>
			DPLL_CFGCR1_QDIV_RATIO_SHIFT;
	else
		p1 = 1;

	switch (p0) {
	case DPLL_CFGCR1_PDIV_2:
		p0 = 2;
		break;
	case DPLL_CFGCR1_PDIV_3:
		p0 = 3;
		break;
	case DPLL_CFGCR1_PDIV_5:
		p0 = 5;
		break;
	case DPLL_CFGCR1_PDIV_7:
		p0 = 7;
		break;
	}

	switch (p2) {
	case DPLL_CFGCR1_KDIV_1:
		p2 = 1;
		break;
	case DPLL_CFGCR1_KDIV_2:
		p2 = 2;
		break;
	case DPLL_CFGCR1_KDIV_3:
		p2 = 3;
		break;
	}

	dco_freq = (pll_state->cfgcr0 & DPLL_CFGCR0_DCO_INTEGER_MASK) *
		   ref_clock;

	dco_fraction = (pll_state->cfgcr0 & DPLL_CFGCR0_DCO_FRACTION_MASK) >>
		       DPLL_CFGCR0_DCO_FRACTION_SHIFT;

	if (ehl_combo_pll_div_frac_wa_needed(i915))
		dco_fraction *= 2;

	dco_freq += (dco_fraction * ref_clock) / 0x8000;

	if (drm_WARN_ON(&i915->drm, p0 == 0 || p1 == 0 || p2 == 0))
		return 0;

	return dco_freq / (p0 * p1 * p2 * 5);
}

static void icl_calc_dpll_state(struct drm_i915_private *i915,
				const struct skl_wrpll_params *pll_params,
				struct intel_dpll_hw_state *pll_state)
{
	u32 dco_fraction = pll_params->dco_fraction;

	if (ehl_combo_pll_div_frac_wa_needed(i915))
		dco_fraction = DIV_ROUND_CLOSEST(dco_fraction, 2);

	pll_state->cfgcr0 = DPLL_CFGCR0_DCO_FRACTION(dco_fraction) |
			    pll_params->dco_integer;

	pll_state->cfgcr1 = DPLL_CFGCR1_QDIV_RATIO(pll_params->qdiv_ratio) |
			    DPLL_CFGCR1_QDIV_MODE(pll_params->qdiv_mode) |
			    DPLL_CFGCR1_KDIV(pll_params->kdiv) |
			    DPLL_CFGCR1_PDIV(pll_params->pdiv);

	pll_state->cfgcr1 |= TGL_DPLL_CFGCR1_CFSELOVRD_NORMAL_XTAL;

	if (i915->vbt.override_afc_startup)
		pll_state->div0 = TGL_DPLL0_DIV0_AFC_STARTUP(i915->vbt.override_afc_startup_val);
}

static int icl_mg_pll_find_divisors(int clock_khz, bool is_dp,
				    u32 *target_dco_khz,
				    struct intel_dpll_hw_state *state)
{
	static const u8 div1_vals[] = { 7, 5, 3, 2 };
	u32 dco_min_freq, dco_max_freq;
	unsigned int i;
	int div2;

	dco_min_freq = is_dp ? 8100000 :  7992000;
	dco_max_freq = is_dp ? 8100000 : 10000000;

	for (i = 0; i < ARRAY_SIZE(div1_vals); i++) {
		int div1 = div1_vals[i];

		for (div2 = 10; div2 > 0; div2--) {
			int dco = div1 * div2 * clock_khz * 5;
			int a_divratio, tlinedrv, inputsel;
			u32 hsdiv;

			if (dco < dco_min_freq || dco > dco_max_freq)
				continue;

			if (div2 >= 2) {
				/*
				 * Note: a_divratio not matching TGL BSpec
				 * algorithm but matching hardcoded values and
				 * working on HW for DP alt-mode at least
				 */
				a_divratio = is_dp ? 10 : 5;
				tlinedrv = 1;
			} else {
				a_divratio = 5;
				tlinedrv = 0;
			}
			inputsel = is_dp ? 0 : 1;

			switch (div1) {
			default:
				MISSING_CASE(div1);
				fallthrough;
			case 2:
				hsdiv = MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_2;
				break;
			case 3:
				hsdiv = MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_3;
				break;
			case 5:
				hsdiv = MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_5;
				break;
			case 7:
				hsdiv = MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_7;
				break;
			}

			*target_dco_khz = dco;

			state->mg_refclkin_ctl = MG_REFCLKIN_CTL_OD_2_MUX(1);

			state->mg_clktop2_coreclkctl1 =
				MG_CLKTOP2_CORECLKCTL1_A_DIVRATIO(a_divratio);

			state->mg_clktop2_hsclkctl =
				MG_CLKTOP2_HSCLKCTL_TLINEDRV_CLKSEL(tlinedrv) |
				MG_CLKTOP2_HSCLKCTL_CORE_INPUTSEL(inputsel) |
				hsdiv |
				MG_CLKTOP2_HSCLKCTL_DSDIV_RATIO(div2);

			return 0;
		}
	}

	return -EINVAL;
}

/*
 * The specification for this function uses real numbers, so the math had to be
 * adapted to integer-only calculation, that's why it looks so different.
 */
static int icl_calc_mg_pll_state(struct intel_crtc_state *crtc_state,
				 struct intel_dpll_hw_state *pll_state)
{
	struct drm_i915_private *dev_priv = to_i915(crtc_state->uapi.crtc->dev);
	int refclk_khz = dev_priv->dpll.ref_clks.nssc;
	int clock = crtc_state->port_clock;
	u32 dco_khz, m1div, m2div_int, m2div_rem, m2div_frac;
	u32 iref_ndiv, iref_trim, iref_pulse_w;
	u32 prop_coeff, int_coeff;
	u32 tdc_targetcnt, feedfwgain;
	u64 ssc_stepsize, ssc_steplen, ssc_steplog;
	u64 tmp;
	bool is_dp = !intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI);
	int ret;

	ret = icl_mg_pll_find_divisors(clock, is_dp, &dco_khz, pll_state);
	if (ret)
		return ret;

	m1div = 2;
	m2div_int = dco_khz / (refclk_khz * m1div);
	if (m2div_int > 255)
		return -EINVAL;
	m2div_rem = dco_khz % (refclk_khz * m1div);

	tmp = (u64)m2div_rem * (1 << 22);
	do_div(tmp, refclk_khz * m1div);
	m2div_frac = tmp;

	switch (refclk_khz) {
	case 19200:
		iref_ndiv = 1;
		iref_trim = 28;
		iref_pulse_w = 1;
		break;
	case 24000:
		iref_ndiv = 1;
		iref_trim = 25;
		iref_pulse_w = 2;
		break;
	case 38400:
		iref_ndiv = 2;
		iref_trim = 28;
		iref_pulse_w = 1;
		break;
	default:
		MISSING_CASE(refclk_khz);
		return -EINVAL;
	}

	/*
	 * tdc_res = 0.000003
	 * tdc_targetcnt = int(2 / (tdc_res * 8 * 50 * 1.1) / refclk_mhz + 0.5)
	 *
	 * The multiplication by 1000 is due to refclk MHz to KHz conversion. It
	 * was supposed to be a division, but we rearranged the operations of
	 * the formula to avoid early divisions so we don't multiply the
	 * rounding errors.
	 *
	 * 0.000003 * 8 * 50 * 1.1 = 0.00132, also known as 132 / 100000, which
	 * we also rearrange to work with integers.
	 *
	 * The 0.5 transformed to 5 results in a multiplication by 10 and the
	 * last division by 10.
	 */
	tdc_targetcnt = (2 * 1000 * 100000 * 10 / (132 * refclk_khz) + 5) / 10;

	/*
	 * Here we divide dco_khz by 10 in order to allow the dividend to fit in
	 * 32 bits. That's not a problem since we round the division down
	 * anyway.
	 */
	feedfwgain = m2div_rem > 0 ?  m1div * 1000000 * 100 / (dco_khz * 3 / 10) : 0;

	if (dco_khz >= 9000000) {
		prop_coeff = 5;
		int_coeff = 10;
	} else {
		prop_coeff = 4;
		int_coeff = 8;
	}

	ssc_stepsize = 0;
	ssc_steplen = 0;
	ssc_steplog = 4;

	/* write pll_state calculations */
	pll_state->mg_pll_div0 = DKL_PLL_DIV0_INTEG_COEFF(int_coeff) |
		DKL_PLL_DIV0_PROP_COEFF(prop_coeff) |
		DKL_PLL_DIV0_FBPREDIV(m1div) |
		DKL_PLL_DIV0_FBDIV_INT(m2div_int);
	if (dev_priv->vbt.override_afc_startup) {
		u8 val = dev_priv->vbt.override_afc_startup_val;

		pll_state->mg_pll_div0 |= DKL_PLL_DIV0_AFC_STARTUP(val);
	}

	pll_state->mg_pll_div1 = DKL_PLL_DIV1_IREF_TRIM(iref_trim) |
		DKL_PLL_DIV1_TDC_TARGET_CNT(tdc_targetcnt);

	pll_state->mg_pll_ssc = DKL_PLL_SSC_IREF_NDIV_RATIO(iref_ndiv) |
		DKL_PLL_SSC_STEP_LEN(ssc_steplen) |
		DKL_PLL_SSC_STEP_NUM(ssc_steplog);

	pll_state->mg_pll_bias = (m2div_frac ? DKL_PLL_BIAS_FRAC_EN_H : 0) |
		DKL_PLL_BIAS_FBDIV_FRAC(m2div_frac);

	pll_state->mg_pll_tdc_coldst_bias =
		DKL_PLL_TDC_SSC_STEP_SIZE(ssc_stepsize) |
		DKL_PLL_TDC_FEED_FWD_GAIN(feedfwgain);

	return 0;
}

static int icl_ddi_mg_pll_get_freq(struct drm_i915_private *dev_priv,
				   const struct intel_shared_dpll *pll,
				   const struct intel_dpll_hw_state *pll_state)
{
	u32 m1, m2_int, m2_frac, div1, div2, ref_clock;
	u64 tmp;

	ref_clock = dev_priv->dpll.ref_clks.nssc;

	m1 = pll_state->mg_pll_div0 & DKL_PLL_DIV0_FBPREDIV_MASK;
	m1 = m1 >> DKL_PLL_DIV0_FBPREDIV_SHIFT;
	m2_int = pll_state->mg_pll_div0 & DKL_PLL_DIV0_FBDIV_INT_MASK;

	if (pll_state->mg_pll_bias & DKL_PLL_BIAS_FRAC_EN_H) {
		m2_frac = pll_state->mg_pll_bias &
			DKL_PLL_BIAS_FBDIV_FRAC_MASK;
		m2_frac = m2_frac >> DKL_PLL_BIAS_FBDIV_SHIFT;
	} else {
		m2_frac = 0;
	}

	switch (pll_state->mg_clktop2_hsclkctl &
		MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_MASK) {
	case MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_2:
		div1 = 2;
		break;
	case MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_3:
		div1 = 3;
		break;
	case MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_5:
		div1 = 5;
		break;
	case MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_7:
		div1 = 7;
		break;
	default:
		MISSING_CASE(pll_state->mg_clktop2_hsclkctl);
		return 0;
	}

	div2 = (pll_state->mg_clktop2_hsclkctl &
		MG_CLKTOP2_HSCLKCTL_DSDIV_RATIO_MASK) >>
		MG_CLKTOP2_HSCLKCTL_DSDIV_RATIO_SHIFT;

	/* div2 value of 0 is same as 1 means no div */
	if (div2 == 0)
		div2 = 1;

	/*
	 * Adjust the original formula to delay the division by 2^22 in order to
	 * minimize possible rounding errors.
	 */
	tmp = (u64)m1 * m2_int * ref_clock +
	      (((u64)m1 * m2_frac * ref_clock) >> 22);
	tmp = div_u64(tmp, 5 * div1 * div2);

	return tmp;
}

/**
 * icl_set_active_port_dpll - select the active port DPLL for a given CRTC
 * @crtc_state: state for the CRTC to select the DPLL for
 * @port_dpll_id: the active @port_dpll_id to select
 *
 * Select the given @port_dpll_id instance from the DPLLs reserved for the
 * CRTC.
 */
void icl_set_active_port_dpll(struct intel_crtc_state *crtc_state,
			      enum icl_port_dpll_id port_dpll_id)
{
	struct icl_port_dpll *port_dpll =
		&crtc_state->icl_port_dplls[port_dpll_id];

	crtc_state->shared_dpll = port_dpll->pll;
	crtc_state->dpll_hw_state = port_dpll->hw_state;
}

static void icl_update_active_dpll(struct intel_atomic_state *state,
				   struct intel_crtc *crtc,
				   struct intel_encoder *encoder)
{
	struct intel_crtc_state *crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	struct intel_digital_port *primary_port;
	enum icl_port_dpll_id port_dpll_id = ICL_PORT_DPLL_DEFAULT;

	primary_port = encoder->type == INTEL_OUTPUT_DP_MST ?
		enc_to_mst(encoder)->primary :
		enc_to_dig_port(encoder);

	if (primary_port &&
	    (intel_tc_port_in_dp_alt_mode(primary_port) ||
	     intel_tc_port_in_legacy_mode(primary_port)))
		port_dpll_id = ICL_PORT_DPLL_MG_PHY;

	icl_set_active_port_dpll(crtc_state, port_dpll_id);
}

static u32 intel_get_hti_plls(struct drm_i915_private *i915)
{
	if (!(i915->hti_state & HDPORT_ENABLED))
		return 0;

	return REG_FIELD_GET(HDPORT_DPLL_USED_MASK, i915->hti_state);
}

static int icl_compute_combo_phy_dpll(struct intel_atomic_state *state,
				      struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	struct intel_crtc_state *crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	struct icl_port_dpll *port_dpll =
		&crtc_state->icl_port_dplls[ICL_PORT_DPLL_DEFAULT];
	struct skl_wrpll_params pll_params = {};
	int ret;

	if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI) ||
	    intel_crtc_has_type(crtc_state, INTEL_OUTPUT_DSI))
		ret = icl_calc_wrpll(crtc_state, &pll_params);
	else
		ret = icl_calc_dp_combo_pll(crtc_state, &pll_params);

	if (ret)
		return ret;

	icl_calc_dpll_state(dev_priv, &pll_params, &port_dpll->hw_state);

	return 0;
}

static int icl_get_combo_phy_dpll(struct intel_atomic_state *state,
				  struct intel_crtc *crtc,
				  struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	struct intel_crtc_state *crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	struct icl_port_dpll *port_dpll =
		&crtc_state->icl_port_dplls[ICL_PORT_DPLL_DEFAULT];
	enum port port = encoder->port;
	unsigned long dpll_mask;

	if (IS_ALDERLAKE_S(dev_priv)) {
		dpll_mask =
			BIT(DPLL_ID_DG1_DPLL3) |
			BIT(DPLL_ID_DG1_DPLL2) |
			BIT(DPLL_ID_ICL_DPLL1) |
			BIT(DPLL_ID_ICL_DPLL0);
	} else if (IS_DG1(dev_priv)) {
		if (port == PORT_D || port == PORT_E) {
			dpll_mask =
				BIT(DPLL_ID_DG1_DPLL2) |
				BIT(DPLL_ID_DG1_DPLL3);
		} else {
			dpll_mask =
				BIT(DPLL_ID_DG1_DPLL0) |
				BIT(DPLL_ID_DG1_DPLL1);
		}
	} else if (IS_ROCKETLAKE(dev_priv)) {
		dpll_mask =
			BIT(DPLL_ID_EHL_DPLL4) |
			BIT(DPLL_ID_ICL_DPLL1) |
			BIT(DPLL_ID_ICL_DPLL0);
	} else {
		dpll_mask = BIT(DPLL_ID_ICL_DPLL1) | BIT(DPLL_ID_ICL_DPLL0);
	}

	/* Eliminate DPLLs from consideration if reserved by HTI */
	dpll_mask &= ~intel_get_hti_plls(dev_priv);

	port_dpll->pll = intel_find_shared_dpll(state, crtc,
						&port_dpll->hw_state,
						dpll_mask);
	if (!port_dpll->pll)
		return -EINVAL;

	intel_reference_shared_dpll(state, crtc,
				    port_dpll->pll, &port_dpll->hw_state);

	icl_update_active_dpll(state, crtc, encoder);

	return 0;
}

static int icl_compute_tc_phy_dplls(struct intel_atomic_state *state,
				    struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	struct intel_crtc_state *crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	struct icl_port_dpll *port_dpll =
		&crtc_state->icl_port_dplls[ICL_PORT_DPLL_DEFAULT];
	struct skl_wrpll_params pll_params = {};
	int ret;

	port_dpll = &crtc_state->icl_port_dplls[ICL_PORT_DPLL_DEFAULT];
	ret = icl_calc_tbt_pll(crtc_state, &pll_params);
	if (ret)
		return ret;

	icl_calc_dpll_state(dev_priv, &pll_params, &port_dpll->hw_state);

	port_dpll = &crtc_state->icl_port_dplls[ICL_PORT_DPLL_MG_PHY];
	ret = icl_calc_mg_pll_state(crtc_state, &port_dpll->hw_state);
	if (ret)
		return ret;

	return 0;
}

static int icl_get_tc_phy_dplls(struct intel_atomic_state *state,
				struct intel_crtc *crtc,
				struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	struct intel_crtc_state *crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	struct icl_port_dpll *port_dpll =
		&crtc_state->icl_port_dplls[ICL_PORT_DPLL_DEFAULT];
	enum intel_dpll_id dpll_id;
	int ret;

	port_dpll = &crtc_state->icl_port_dplls[ICL_PORT_DPLL_DEFAULT];
	port_dpll->pll = intel_find_shared_dpll(state, crtc,
						&port_dpll->hw_state,
						BIT(DPLL_ID_ICL_TBTPLL));
	if (!port_dpll->pll)
		return -EINVAL;
	intel_reference_shared_dpll(state, crtc,
				    port_dpll->pll, &port_dpll->hw_state);


	port_dpll = &crtc_state->icl_port_dplls[ICL_PORT_DPLL_MG_PHY];
	dpll_id = icl_tc_port_to_pll_id(intel_port_to_tc(dev_priv,
							 encoder->port));
	port_dpll->pll = intel_find_shared_dpll(state, crtc,
						&port_dpll->hw_state,
						BIT(dpll_id));
	if (!port_dpll->pll) {
		ret = -EINVAL;
		goto err_unreference_tbt_pll;
	}
	intel_reference_shared_dpll(state, crtc,
				    port_dpll->pll, &port_dpll->hw_state);

	icl_update_active_dpll(state, crtc, encoder);

	return 0;

err_unreference_tbt_pll:
	port_dpll = &crtc_state->icl_port_dplls[ICL_PORT_DPLL_DEFAULT];
	intel_unreference_shared_dpll(state, crtc, port_dpll->pll);

	return ret;
}

static int icl_compute_dplls(struct intel_atomic_state *state,
			     struct intel_crtc *crtc,
			     struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	enum phy phy = intel_port_to_phy(dev_priv, encoder->port);

	if (intel_phy_is_combo(dev_priv, phy))
		return icl_compute_combo_phy_dpll(state, crtc);
	else if (intel_phy_is_tc(dev_priv, phy))
		return icl_compute_tc_phy_dplls(state, crtc);

	MISSING_CASE(phy);

	return 0;
}

static int icl_get_dplls(struct intel_atomic_state *state,
			 struct intel_crtc *crtc,
			 struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	enum phy phy = intel_port_to_phy(dev_priv, encoder->port);

	if (intel_phy_is_combo(dev_priv, phy))
		return icl_get_combo_phy_dpll(state, crtc, encoder);
	else if (intel_phy_is_tc(dev_priv, phy))
		return icl_get_tc_phy_dplls(state, crtc, encoder);

	MISSING_CASE(phy);

	return -EINVAL;
}

static void icl_put_dplls(struct intel_atomic_state *state,
			  struct intel_crtc *crtc)
{
	const struct intel_crtc_state *old_crtc_state =
		intel_atomic_get_old_crtc_state(state, crtc);
	struct intel_crtc_state *new_crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	enum icl_port_dpll_id id;

	new_crtc_state->shared_dpll = NULL;

	for (id = ICL_PORT_DPLL_DEFAULT; id < ICL_PORT_DPLL_COUNT; id++) {
		const struct icl_port_dpll *old_port_dpll =
			&old_crtc_state->icl_port_dplls[id];
		struct icl_port_dpll *new_port_dpll =
			&new_crtc_state->icl_port_dplls[id];

		new_port_dpll->pll = NULL;

		if (!old_port_dpll->pll)
			continue;

		intel_unreference_shared_dpll(state, crtc, old_port_dpll->pll);
	}
}

static bool dkl_pll_get_hw_state(struct drm_i915_private *dev_priv,
				 struct intel_shared_dpll *pll,
				 struct intel_dpll_hw_state *hw_state)
{
	const enum intel_dpll_id id = pll->info->id;
	enum tc_port tc_port = icl_pll_id_to_tc_port(id);
	intel_wakeref_t wakeref;
	bool ret = false;
	u32 val;

	wakeref = intel_display_power_get_if_enabled(dev_priv,
						     POWER_DOMAIN_DISPLAY_CORE);
	if (!wakeref)
		return false;

	val = intel_de_read(dev_priv, intel_tc_pll_enable_reg(dev_priv, pll));
	if (!(val & PLL_ENABLE))
		goto out;

	/*
	 * All registers read here have the same HIP_INDEX_REG even though
	 * they are on different building blocks
	 */
	intel_de_write(dev_priv, HIP_INDEX_REG(tc_port),
		       HIP_INDEX_VAL(tc_port, 0x2));

	hw_state->mg_refclkin_ctl = intel_de_read(dev_priv,
						  DKL_REFCLKIN_CTL(tc_port));
	hw_state->mg_refclkin_ctl &= MG_REFCLKIN_CTL_OD_2_MUX_MASK;

	hw_state->mg_clktop2_hsclkctl =
		intel_de_read(dev_priv, DKL_CLKTOP2_HSCLKCTL(tc_port));
	hw_state->mg_clktop2_hsclkctl &=
		MG_CLKTOP2_HSCLKCTL_TLINEDRV_CLKSEL_MASK |
		MG_CLKTOP2_HSCLKCTL_CORE_INPUTSEL_MASK |
		MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_MASK |
		MG_CLKTOP2_HSCLKCTL_DSDIV_RATIO_MASK;

	hw_state->mg_clktop2_coreclkctl1 =
		intel_de_read(dev_priv, DKL_CLKTOP2_CORECLKCTL1(tc_port));
	hw_state->mg_clktop2_coreclkctl1 &=
		MG_CLKTOP2_CORECLKCTL1_A_DIVRATIO_MASK;

	hw_state->mg_pll_div0 = intel_de_read(dev_priv, DKL_PLL_DIV0(tc_port));
	val = DKL_PLL_DIV0_MASK;
	if (dev_priv->vbt.override_afc_startup)
		val |= DKL_PLL_DIV0_AFC_STARTUP_MASK;
	hw_state->mg_pll_div0 &= val;

	hw_state->mg_pll_div1 = intel_de_read(dev_priv, DKL_PLL_DIV1(tc_port));
	hw_state->mg_pll_div1 &= (DKL_PLL_DIV1_IREF_TRIM_MASK |
				  DKL_PLL_DIV1_TDC_TARGET_CNT_MASK);

	hw_state->mg_pll_ssc = intel_de_read(dev_priv, DKL_PLL_SSC(tc_port));
	hw_state->mg_pll_ssc &= (DKL_PLL_SSC_IREF_NDIV_RATIO_MASK |
				 DKL_PLL_SSC_STEP_LEN_MASK |
				 DKL_PLL_SSC_STEP_NUM_MASK |
				 DKL_PLL_SSC_EN);

	hw_state->mg_pll_bias = intel_de_read(dev_priv, DKL_PLL_BIAS(tc_port));
	hw_state->mg_pll_bias &= (DKL_PLL_BIAS_FRAC_EN_H |
				  DKL_PLL_BIAS_FBDIV_FRAC_MASK);

	hw_state->mg_pll_tdc_coldst_bias =
		intel_de_read(dev_priv, DKL_PLL_TDC_COLDST_BIAS(tc_port));
	hw_state->mg_pll_tdc_coldst_bias &= (DKL_PLL_TDC_SSC_STEP_SIZE_MASK |
					     DKL_PLL_TDC_FEED_FWD_GAIN_MASK);

	ret = true;
out:
	intel_display_power_put(dev_priv, POWER_DOMAIN_DISPLAY_CORE, wakeref);
	return ret;
}

static bool icl_pll_get_hw_state(struct drm_i915_private *dev_priv,
				 struct intel_shared_dpll *pll,
				 struct intel_dpll_hw_state *hw_state,
				 i915_reg_t enable_reg)
{
	const enum intel_dpll_id id = pll->info->id;
	intel_wakeref_t wakeref;
	bool ret = false;
	u32 val;

	wakeref = intel_display_power_get_if_enabled(dev_priv,
						     POWER_DOMAIN_DISPLAY_CORE);
	if (!wakeref)
		return false;

	val = intel_de_read(dev_priv, enable_reg);
	if (!(val & PLL_ENABLE))
		goto out;

	if (IS_ALDERLAKE_S(dev_priv)) {
		hw_state->cfgcr0 = intel_de_read(dev_priv, ADLS_DPLL_CFGCR0(id));
		hw_state->cfgcr1 = intel_de_read(dev_priv, ADLS_DPLL_CFGCR1(id));
	} else if (IS_DG1(dev_priv)) {
		hw_state->cfgcr0 = intel_de_read(dev_priv, DG1_DPLL_CFGCR0(id));
		hw_state->cfgcr1 = intel_de_read(dev_priv, DG1_DPLL_CFGCR1(id));
	} else if (IS_ROCKETLAKE(dev_priv)) {
		hw_state->cfgcr0 = intel_de_read(dev_priv,
						 RKL_DPLL_CFGCR0(id));
		hw_state->cfgcr1 = intel_de_read(dev_priv,
						 RKL_DPLL_CFGCR1(id));
	} else {
		hw_state->cfgcr0 = intel_de_read(dev_priv,
						 TGL_DPLL_CFGCR0(id));
		hw_state->cfgcr1 = intel_de_read(dev_priv,
						 TGL_DPLL_CFGCR1(id));
		if (dev_priv->vbt.override_afc_startup) {
			hw_state->div0 = intel_de_read(dev_priv, TGL_DPLL0_DIV0(id));
			hw_state->div0 &= TGL_DPLL0_DIV0_AFC_STARTUP_MASK;
		}
	}

	ret = true;
out:
	intel_display_power_put(dev_priv, POWER_DOMAIN_DISPLAY_CORE, wakeref);
	return ret;
}

static bool combo_pll_get_hw_state(struct drm_i915_private *dev_priv,
				   struct intel_shared_dpll *pll,
				   struct intel_dpll_hw_state *hw_state)
{
	i915_reg_t enable_reg = intel_combo_pll_enable_reg(dev_priv, pll);

	return icl_pll_get_hw_state(dev_priv, pll, hw_state, enable_reg);
}

static bool tbt_pll_get_hw_state(struct drm_i915_private *dev_priv,
				 struct intel_shared_dpll *pll,
				 struct intel_dpll_hw_state *hw_state)
{
	return icl_pll_get_hw_state(dev_priv, pll, hw_state, TBT_PLL_ENABLE);
}

static void icl_dpll_write(struct drm_i915_private *dev_priv,
			   struct intel_shared_dpll *pll)
{
	struct intel_dpll_hw_state *hw_state = &pll->state.hw_state;
	const enum intel_dpll_id id = pll->info->id;
	i915_reg_t cfgcr0_reg, cfgcr1_reg, div0_reg = INVALID_MMIO_REG;

	if (IS_ALDERLAKE_S(dev_priv)) {
		cfgcr0_reg = ADLS_DPLL_CFGCR0(id);
		cfgcr1_reg = ADLS_DPLL_CFGCR1(id);
	} else if (IS_DG1(dev_priv)) {
		cfgcr0_reg = DG1_DPLL_CFGCR0(id);
		cfgcr1_reg = DG1_DPLL_CFGCR1(id);
	} else if (IS_ROCKETLAKE(dev_priv)) {
		cfgcr0_reg = RKL_DPLL_CFGCR0(id);
		cfgcr1_reg = RKL_DPLL_CFGCR1(id);
	} else {
		cfgcr0_reg = TGL_DPLL_CFGCR0(id);
		cfgcr1_reg = TGL_DPLL_CFGCR1(id);
		div0_reg = TGL_DPLL0_DIV0(id);
	}

	intel_de_write(dev_priv, cfgcr0_reg, hw_state->cfgcr0);
	intel_de_write(dev_priv, cfgcr1_reg, hw_state->cfgcr1);
	drm_WARN_ON_ONCE(&dev_priv->drm, dev_priv->vbt.override_afc_startup &&
			 !i915_mmio_reg_valid(div0_reg));
	if (dev_priv->vbt.override_afc_startup &&
	    i915_mmio_reg_valid(div0_reg))
		intel_de_rmw(dev_priv, div0_reg, TGL_DPLL0_DIV0_AFC_STARTUP_MASK,
			     hw_state->div0);
	intel_de_posting_read(dev_priv, cfgcr1_reg);
}

static void dkl_pll_write(struct drm_i915_private *dev_priv,
			  struct intel_shared_dpll *pll)
{
	struct intel_dpll_hw_state *hw_state = &pll->state.hw_state;
	enum tc_port tc_port = icl_pll_id_to_tc_port(pll->info->id);
	u32 val;

	/*
	 * All registers programmed here have the same HIP_INDEX_REG even
	 * though on different building block
	 */
	intel_de_write(dev_priv, HIP_INDEX_REG(tc_port),
		       HIP_INDEX_VAL(tc_port, 0x2));

	/* All the registers are RMW */
	val = intel_de_read(dev_priv, DKL_REFCLKIN_CTL(tc_port));
	val &= ~MG_REFCLKIN_CTL_OD_2_MUX_MASK;
	val |= hw_state->mg_refclkin_ctl;
	intel_de_write(dev_priv, DKL_REFCLKIN_CTL(tc_port), val);

	val = intel_de_read(dev_priv, DKL_CLKTOP2_CORECLKCTL1(tc_port));
	val &= ~MG_CLKTOP2_CORECLKCTL1_A_DIVRATIO_MASK;
	val |= hw_state->mg_clktop2_coreclkctl1;
	intel_de_write(dev_priv, DKL_CLKTOP2_CORECLKCTL1(tc_port), val);

	val = intel_de_read(dev_priv, DKL_CLKTOP2_HSCLKCTL(tc_port));
	val &= ~(MG_CLKTOP2_HSCLKCTL_TLINEDRV_CLKSEL_MASK |
		 MG_CLKTOP2_HSCLKCTL_CORE_INPUTSEL_MASK |
		 MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_MASK |
		 MG_CLKTOP2_HSCLKCTL_DSDIV_RATIO_MASK);
	val |= hw_state->mg_clktop2_hsclkctl;
	intel_de_write(dev_priv, DKL_CLKTOP2_HSCLKCTL(tc_port), val);

	val = DKL_PLL_DIV0_MASK;
	if (dev_priv->vbt.override_afc_startup)
		val |= DKL_PLL_DIV0_AFC_STARTUP_MASK;
	intel_de_rmw(dev_priv, DKL_PLL_DIV0(tc_port), val,
		     hw_state->mg_pll_div0);

	val = intel_de_read(dev_priv, DKL_PLL_DIV1(tc_port));
	val &= ~(DKL_PLL_DIV1_IREF_TRIM_MASK |
		 DKL_PLL_DIV1_TDC_TARGET_CNT_MASK);
	val |= hw_state->mg_pll_div1;
	intel_de_write(dev_priv, DKL_PLL_DIV1(tc_port), val);

	val = intel_de_read(dev_priv, DKL_PLL_SSC(tc_port));
	val &= ~(DKL_PLL_SSC_IREF_NDIV_RATIO_MASK |
		 DKL_PLL_SSC_STEP_LEN_MASK |
		 DKL_PLL_SSC_STEP_NUM_MASK |
		 DKL_PLL_SSC_EN);
	val |= hw_state->mg_pll_ssc;
	intel_de_write(dev_priv, DKL_PLL_SSC(tc_port), val);

	val = intel_de_read(dev_priv, DKL_PLL_BIAS(tc_port));
	val &= ~(DKL_PLL_BIAS_FRAC_EN_H |
		 DKL_PLL_BIAS_FBDIV_FRAC_MASK);
	val |= hw_state->mg_pll_bias;
	intel_de_write(dev_priv, DKL_PLL_BIAS(tc_port), val);

	val = intel_de_read(dev_priv, DKL_PLL_TDC_COLDST_BIAS(tc_port));
	val &= ~(DKL_PLL_TDC_SSC_STEP_SIZE_MASK |
		 DKL_PLL_TDC_FEED_FWD_GAIN_MASK);
	val |= hw_state->mg_pll_tdc_coldst_bias;
	intel_de_write(dev_priv, DKL_PLL_TDC_COLDST_BIAS(tc_port), val);

	intel_de_posting_read(dev_priv, DKL_PLL_TDC_COLDST_BIAS(tc_port));
}

static void icl_pll_power_enable(struct drm_i915_private *dev_priv,
				 struct intel_shared_dpll *pll,
				 i915_reg_t enable_reg)
{
	u32 val;

	val = intel_de_read(dev_priv, enable_reg);
	val |= PLL_POWER_ENABLE;
	intel_de_write(dev_priv, enable_reg, val);

	/*
	 * The spec says we need to "wait" but it also says it should be
	 * immediate.
	 */
	if (intel_de_wait_for_set(dev_priv, enable_reg, PLL_POWER_STATE, 1))
		drm_err(&dev_priv->drm, "PLL %d Power not enabled\n",
			pll->info->id);
}

static void icl_pll_enable(struct drm_i915_private *dev_priv,
			   struct intel_shared_dpll *pll,
			   i915_reg_t enable_reg)
{
	u32 val;

	val = intel_de_read(dev_priv, enable_reg);
	val |= PLL_ENABLE;
	intel_de_write(dev_priv, enable_reg, val);

	/* Timeout is actually 600us. */
	if (intel_de_wait_for_set(dev_priv, enable_reg, PLL_LOCK, 1))
		drm_err(&dev_priv->drm, "PLL %d not locked\n", pll->info->id);
}

static void adlp_cmtg_clock_gating_wa(struct drm_i915_private *i915, struct intel_shared_dpll *pll)
{
	u32 val;

	if (!IS_ADLP_DISPLAY_STEP(i915, STEP_A0, STEP_B0) ||
	    pll->info->id != DPLL_ID_ICL_DPLL0)
		return;
	/*
	 * Wa_16011069516:adl-p[a0]
	 *
	 * All CMTG regs are unreliable until CMTG clock gating is disabled,
	 * so we can only assume the default TRANS_CMTG_CHICKEN reg value and
	 * sanity check this assumption with a double read, which presumably
	 * returns the correct value even with clock gating on.
	 *
	 * Instead of the usual place for workarounds we apply this one here,
	 * since TRANS_CMTG_CHICKEN is only accessible while DPLL0 is enabled.
	 */
	val = intel_de_read(i915, TRANS_CMTG_CHICKEN);
	val = intel_de_read(i915, TRANS_CMTG_CHICKEN);
	intel_de_write(i915, TRANS_CMTG_CHICKEN, DISABLE_DPT_CLK_GATING);
	if (drm_WARN_ON(&i915->drm, val & ~DISABLE_DPT_CLK_GATING))
		drm_dbg_kms(&i915->drm, "Unexpected flags in TRANS_CMTG_CHICKEN: %08x\n", val);
}

static void combo_pll_enable(struct drm_i915_private *dev_priv,
			     struct intel_shared_dpll *pll)
{
	i915_reg_t enable_reg = intel_combo_pll_enable_reg(dev_priv, pll);

	icl_pll_power_enable(dev_priv, pll, enable_reg);

	icl_dpll_write(dev_priv, pll);

	/*
	 * DVFS pre sequence would be here, but in our driver the cdclk code
	 * paths should already be setting the appropriate voltage, hence we do
	 * nothing here.
	 */

	icl_pll_enable(dev_priv, pll, enable_reg);

	adlp_cmtg_clock_gating_wa(dev_priv, pll);

	/* DVFS post sequence would be here. See the comment above. */
}

static void tbt_pll_enable(struct drm_i915_private *dev_priv,
			   struct intel_shared_dpll *pll)
{
	icl_pll_power_enable(dev_priv, pll, TBT_PLL_ENABLE);

	icl_dpll_write(dev_priv, pll);

	/*
	 * DVFS pre sequence would be here, but in our driver the cdclk code
	 * paths should already be setting the appropriate voltage, hence we do
	 * nothing here.
	 */

	icl_pll_enable(dev_priv, pll, TBT_PLL_ENABLE);

	/* DVFS post sequence would be here. See the comment above. */
}

static void mg_pll_enable(struct drm_i915_private *dev_priv,
			  struct intel_shared_dpll *pll)
{
	i915_reg_t enable_reg = intel_tc_pll_enable_reg(dev_priv, pll);

	icl_pll_power_enable(dev_priv, pll, enable_reg);

	dkl_pll_write(dev_priv, pll);

	/*
	 * DVFS pre sequence would be here, but in our driver the cdclk code
	 * paths should already be setting the appropriate voltage, hence we do
	 * nothing here.
	 */

	icl_pll_enable(dev_priv, pll, enable_reg);

	/* DVFS post sequence would be here. See the comment above. */
}

static void icl_pll_disable(struct drm_i915_private *dev_priv,
			    struct intel_shared_dpll *pll,
			    i915_reg_t enable_reg)
{
	u32 val;

	/* The first steps are done by intel_ddi_post_disable(). */

	/*
	 * DVFS pre sequence would be here, but in our driver the cdclk code
	 * paths should already be setting the appropriate voltage, hence we do
	 * nothing here.
	 */

	val = intel_de_read(dev_priv, enable_reg);
	val &= ~PLL_ENABLE;
	intel_de_write(dev_priv, enable_reg, val);

	/* Timeout is actually 1us. */
	if (intel_de_wait_for_clear(dev_priv, enable_reg, PLL_LOCK, 1))
		drm_err(&dev_priv->drm, "PLL %d locked\n", pll->info->id);

	/* DVFS post sequence would be here. See the comment above. */

	val = intel_de_read(dev_priv, enable_reg);
	val &= ~PLL_POWER_ENABLE;
	intel_de_write(dev_priv, enable_reg, val);

	/*
	 * The spec says we need to "wait" but it also says it should be
	 * immediate.
	 */
	if (intel_de_wait_for_clear(dev_priv, enable_reg, PLL_POWER_STATE, 1))
		drm_err(&dev_priv->drm, "PLL %d Power not disabled\n",
			pll->info->id);
}

static void combo_pll_disable(struct drm_i915_private *dev_priv,
			      struct intel_shared_dpll *pll)
{
	i915_reg_t enable_reg = intel_combo_pll_enable_reg(dev_priv, pll);

	icl_pll_disable(dev_priv, pll, enable_reg);
}

static void tbt_pll_disable(struct drm_i915_private *dev_priv,
			    struct intel_shared_dpll *pll)
{
	icl_pll_disable(dev_priv, pll, TBT_PLL_ENABLE);
}

static void mg_pll_disable(struct drm_i915_private *dev_priv,
			   struct intel_shared_dpll *pll)
{
	i915_reg_t enable_reg = intel_tc_pll_enable_reg(dev_priv, pll);

	icl_pll_disable(dev_priv, pll, enable_reg);
}

static void icl_update_dpll_ref_clks(struct drm_i915_private *i915)
{
	/* No SSC ref */
	i915->dpll.ref_clks.nssc = i915->cdclk.hw.ref;
}

static void icl_dump_hw_state(struct drm_i915_private *dev_priv,
			      const struct intel_dpll_hw_state *hw_state)
{
	drm_dbg_kms(&dev_priv->drm,
		    "dpll_hw_state: cfgcr0: 0x%x, cfgcr1: 0x%x, div0: 0x%x, "
		    "mg_refclkin_ctl: 0x%x, hg_clktop2_coreclkctl1: 0x%x, "
		    "mg_clktop2_hsclkctl: 0x%x, mg_pll_div0: 0x%x, "
		    "mg_pll_div2: 0x%x, mg_pll_lf: 0x%x, "
		    "mg_pll_frac_lock: 0x%x, mg_pll_ssc: 0x%x, "
		    "mg_pll_bias: 0x%x, mg_pll_tdc_coldst_bias: 0x%x\n",
		    hw_state->cfgcr0, hw_state->cfgcr1,
		    hw_state->div0,
		    hw_state->mg_refclkin_ctl,
		    hw_state->mg_clktop2_coreclkctl1,
		    hw_state->mg_clktop2_hsclkctl,
		    hw_state->mg_pll_div0,
		    hw_state->mg_pll_div1,
		    hw_state->mg_pll_lf,
		    hw_state->mg_pll_frac_lock,
		    hw_state->mg_pll_ssc,
		    hw_state->mg_pll_bias,
		    hw_state->mg_pll_tdc_coldst_bias);
}

static const struct intel_shared_dpll_funcs combo_pll_funcs = {
	.enable = combo_pll_enable,
	.disable = combo_pll_disable,
	.get_hw_state = combo_pll_get_hw_state,
	.get_freq = icl_ddi_combo_pll_get_freq,
};

static const struct intel_shared_dpll_funcs tbt_pll_funcs = {
	.enable = tbt_pll_enable,
	.disable = tbt_pll_disable,
	.get_hw_state = tbt_pll_get_hw_state,
	.get_freq = icl_ddi_tbt_pll_get_freq,
};

static const struct intel_shared_dpll_funcs dkl_pll_funcs = {
	.enable = mg_pll_enable,
	.disable = mg_pll_disable,
	.get_hw_state = dkl_pll_get_hw_state,
	.get_freq = icl_ddi_mg_pll_get_freq,
};

static const struct dpll_info tgl_plls[] = {
	{ "DPLL 0", &combo_pll_funcs, DPLL_ID_ICL_DPLL0,  0 },
	{ "DPLL 1", &combo_pll_funcs, DPLL_ID_ICL_DPLL1,  0 },
	{ "TBT PLL",  &tbt_pll_funcs, DPLL_ID_ICL_TBTPLL, 0 },
	{ "TC PLL 1", &dkl_pll_funcs, DPLL_ID_ICL_MGPLL1, 0 },
	{ "TC PLL 2", &dkl_pll_funcs, DPLL_ID_ICL_MGPLL2, 0 },
	{ "TC PLL 3", &dkl_pll_funcs, DPLL_ID_ICL_MGPLL3, 0 },
	{ "TC PLL 4", &dkl_pll_funcs, DPLL_ID_ICL_MGPLL4, 0 },
	{ "TC PLL 5", &dkl_pll_funcs, DPLL_ID_TGL_MGPLL5, 0 },
	{ "TC PLL 6", &dkl_pll_funcs, DPLL_ID_TGL_MGPLL6, 0 },
	{ },
};

static const struct intel_dpll_mgr tgl_pll_mgr = {
	.dpll_info = tgl_plls,
	.compute_dplls = icl_compute_dplls,
	.get_dplls = icl_get_dplls,
	.put_dplls = icl_put_dplls,
	.update_active_dpll = icl_update_active_dpll,
	.update_ref_clks = icl_update_dpll_ref_clks,
	.dump_hw_state = icl_dump_hw_state,
};

static const struct dpll_info rkl_plls[] = {
	{ "DPLL 0", &combo_pll_funcs, DPLL_ID_ICL_DPLL0, 0 },
	{ "DPLL 1", &combo_pll_funcs, DPLL_ID_ICL_DPLL1, 0 },
	{ "DPLL 4", &combo_pll_funcs, DPLL_ID_EHL_DPLL4, 0 },
	{ },
};

static const struct intel_dpll_mgr rkl_pll_mgr = {
	.dpll_info = rkl_plls,
	.compute_dplls = icl_compute_dplls,
	.get_dplls = icl_get_dplls,
	.put_dplls = icl_put_dplls,
	.update_ref_clks = icl_update_dpll_ref_clks,
	.dump_hw_state = icl_dump_hw_state,
};

static const struct dpll_info dg1_plls[] = {
	{ "DPLL 0", &combo_pll_funcs, DPLL_ID_DG1_DPLL0, 0 },
	{ "DPLL 1", &combo_pll_funcs, DPLL_ID_DG1_DPLL1, 0 },
	{ "DPLL 2", &combo_pll_funcs, DPLL_ID_DG1_DPLL2, 0 },
	{ "DPLL 3", &combo_pll_funcs, DPLL_ID_DG1_DPLL3, 0 },
	{ },
};

static const struct intel_dpll_mgr dg1_pll_mgr = {
	.dpll_info = dg1_plls,
	.compute_dplls = icl_compute_dplls,
	.get_dplls = icl_get_dplls,
	.put_dplls = icl_put_dplls,
	.update_ref_clks = icl_update_dpll_ref_clks,
	.dump_hw_state = icl_dump_hw_state,
};

static const struct dpll_info adls_plls[] = {
	{ "DPLL 0", &combo_pll_funcs, DPLL_ID_ICL_DPLL0, 0 },
	{ "DPLL 1", &combo_pll_funcs, DPLL_ID_ICL_DPLL1, 0 },
	{ "DPLL 2", &combo_pll_funcs, DPLL_ID_DG1_DPLL2, 0 },
	{ "DPLL 3", &combo_pll_funcs, DPLL_ID_DG1_DPLL3, 0 },
	{ },
};

static const struct intel_dpll_mgr adls_pll_mgr = {
	.dpll_info = adls_plls,
	.compute_dplls = icl_compute_dplls,
	.get_dplls = icl_get_dplls,
	.put_dplls = icl_put_dplls,
	.update_ref_clks = icl_update_dpll_ref_clks,
	.dump_hw_state = icl_dump_hw_state,
};

static const struct dpll_info adlp_plls[] = {
	{ "DPLL 0", &combo_pll_funcs, DPLL_ID_ICL_DPLL0,  0 },
	{ "DPLL 1", &combo_pll_funcs, DPLL_ID_ICL_DPLL1,  0 },
	{ "TBT PLL",  &tbt_pll_funcs, DPLL_ID_ICL_TBTPLL, 0 },
	{ "TC PLL 1", &dkl_pll_funcs, DPLL_ID_ICL_MGPLL1, 0 },
	{ "TC PLL 2", &dkl_pll_funcs, DPLL_ID_ICL_MGPLL2, 0 },
	{ "TC PLL 3", &dkl_pll_funcs, DPLL_ID_ICL_MGPLL3, 0 },
	{ "TC PLL 4", &dkl_pll_funcs, DPLL_ID_ICL_MGPLL4, 0 },
	{ },
};

static const struct intel_dpll_mgr adlp_pll_mgr = {
	.dpll_info = adlp_plls,
	.compute_dplls = icl_compute_dplls,
	.get_dplls = icl_get_dplls,
	.put_dplls = icl_put_dplls,
	.update_active_dpll = icl_update_active_dpll,
	.update_ref_clks = icl_update_dpll_ref_clks,
	.dump_hw_state = icl_dump_hw_state,
};

/**
 * intel_shared_dpll_init - Initialize shared DPLLs
 * @dev_priv: i915 device
 *
 * Initialize shared DPLLs for @dev_priv.
 */
void intel_shared_dpll_init(struct drm_i915_private *dev_priv)
{
	const struct intel_dpll_mgr *dpll_mgr = NULL;
	const struct dpll_info *dpll_info;
	int i;

	if (DISPLAY_VER(dev_priv) >= 14 || IS_DG2(dev_priv))
		/* No shared DPLLs on DG2; port PLLs are part of the PHY */
		dpll_mgr = NULL;
	else if (IS_ALDERLAKE_P(dev_priv))
		dpll_mgr = &adlp_pll_mgr;
	else if (IS_ALDERLAKE_S(dev_priv))
		dpll_mgr = &adls_pll_mgr;
	else if (IS_DG1(dev_priv))
		dpll_mgr = &dg1_pll_mgr;
	else if (IS_ROCKETLAKE(dev_priv))
		dpll_mgr = &rkl_pll_mgr;
	else if (DISPLAY_VER(dev_priv) >= 12)
		dpll_mgr = &tgl_pll_mgr;

	if (!dpll_mgr) {
		dev_priv->dpll.num_shared_dpll = 0;
		return;
	}

	dpll_info = dpll_mgr->dpll_info;

	for (i = 0; dpll_info[i].name; i++) {
		drm_WARN_ON(&dev_priv->drm, i != dpll_info[i].id);
		dev_priv->dpll.shared_dplls[i].info = &dpll_info[i];
	}

	dev_priv->dpll.mgr = dpll_mgr;
	dev_priv->dpll.num_shared_dpll = i;
	mutex_init(&dev_priv->dpll.lock);

	BUG_ON(dev_priv->dpll.num_shared_dpll > I915_NUM_PLLS);
}

/**
 * intel_compute_shared_dplls - compute DPLL state CRTC and encoder combination
 * @state: atomic state
 * @crtc: CRTC to compute DPLLs for
 * @encoder: encoder
 *
 * This function computes the DPLL state for the given CRTC and encoder.
 *
 * The new configuration in the atomic commit @state is made effective by
 * calling intel_shared_dpll_swap_state().
 *
 * Returns:
 * 0 on success, negative error code on falure.
 */
int intel_compute_shared_dplls(struct intel_atomic_state *state,
			       struct intel_crtc *crtc,
			       struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	const struct intel_dpll_mgr *dpll_mgr = dev_priv->dpll.mgr;

	if (drm_WARN_ON(&dev_priv->drm, !dpll_mgr))
		return -EINVAL;

	return dpll_mgr->compute_dplls(state, crtc, encoder);
}

/**
 * intel_reserve_shared_dplls - reserve DPLLs for CRTC and encoder combination
 * @state: atomic state
 * @crtc: CRTC to reserve DPLLs for
 * @encoder: encoder
 *
 * This function reserves all required DPLLs for the given CRTC and encoder
 * combination in the current atomic commit @state and the new @crtc atomic
 * state.
 *
 * The new configuration in the atomic commit @state is made effective by
 * calling intel_shared_dpll_swap_state().
 *
 * The reserved DPLLs should be released by calling
 * intel_release_shared_dplls().
 *
 * Returns:
 * 0 if all required DPLLs were successfully reserved,
 * negative error code otherwise.
 */
int intel_reserve_shared_dplls(struct intel_atomic_state *state,
			       struct intel_crtc *crtc,
			       struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	const struct intel_dpll_mgr *dpll_mgr = dev_priv->dpll.mgr;

	if (drm_WARN_ON(&dev_priv->drm, !dpll_mgr))
		return -EINVAL;

	return dpll_mgr->get_dplls(state, crtc, encoder);
}

/**
 * intel_release_shared_dplls - end use of DPLLs by CRTC in atomic state
 * @state: atomic state
 * @crtc: crtc from which the DPLLs are to be released
 *
 * This function releases all DPLLs reserved by intel_reserve_shared_dplls()
 * from the current atomic commit @state and the old @crtc atomic state.
 *
 * The new configuration in the atomic commit @state is made effective by
 * calling intel_shared_dpll_swap_state().
 */
void intel_release_shared_dplls(struct intel_atomic_state *state,
				struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	const struct intel_dpll_mgr *dpll_mgr = dev_priv->dpll.mgr;

	/*
	 * FIXME: this function is called for every platform having a
	 * compute_clock hook, even though the platform doesn't yet support
	 * the shared DPLL framework and intel_reserve_shared_dplls() is not
	 * called on those.
	 */
	if (!dpll_mgr)
		return;

	dpll_mgr->put_dplls(state, crtc);
}

/**
 * intel_update_active_dpll - update the active DPLL for a CRTC/encoder
 * @state: atomic state
 * @crtc: the CRTC for which to update the active DPLL
 * @encoder: encoder determining the type of port DPLL
 *
 * Update the active DPLL for the given @crtc/@encoder in @crtc's atomic state,
 * from the port DPLLs reserved previously by intel_reserve_shared_dplls(). The
 * DPLL selected will be based on the current mode of the encoder's port.
 */
void intel_update_active_dpll(struct intel_atomic_state *state,
			      struct intel_crtc *crtc,
			      struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	const struct intel_dpll_mgr *dpll_mgr = dev_priv->dpll.mgr;

	if (drm_WARN_ON(&dev_priv->drm, !dpll_mgr))
		return;

	dpll_mgr->update_active_dpll(state, crtc, encoder);
}

/**
 * intel_dpll_get_freq - calculate the DPLL's output frequency
 * @i915: i915 device
 * @pll: DPLL for which to calculate the output frequency
 * @pll_state: DPLL state from which to calculate the output frequency
 *
 * Return the output frequency corresponding to @pll's passed in @pll_state.
 */
int intel_dpll_get_freq(struct drm_i915_private *i915,
			const struct intel_shared_dpll *pll,
			const struct intel_dpll_hw_state *pll_state)
{
	if (drm_WARN_ON(&i915->drm, !pll->info->funcs->get_freq))
		return 0;

	return pll->info->funcs->get_freq(i915, pll, pll_state);
}

/**
 * intel_dpll_get_hw_state - readout the DPLL's hardware state
 * @i915: i915 device
 * @pll: DPLL for which to calculate the output frequency
 * @hw_state: DPLL's hardware state
 *
 * Read out @pll's hardware state into @hw_state.
 */
bool intel_dpll_get_hw_state(struct drm_i915_private *i915,
			     struct intel_shared_dpll *pll,
			     struct intel_dpll_hw_state *hw_state)
{
	return pll->info->funcs->get_hw_state(i915, pll, hw_state);
}

static void readout_dpll_hw_state(struct drm_i915_private *i915,
				  struct intel_shared_dpll *pll)
{
	struct intel_crtc *crtc;

	pll->on = intel_dpll_get_hw_state(i915, pll, &pll->state.hw_state);

	pll->state.pipe_mask = 0;
	for_each_intel_crtc(&i915->drm, crtc) {
		struct intel_crtc_state *crtc_state =
			to_intel_crtc_state(crtc->base.state);

		if (crtc_state->hw.active && crtc_state->shared_dpll == pll)
			pll->state.pipe_mask |= BIT(crtc->pipe);
	}
	pll->active_mask = pll->state.pipe_mask;

	drm_dbg_kms(&i915->drm,
		    "%s hw state readout: pipe_mask 0x%x, on %i\n",
		    pll->info->name, pll->state.pipe_mask, pll->on);
}

void intel_dpll_update_ref_clks(struct drm_i915_private *i915)
{
	if (i915->dpll.mgr && i915->dpll.mgr->update_ref_clks)
		i915->dpll.mgr->update_ref_clks(i915);
}

void intel_dpll_readout_hw_state(struct drm_i915_private *i915)
{
	int i;

	for (i = 0; i < i915->dpll.num_shared_dpll; i++)
		readout_dpll_hw_state(i915, &i915->dpll.shared_dplls[i]);
}

static void sanitize_dpll_state(struct drm_i915_private *i915,
				struct intel_shared_dpll *pll)
{
	if (!pll->on)
		return;

	adlp_cmtg_clock_gating_wa(i915, pll);

	if (pll->active_mask)
		return;

	drm_dbg_kms(&i915->drm,
		    "%s enabled but not in use, disabling\n",
		    pll->info->name);

	pll->info->funcs->disable(i915, pll);
	pll->on = false;
}

void intel_dpll_sanitize_state(struct drm_i915_private *i915)
{
	int i;

	for (i = 0; i < i915->dpll.num_shared_dpll; i++)
		sanitize_dpll_state(i915, &i915->dpll.shared_dplls[i]);
}

/**
 * intel_dpll_dump_hw_state - write hw_state to dmesg
 * @dev_priv: i915 drm device
 * @hw_state: hw state to be written to the log
 *
 * Write the relevant values in @hw_state to dmesg using drm_dbg_kms.
 */
void intel_dpll_dump_hw_state(struct drm_i915_private *dev_priv,
			      const struct intel_dpll_hw_state *hw_state)
{
	if (dev_priv->dpll.mgr) {
		dev_priv->dpll.mgr->dump_hw_state(dev_priv, hw_state);
	} else {
		/* fallback for platforms that don't use the shared dpll
		 * infrastructure
		 */
		drm_dbg_kms(&dev_priv->drm,
			    "dpll_hw_state: dpll: 0x%x, dpll_md: 0x%x, "
			    "fp0: 0x%x, fp1: 0x%x\n",
			    hw_state->dpll,
			    hw_state->dpll_md,
			    hw_state->fp0,
			    hw_state->fp1);
	}
}

static void
verify_single_dpll_state(struct drm_i915_private *dev_priv,
			 struct intel_shared_dpll *pll,
			 struct intel_crtc *crtc,
			 struct intel_crtc_state *new_crtc_state)
{
	struct intel_dpll_hw_state dpll_hw_state;
	u8 pipe_mask;
	bool active;

	memset(&dpll_hw_state, 0, sizeof(dpll_hw_state));

	drm_dbg_kms(&dev_priv->drm, "%s\n", pll->info->name);

	active = intel_dpll_get_hw_state(dev_priv, pll, &dpll_hw_state);

	if (!(pll->info->flags & INTEL_DPLL_ALWAYS_ON)) {
		I915_STATE_WARN(!pll->on && pll->active_mask,
				"pll in active use but not on in sw tracking\n");
		I915_STATE_WARN(pll->on && !pll->active_mask,
				"pll is on but not used by any active pipe\n");
		I915_STATE_WARN(pll->on != active,
				"pll on state mismatch (expected %i, found %i)\n",
				pll->on, active);
	}

	if (!crtc) {
		I915_STATE_WARN(pll->active_mask & ~pll->state.pipe_mask,
				"more active pll users than references: 0x%x vs 0x%x\n",
				pll->active_mask, pll->state.pipe_mask);

		return;
	}

	pipe_mask = BIT(crtc->pipe);

	if (new_crtc_state->hw.active)
		I915_STATE_WARN(!(pll->active_mask & pipe_mask),
				"pll active mismatch (expected pipe %c in active mask 0x%x)\n",
				pipe_name(crtc->pipe), pll->active_mask);
	else
		I915_STATE_WARN(pll->active_mask & pipe_mask,
				"pll active mismatch (didn't expect pipe %c in active mask 0x%x)\n",
				pipe_name(crtc->pipe), pll->active_mask);

	I915_STATE_WARN(!(pll->state.pipe_mask & pipe_mask),
			"pll enabled crtcs mismatch (expected 0x%x in 0x%x)\n",
			pipe_mask, pll->state.pipe_mask);

	I915_STATE_WARN(pll->on && memcmp(&pll->state.hw_state,
					  &dpll_hw_state,
					  sizeof(dpll_hw_state)),
			"pll hw state mismatch\n");
}

void intel_shared_dpll_state_verify(struct intel_crtc *crtc,
				    struct intel_crtc_state *old_crtc_state,
				    struct intel_crtc_state *new_crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);

	if (new_crtc_state->shared_dpll)
		verify_single_dpll_state(dev_priv, new_crtc_state->shared_dpll,
					 crtc, new_crtc_state);

	if (old_crtc_state->shared_dpll &&
	    old_crtc_state->shared_dpll != new_crtc_state->shared_dpll) {
		u8 pipe_mask = BIT(crtc->pipe);
		struct intel_shared_dpll *pll = old_crtc_state->shared_dpll;

		I915_STATE_WARN(pll->active_mask & pipe_mask,
				"pll active mismatch (didn't expect pipe %c in active mask (0x%x))\n",
				pipe_name(crtc->pipe), pll->active_mask);
		I915_STATE_WARN(pll->state.pipe_mask & pipe_mask,
				"pll enabled crtcs mismatch (found %x in enabled mask (0x%x))\n",
				pipe_name(crtc->pipe), pll->state.pipe_mask);
	}
}

void intel_shared_dpll_verify_disabled(struct drm_i915_private *i915)
{
	int i;

	for (i = 0; i < i915->dpll.num_shared_dpll; i++)
		verify_single_dpll_state(i915, &i915->dpll.shared_dplls[i],
					 NULL, NULL);
}
