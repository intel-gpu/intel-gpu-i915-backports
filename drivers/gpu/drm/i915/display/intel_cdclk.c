/*
 * Copyright © 2006-2017 Intel Corporation
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

#include <linux/time.h>

#include "intel_atomic.h"
#include "intel_atomic_plane.h"
#include "intel_audio.h"
#include "intel_bw.h"
#include "intel_cdclk.h"
#include "intel_crtc.h"
#include "intel_de.h"
#include "intel_display_types.h"
#include "intel_mchbar_regs.h"
#include "intel_pci_config.h"
#include "intel_pcode.h"
#include "intel_psr.h"

#define MTL_CDCLK_THRESHOLD	307200

#define HAS_SQUASH_AND_CRAWL(i915)	(has_cdclk_squasher(i915) && HAS_CDCLK_CRAWL(i915))

#define MTL_SQUASH_ONLY(i915, cdclk)		((i915)->cdclk.hw.cdclk <= MTL_CDCLK_THRESHOLD && \
						 (cdclk) < MTL_CDCLK_THRESHOLD)
#define MTL_SQUASH_CRAWL(i915, cdclk)		((i915)->cdclk.hw.cdclk < MTL_CDCLK_THRESHOLD && \
						 (cdclk) > MTL_CDCLK_THRESHOLD)
#define MTL_SQUASH_THRESHOLD(i915, cdclk)	((i915)->cdclk.hw.cdclk < MTL_CDCLK_THRESHOLD && \
						 (cdclk) == MTL_CDCLK_THRESHOLD)
#define MTL_CRAWL_THRESHOLD(i915, cdclk)	((i915)->cdclk.hw.cdclk > MTL_CDCLK_THRESHOLD && \
						 (cdclk) == MTL_CDCLK_THRESHOLD)
#define MTL_CRAWL_ONLY(i915, cdclk)		((i915)->cdclk.hw.cdclk > MTL_CDCLK_THRESHOLD && \
						 (cdclk) > MTL_CDCLK_THRESHOLD)
#define MTL_CRAWL_SQUASH(i915, cdclk)		((i915)->cdclk.hw.cdclk > MTL_CDCLK_THRESHOLD && \
						 (cdclk) <= MTL_CDCLK_THRESHOLD)

enum mtl_cdclk_sequence {
	CDCLK_INVALID_ACTION = -1,

	CDCLK_SQUASH_ONLY = 0,
	CDCLK_CRAWL_ONLY,
	CDCLK_SQUASH_THRESHOLD_CRAWL,
	CDCLK_CRAWL_THRESHOLD_SQUASH,
	CDCLK_LEGACY_CHANGE,
	CDCLK_DISABLE
};

/**
 * DOC: CDCLK / RAWCLK
 *
 * The display engine uses several different clocks to do its work. There
 * are two main clocks involved that aren't directly related to the actual
 * pixel clock or any symbol/bit clock of the actual output port. These
 * are the core display clock (CDCLK) and RAWCLK.
 *
 * CDCLK clocks most of the display pipe logic, and thus its frequency
 * must be high enough to support the rate at which pixels are flowing
 * through the pipes. Downscaling must also be accounted as that increases
 * the effective pixel rate.
 *
 * On several platforms the CDCLK frequency can be changed dynamically
 * to minimize power consumption for a given display configuration.
 * Typically changes to the CDCLK frequency require all the display pipes
 * to be shut down while the frequency is being changed.
 *
 * On SKL+ the DMC will toggle the CDCLK off/on during DC5/6 entry/exit.
 * DMC will not change the active CDCLK frequency however, so that part
 * will still be performed by the driver directly.
 *
 * RAWCLK is a fixed frequency clock, often used by various auxiliary
 * blocks such as AUX CH or backlight PWM. Hence the only thing we
 * really need to know about RAWCLK is its frequency so that various
 * dividers can be programmed correctly.
 */

struct intel_cdclk_funcs {
	void (*get_cdclk)(struct drm_i915_private *i915,
			  struct intel_cdclk_config *cdclk_config);
	void (*set_cdclk)(struct drm_i915_private *i915,
			  const struct intel_cdclk_config *cdclk_config,
			  enum pipe pipe);
	int (*modeset_calc_cdclk)(struct intel_cdclk_state *state);
	u8 (*calc_voltage_level)(int cdclk);
};

void intel_cdclk_get_cdclk(struct drm_i915_private *dev_priv,
			   struct intel_cdclk_config *cdclk_config)
{
	dev_priv->cdclk_funcs->get_cdclk(dev_priv, cdclk_config);
}

static void intel_cdclk_set_cdclk(struct drm_i915_private *dev_priv,
				  const struct intel_cdclk_config *cdclk_config,
				  enum pipe pipe)
{
	dev_priv->cdclk_funcs->set_cdclk(dev_priv, cdclk_config, pipe);
}

static int intel_cdclk_modeset_calc_cdclk(struct drm_i915_private *dev_priv,
					  struct intel_cdclk_state *cdclk_config)
{
	return dev_priv->cdclk_funcs->modeset_calc_cdclk(cdclk_config);
}

static u8 intel_cdclk_calc_voltage_level(struct drm_i915_private *dev_priv,
					 int cdclk)
{
	return dev_priv->cdclk_funcs->calc_voltage_level(cdclk);
}

/* convert from kHz to .1 fixpoint MHz with -1MHz offset */
static int skl_cdclk_decimal(int cdclk)
{
	return DIV_ROUND_CLOSEST(cdclk - 1000, 500);
}

static bool has_cdclk_squasher(struct drm_i915_private *i915)
{
	return DISPLAY_VER(i915) >= 14 || IS_DG2(i915);
}

struct intel_cdclk_vals {
	u32 cdclk;
	u16 refclk;
	u16 waveform;
	u8 divider;	/* CD2X divider * 2 */
	u8 ratio;
};

static const struct intel_cdclk_vals icl_cdclk_table[] = {
	{ .refclk = 19200, .cdclk = 172800, .divider = 2, .ratio = 18 },
	{ .refclk = 19200, .cdclk = 192000, .divider = 2, .ratio = 20 },
	{ .refclk = 19200, .cdclk = 307200, .divider = 2, .ratio = 32 },
	{ .refclk = 19200, .cdclk = 326400, .divider = 4, .ratio = 68 },
	{ .refclk = 19200, .cdclk = 556800, .divider = 2, .ratio = 58 },
	{ .refclk = 19200, .cdclk = 652800, .divider = 2, .ratio = 68 },

	{ .refclk = 24000, .cdclk = 180000, .divider = 2, .ratio = 15 },
	{ .refclk = 24000, .cdclk = 192000, .divider = 2, .ratio = 16 },
	{ .refclk = 24000, .cdclk = 312000, .divider = 2, .ratio = 26 },
	{ .refclk = 24000, .cdclk = 324000, .divider = 4, .ratio = 54 },
	{ .refclk = 24000, .cdclk = 552000, .divider = 2, .ratio = 46 },
	{ .refclk = 24000, .cdclk = 648000, .divider = 2, .ratio = 54 },

	{ .refclk = 38400, .cdclk = 172800, .divider = 2, .ratio =  9 },
	{ .refclk = 38400, .cdclk = 192000, .divider = 2, .ratio = 10 },
	{ .refclk = 38400, .cdclk = 307200, .divider = 2, .ratio = 16 },
	{ .refclk = 38400, .cdclk = 326400, .divider = 4, .ratio = 34 },
	{ .refclk = 38400, .cdclk = 556800, .divider = 2, .ratio = 29 },
	{ .refclk = 38400, .cdclk = 652800, .divider = 2, .ratio = 34 },
	{}
};

static const struct intel_cdclk_vals rkl_cdclk_table[] = {
	{ .refclk = 19200, .cdclk = 172800, .divider = 4, .ratio =  36 },
	{ .refclk = 19200, .cdclk = 192000, .divider = 4, .ratio =  40 },
	{ .refclk = 19200, .cdclk = 307200, .divider = 4, .ratio =  64 },
	{ .refclk = 19200, .cdclk = 326400, .divider = 8, .ratio = 136 },
	{ .refclk = 19200, .cdclk = 556800, .divider = 4, .ratio = 116 },
	{ .refclk = 19200, .cdclk = 652800, .divider = 4, .ratio = 136 },

	{ .refclk = 24000, .cdclk = 180000, .divider = 4, .ratio =  30 },
	{ .refclk = 24000, .cdclk = 192000, .divider = 4, .ratio =  32 },
	{ .refclk = 24000, .cdclk = 312000, .divider = 4, .ratio =  52 },
	{ .refclk = 24000, .cdclk = 324000, .divider = 8, .ratio = 108 },
	{ .refclk = 24000, .cdclk = 552000, .divider = 4, .ratio =  92 },
	{ .refclk = 24000, .cdclk = 648000, .divider = 4, .ratio = 108 },

	{ .refclk = 38400, .cdclk = 172800, .divider = 4, .ratio = 18 },
	{ .refclk = 38400, .cdclk = 192000, .divider = 4, .ratio = 20 },
	{ .refclk = 38400, .cdclk = 307200, .divider = 4, .ratio = 32 },
	{ .refclk = 38400, .cdclk = 326400, .divider = 8, .ratio = 68 },
	{ .refclk = 38400, .cdclk = 556800, .divider = 4, .ratio = 58 },
	{ .refclk = 38400, .cdclk = 652800, .divider = 4, .ratio = 68 },
	{}
};

static const struct intel_cdclk_vals adlp_a_step_cdclk_table[] = {
	{ .refclk = 19200, .cdclk = 307200, .divider = 2, .ratio = 32 },
	{ .refclk = 19200, .cdclk = 556800, .divider = 2, .ratio = 58 },
	{ .refclk = 19200, .cdclk = 652800, .divider = 2, .ratio = 68 },

	{ .refclk = 24000, .cdclk = 312000, .divider = 2, .ratio = 26 },
	{ .refclk = 24000, .cdclk = 552000, .divider = 2, .ratio = 46 },
	{ .refclk = 24400, .cdclk = 648000, .divider = 2, .ratio = 54 },

	{ .refclk = 38400, .cdclk = 307200, .divider = 2, .ratio = 16 },
	{ .refclk = 38400, .cdclk = 556800, .divider = 2, .ratio = 29 },
	{ .refclk = 38400, .cdclk = 652800, .divider = 2, .ratio = 34 },
	{}
};

static const struct intel_cdclk_vals adlp_cdclk_table[] = {
	{ .refclk = 19200, .cdclk = 172800, .divider = 3, .ratio = 27 },
	{ .refclk = 19200, .cdclk = 192000, .divider = 2, .ratio = 20 },
	{ .refclk = 19200, .cdclk = 307200, .divider = 2, .ratio = 32 },
	{ .refclk = 19200, .cdclk = 556800, .divider = 2, .ratio = 58 },
	{ .refclk = 19200, .cdclk = 652800, .divider = 2, .ratio = 68 },

	{ .refclk = 24000, .cdclk = 176000, .divider = 3, .ratio = 22 },
	{ .refclk = 24000, .cdclk = 192000, .divider = 2, .ratio = 16 },
	{ .refclk = 24000, .cdclk = 312000, .divider = 2, .ratio = 26 },
	{ .refclk = 24000, .cdclk = 552000, .divider = 2, .ratio = 46 },
	{ .refclk = 24400, .cdclk = 648000, .divider = 2, .ratio = 54 },

	{ .refclk = 38400, .cdclk = 179200, .divider = 3, .ratio = 14 },
	{ .refclk = 38400, .cdclk = 192000, .divider = 2, .ratio = 10 },
	{ .refclk = 38400, .cdclk = 307200, .divider = 2, .ratio = 16 },
	{ .refclk = 38400, .cdclk = 556800, .divider = 2, .ratio = 29 },
	{ .refclk = 38400, .cdclk = 652800, .divider = 2, .ratio = 34 },
	{}
};

static const struct intel_cdclk_vals dg2_cdclk_table[] = {
	{ .refclk = 38400, .cdclk = 163200, .divider = 2, .ratio = 34, .waveform = 0x8888 },
	{ .refclk = 38400, .cdclk = 204000, .divider = 2, .ratio = 34, .waveform = 0x9248 },
	{ .refclk = 38400, .cdclk = 244800, .divider = 2, .ratio = 34, .waveform = 0xa4a4 },
	{ .refclk = 38400, .cdclk = 285600, .divider = 2, .ratio = 34, .waveform = 0xa54a },
	{ .refclk = 38400, .cdclk = 326400, .divider = 2, .ratio = 34, .waveform = 0xaaaa },
	{ .refclk = 38400, .cdclk = 367200, .divider = 2, .ratio = 34, .waveform = 0xad5a },
	{ .refclk = 38400, .cdclk = 408000, .divider = 2, .ratio = 34, .waveform = 0xb6b6 },
	{ .refclk = 38400, .cdclk = 448800, .divider = 2, .ratio = 34, .waveform = 0xdbb6 },
	{ .refclk = 38400, .cdclk = 489600, .divider = 2, .ratio = 34, .waveform = 0xeeee },
	{ .refclk = 38400, .cdclk = 530400, .divider = 2, .ratio = 34, .waveform = 0xf7de },
	{ .refclk = 38400, .cdclk = 571200, .divider = 2, .ratio = 34, .waveform = 0xfefe },
	{ .refclk = 38400, .cdclk = 612000, .divider = 2, .ratio = 34, .waveform = 0xfffe },
	{ .refclk = 38400, .cdclk = 652800, .divider = 2, .ratio = 34, .waveform = 0xffff },
	{}
};

static const struct intel_cdclk_vals mtl_cdclk_table[] = {
	{ .refclk = 38400, .cdclk = 172800, .divider = 2, .ratio = 16, .waveform = 0xad5a },
	{ .refclk = 38400, .cdclk = 192000, .divider = 2, .ratio = 16, .waveform = 0xb6b6 },
	{ .refclk = 38400, .cdclk = 307200, .divider = 2, .ratio = 16, .waveform = 0x0000 },
	{ .refclk = 38400, .cdclk = 480000, .divider = 2, .ratio = 25, .waveform = 0x0000 },
	{ .refclk = 38400, .cdclk = 556800, .divider = 2, .ratio = 29, .waveform = 0x0000 },
	{ .refclk = 38400, .cdclk = 652800, .divider = 2, .ratio = 34, .waveform = 0x0000 },
	{}
};

static int bxt_calc_cdclk(struct drm_i915_private *dev_priv, int min_cdclk)
{
	const struct intel_cdclk_vals *table = dev_priv->cdclk.table;
	int i;

	for (i = 0; table[i].refclk; i++)
		if (table[i].refclk == dev_priv->cdclk.hw.ref &&
		    table[i].cdclk >= min_cdclk)
			return table[i].cdclk;

	drm_WARN(&dev_priv->drm, 1,
		 "Cannot satisfy minimum cdclk %d with refclk %u\n",
		 min_cdclk, dev_priv->cdclk.hw.ref);
	return table[0].cdclk;
}

static int bxt_calc_cdclk_pll_vco(struct drm_i915_private *dev_priv, int cdclk)
{
	const struct intel_cdclk_vals *table = dev_priv->cdclk.table;
	int i;

	if (cdclk == dev_priv->cdclk.hw.bypass)
		return 0;

	for (i = 0; table[i].refclk; i++)
		if (table[i].refclk == dev_priv->cdclk.hw.ref &&
		    table[i].cdclk == cdclk)
			return dev_priv->cdclk.hw.ref * table[i].ratio;

	drm_WARN(&dev_priv->drm, 1, "cdclk %d not valid for refclk %u\n",
		 cdclk, dev_priv->cdclk.hw.ref);
	return 0;
}

static u8 tgl_calc_voltage_level(int cdclk)
{
	if (cdclk > 556800)
		return 3;
	else if (cdclk > 326400)
		return 2;
	else if (cdclk > 312000)
		return 1;
	else
		return 0;
}

static void icl_readout_refclk(struct drm_i915_private *dev_priv,
			       struct intel_cdclk_config *cdclk_config)
{
	u32 dssm = intel_de_read(dev_priv, SKL_DSSM) & ICL_DSSM_CDCLK_PLL_REFCLK_MASK;

	switch (dssm) {
	default:
		MISSING_CASE(dssm);
		fallthrough;
	case ICL_DSSM_CDCLK_PLL_REFCLK_24MHz:
		cdclk_config->ref = 24000;
		break;
	case ICL_DSSM_CDCLK_PLL_REFCLK_19_2MHz:
		cdclk_config->ref = 19200;
		break;
	case ICL_DSSM_CDCLK_PLL_REFCLK_38_4MHz:
		cdclk_config->ref = 38400;
		break;
	}
}

static void bxt_de_pll_readout(struct drm_i915_private *dev_priv,
			       struct intel_cdclk_config *cdclk_config)
{
	u32 val, ratio;

	if (DISPLAY_VER(dev_priv) >= 14)
		cdclk_config->ref = 38400;
	else if (IS_DG2(dev_priv))
		cdclk_config->ref = 38400;
	else
		icl_readout_refclk(dev_priv, cdclk_config);

	val = intel_de_read(dev_priv, BXT_DE_PLL_ENABLE);
	if ((val & BXT_DE_PLL_PLL_ENABLE) == 0 ||
	    (val & BXT_DE_PLL_LOCK) == 0) {
		/*
		 * CDCLK PLL is disabled, the VCO/ratio doesn't matter, but
		 * setting it to zero is a way to signal that.
		 */
		cdclk_config->vco = 0;
		return;
	}

	/*
	 * DISPLAY_VER >= 11 have the ratio directly in the PLL enable register,
	 * gen9lp had it in a separate PLL control register.
	 */
	ratio = val & ICL_CDCLK_PLL_RATIO_MASK;
	cdclk_config->vco = ratio * cdclk_config->ref;
}

static void mtl_get_cdclk(struct drm_i915_private *dev_priv,
			  struct intel_cdclk_config *cdclk_config)
{
	const struct intel_cdclk_vals *table = dev_priv->cdclk.table;
	u32 squash_ctl, divider, waveform;
	int div, i, ratio;

	bxt_de_pll_readout(dev_priv, cdclk_config);

	cdclk_config->bypass = cdclk_config->ref / 2;

	if (cdclk_config->vco == 0) {
		cdclk_config->cdclk = cdclk_config->bypass;
		goto out;
	}

	divider = intel_de_read(dev_priv, CDCLK_CTL) & BXT_CDCLK_CD2X_DIV_SEL_MASK;
	switch (divider) {
	case BXT_CDCLK_CD2X_DIV_SEL_1:
		div = 2;
		break;
	case BXT_CDCLK_CD2X_DIV_SEL_1_5:
		div = 3;
		break;
	case BXT_CDCLK_CD2X_DIV_SEL_2:
		div = 4;
		break;
	case BXT_CDCLK_CD2X_DIV_SEL_4:
		div = 8;
		break;
	default:
		MISSING_CASE(divider);
		return;
	}

	squash_ctl = intel_de_read(dev_priv, CDCLK_SQUASH_CTL);
	if (squash_ctl & CDCLK_SQUASH_ENABLE)
		waveform = squash_ctl & CDCLK_SQUASH_WAVEFORM_MASK;
	else
		waveform = 0;

	ratio = cdclk_config->vco / cdclk_config->ref;

	for (i = 0, cdclk_config->cdclk = 0; table[i].refclk; i++) {
		if (table[i].refclk != cdclk_config->ref)
			continue;

		if (table[i].divider != div)
			continue;

		if (table[i].waveform != waveform)
			continue;

		if (table[i].ratio != ratio)
			continue;

		cdclk_config->cdclk = table[i].cdclk;
		break;
	}

out:
	/*
	 * Can't read this out :( Let's assume it's
	 * at least what the CDCLK frequency requires.
	 */
	cdclk_config->voltage_level =
		intel_cdclk_calc_voltage_level(dev_priv, cdclk_config->cdclk);
}

static void bxt_get_cdclk(struct drm_i915_private *dev_priv,
			  struct intel_cdclk_config *cdclk_config)
{
	u32 squash_ctl = 0;
	u32 divider;
	int div;

	bxt_de_pll_readout(dev_priv, cdclk_config);

	cdclk_config->bypass = cdclk_config->ref / 2;

	if (cdclk_config->vco == 0) {
		cdclk_config->cdclk = cdclk_config->bypass;
		goto out;
	}

	divider = intel_de_read(dev_priv, CDCLK_CTL) & BXT_CDCLK_CD2X_DIV_SEL_MASK;

	switch (divider) {
	case BXT_CDCLK_CD2X_DIV_SEL_1:
		div = 2;
		break;
	case BXT_CDCLK_CD2X_DIV_SEL_1_5:
		div = 3;
		break;
	case BXT_CDCLK_CD2X_DIV_SEL_2:
		div = 4;
		break;
	case BXT_CDCLK_CD2X_DIV_SEL_4:
		div = 8;
		break;
	default:
		MISSING_CASE(divider);
		return;
	}

	if (has_cdclk_squasher(dev_priv))
		squash_ctl = intel_de_read(dev_priv, CDCLK_SQUASH_CTL);

	if (squash_ctl & CDCLK_SQUASH_ENABLE) {
		u16 waveform;
		int size;

		size = REG_FIELD_GET(CDCLK_SQUASH_WINDOW_SIZE_MASK, squash_ctl) + 1;
		waveform = REG_FIELD_GET(CDCLK_SQUASH_WAVEFORM_MASK, squash_ctl) >> (16 - size);

		cdclk_config->cdclk = DIV_ROUND_CLOSEST(hweight16(waveform) *
							cdclk_config->vco, size * div);
	} else {
		cdclk_config->cdclk = DIV_ROUND_CLOSEST(cdclk_config->vco, div);
	}

 out:
	/*
	 * Can't read this out :( Let's assume it's
	 * at least what the CDCLK frequency requires.
	 */
	cdclk_config->voltage_level =
		intel_cdclk_calc_voltage_level(dev_priv, cdclk_config->cdclk);
}

static void icl_cdclk_pll_disable(struct drm_i915_private *dev_priv)
{
	intel_de_rmw(dev_priv, BXT_DE_PLL_ENABLE,
		     BXT_DE_PLL_PLL_ENABLE, 0);

	/* Timeout 200us */
	if (intel_de_wait_for_clear(dev_priv, BXT_DE_PLL_ENABLE, BXT_DE_PLL_LOCK, 1))
		drm_err(&dev_priv->drm, "timeout waiting for CDCLK PLL unlock\n");

	dev_priv->cdclk.hw.vco = 0;
}

static void icl_cdclk_pll_enable(struct drm_i915_private *dev_priv, int vco)
{
	int ratio = DIV_ROUND_CLOSEST(vco, dev_priv->cdclk.hw.ref);
	u32 val;

	val = ICL_CDCLK_PLL_RATIO(ratio);
	intel_de_write(dev_priv, BXT_DE_PLL_ENABLE, val);

	val |= BXT_DE_PLL_PLL_ENABLE;
	intel_de_write(dev_priv, BXT_DE_PLL_ENABLE, val);

	/* Timeout 200us */
	if (intel_de_wait_for_set(dev_priv, BXT_DE_PLL_ENABLE, BXT_DE_PLL_LOCK, 1))
		drm_err(&dev_priv->drm, "timeout waiting for CDCLK PLL lock\n");

	dev_priv->cdclk.hw.vco = vco;
}

static void adlp_cdclk_pll_crawl(struct drm_i915_private *dev_priv, int vco)
{
	int ratio = DIV_ROUND_CLOSEST(vco, dev_priv->cdclk.hw.ref);
	u32 val;

	/* Write PLL ratio without disabling */
	val = ICL_CDCLK_PLL_RATIO(ratio) | BXT_DE_PLL_PLL_ENABLE;
	intel_de_write(dev_priv, BXT_DE_PLL_ENABLE, val);

	/* Submit freq change request */
	val |= BXT_DE_PLL_FREQ_REQ;
	intel_de_write(dev_priv, BXT_DE_PLL_ENABLE, val);

	/* Timeout 200us */
	if (intel_de_wait_for_set(dev_priv, BXT_DE_PLL_ENABLE,
				  BXT_DE_PLL_LOCK | BXT_DE_PLL_FREQ_REQ_ACK, 1))
		drm_err(&dev_priv->drm, "timeout waiting for FREQ change request ack\n");

	val &= ~BXT_DE_PLL_FREQ_REQ;
	intel_de_write(dev_priv, BXT_DE_PLL_ENABLE, val);

	dev_priv->cdclk.hw.vco = vco;
}

static u32 bxt_cdclk_cd2x_pipe(struct drm_i915_private *dev_priv, enum pipe pipe)
{
	if (pipe == INVALID_PIPE)
		return TGL_CDCLK_CD2X_PIPE_NONE;
	else
		return TGL_CDCLK_CD2X_PIPE(pipe);
}

static u32 bxt_cdclk_cd2x_div_sel(struct drm_i915_private *dev_priv,
				  int cdclk, int vco)
{
	/* cdclk = vco / 2 / div{1,1.5,2,4} */
	switch (DIV_ROUND_CLOSEST(vco, cdclk)) {
	default:
		drm_WARN_ON(&dev_priv->drm,
			    cdclk != dev_priv->cdclk.hw.bypass);
		drm_WARN_ON(&dev_priv->drm, vco != 0);
		fallthrough;
	case 2:
		return BXT_CDCLK_CD2X_DIV_SEL_1;
	case 3:
		return BXT_CDCLK_CD2X_DIV_SEL_1_5;
	case 4:
		return BXT_CDCLK_CD2X_DIV_SEL_2;
	case 8:
		return BXT_CDCLK_CD2X_DIV_SEL_4;
	}
}

static u32 cdclk_squash_waveform(struct drm_i915_private *dev_priv,
				 int cdclk)
{
	const struct intel_cdclk_vals *table = dev_priv->cdclk.table;
	int i;

	if (cdclk == dev_priv->cdclk.hw.bypass)
		return 0;

	for (i = 0; table[i].refclk; i++)
		if (table[i].refclk == dev_priv->cdclk.hw.ref &&
		    table[i].cdclk == cdclk)
			return table[i].waveform;

	drm_WARN(&dev_priv->drm, 1, "cdclk %d not valid for refclk %u\n",
		 cdclk, dev_priv->cdclk.hw.ref);

	return 0xffff;
}

static enum mtl_cdclk_sequence
mtl_determine_cdclk_sequence(struct drm_i915_private *i915,
			     const struct intel_cdclk_config *cdclk_config,
			     int cdclk)
{
	if (cdclk_config->vco == 0 || cdclk_config->vco == ~0) {
		return CDCLK_DISABLE;
	} else if ((i915)->cdclk.hw.cdclk == 0) {
		return CDCLK_LEGACY_CHANGE;
	} else if (MTL_CRAWL_ONLY(i915, cdclk) || MTL_CRAWL_THRESHOLD(i915, cdclk)) {
		return CDCLK_CRAWL_ONLY;
	} else if (MTL_SQUASH_ONLY(i915, cdclk) ||
		   MTL_SQUASH_THRESHOLD(i915, cdclk)) {
		return CDCLK_SQUASH_ONLY;
	} else if (MTL_SQUASH_CRAWL(i915, cdclk)) {
		return CDCLK_SQUASH_THRESHOLD_CRAWL;
	} else if (MTL_CRAWL_SQUASH(i915, cdclk)) {
		return CDCLK_CRAWL_THRESHOLD_SQUASH;
	}

	drm_err(&i915->drm, "Not a valid cdclk sequence of actions\n");
	return CDCLK_INVALID_ACTION;
}

static void dg2_prog_squash_ctl(struct drm_i915_private *i915, u16 waveform)
{
	u32 squash_ctl = 0;

	if (waveform) {
		squash_ctl |= CDCLK_SQUASH_ENABLE;
		squash_ctl |= CDCLK_SQUASH_WINDOW_SIZE(0xf) | waveform;
	}

	intel_de_write(i915, CDCLK_SQUASH_CTL, squash_ctl);
}

static const char *cdclk_sequence_to_string(enum mtl_cdclk_sequence
					    mtl_cdclk_sequence)
{
	switch (mtl_cdclk_sequence) {
	case CDCLK_SQUASH_ONLY:
		return "Squash only";
	case CDCLK_CRAWL_ONLY:
		return "Crawl only";
	case CDCLK_SQUASH_THRESHOLD_CRAWL:
		return "Squash to threshold, followed by Crawl";
	case CDCLK_CRAWL_THRESHOLD_SQUASH:
		return "Crawl to threshold, followed by Squash";
	case CDCLK_LEGACY_CHANGE:
		return "Legacy method";
	case CDCLK_DISABLE:
		return "Disable CDCLK";
	default:
		return "Not a valid cdclk sequence";
	}
}

static void mtl_set_cdclk(struct drm_i915_private *i915,
			  const struct intel_cdclk_config *cdclk_config,
			  enum pipe pipe)
{
	enum mtl_cdclk_sequence mtl_cdclk_sequence;
	int cdclk = cdclk_config->cdclk;
	int vco = cdclk_config->vco;
	int squash_crawl_vco;
	u32 val;
	u16 waveform;

	mtl_cdclk_sequence = mtl_determine_cdclk_sequence(i915, cdclk_config, cdclk);
	if (mtl_cdclk_sequence == CDCLK_INVALID_ACTION)
		return;

	/*
	 * MTL supports CDCLK flow with squashing and crawling.
	 * - If current CDCLK and required CDCLK both are greater
	 * than threshold(307200), crawl
	 * - If we need to transition from CDCLK higher than threshold
	 * to a frequency less than threshold, crawl till the threshold
	 * and then squash to desired CDCLK.
	 */

	drm_dbg_kms(&i915->drm, "CDCLK changing from %i to %i using %s\n",
		    i915->cdclk.hw.cdclk,
		    cdclk, cdclk_sequence_to_string(mtl_cdclk_sequence));

	switch (mtl_cdclk_sequence) {
	case CDCLK_SQUASH_ONLY:
		waveform = cdclk_squash_waveform(i915, cdclk);
		dg2_prog_squash_ctl(i915, waveform);
		break;
	case CDCLK_CRAWL_ONLY:
		adlp_cdclk_pll_crawl(i915, vco);
		break;
	case CDCLK_SQUASH_THRESHOLD_CRAWL:
		waveform = cdclk_squash_waveform(i915, MTL_CDCLK_THRESHOLD);
		dg2_prog_squash_ctl(i915, waveform);
		adlp_cdclk_pll_crawl(i915, vco);
		break;
	case CDCLK_CRAWL_THRESHOLD_SQUASH:
		squash_crawl_vco = bxt_calc_cdclk_pll_vco(i915, MTL_CDCLK_THRESHOLD);
		adlp_cdclk_pll_crawl(i915, squash_crawl_vco);
		waveform = cdclk_squash_waveform(i915, cdclk);
		dg2_prog_squash_ctl(i915, waveform);
		break;
	case CDCLK_LEGACY_CHANGE:
		icl_cdclk_pll_disable(i915);
		icl_cdclk_pll_enable(i915, vco);

		waveform = cdclk_squash_waveform(i915, cdclk);
		dg2_prog_squash_ctl(i915, waveform);
		break;
	case CDCLK_DISABLE:
		icl_cdclk_pll_disable(i915);
		break;
	default:
		drm_err(&i915->drm, "Invalid CDCLK sequence requested");
		return;
	}

	val = BXT_CDCLK_CD2X_DIV_SEL_1 |
		bxt_cdclk_cd2x_pipe(i915, pipe) |
		skl_cdclk_decimal(cdclk);

	intel_de_write(i915, CDCLK_CTL, val);

	if (pipe != INVALID_PIPE)
		intel_crtc_wait_for_next_vblank(intel_crtc_for_pipe(i915, pipe));

	intel_update_cdclk(i915);

	i915->cdclk.hw.voltage_level = cdclk_config->voltage_level;
}

static void bxt_set_cdclk(struct drm_i915_private *dev_priv,
			  const struct intel_cdclk_config *cdclk_config,
			  enum pipe pipe)
{
	int cdclk = cdclk_config->cdclk;
	int vco = cdclk_config->vco;
	u32 val;
	u16 waveform;
	int clock;
	int ret;

	/* Inform power controller of upcoming frequency change. */
	ret = skl_pcode_request(&dev_priv->uncore, SKL_PCODE_CDCLK_CONTROL,
				SKL_CDCLK_PREPARE_FOR_CHANGE,
				SKL_CDCLK_READY_FOR_CHANGE,
				SKL_CDCLK_READY_FOR_CHANGE, 3);
	if (ret) {
		drm_err(&dev_priv->drm,
			"Failed to inform PCU about cdclk change (err %d, freq %d)\n",
			ret, cdclk);
		return;
	}

	if (HAS_CDCLK_CRAWL(dev_priv) && dev_priv->cdclk.hw.vco > 0 && vco > 0) {
		if (dev_priv->cdclk.hw.vco != vco)
			adlp_cdclk_pll_crawl(dev_priv, vco);
	} else {
		if (dev_priv->cdclk.hw.vco != 0 &&
		    dev_priv->cdclk.hw.vco != vco)
			icl_cdclk_pll_disable(dev_priv);

		if (dev_priv->cdclk.hw.vco != vco)
			icl_cdclk_pll_enable(dev_priv, vco);
	}

	waveform = cdclk_squash_waveform(dev_priv, cdclk);

	if (waveform)
		clock = vco / 2;
	else
		clock = cdclk;

	if (has_cdclk_squasher(dev_priv))
		dg2_prog_squash_ctl(dev_priv, waveform);

	val = bxt_cdclk_cd2x_div_sel(dev_priv, clock, vco) |
		bxt_cdclk_cd2x_pipe(dev_priv, pipe) |
		skl_cdclk_decimal(cdclk);

	intel_de_write(dev_priv, CDCLK_CTL, val);

	if (pipe != INVALID_PIPE)
		intel_crtc_wait_for_next_vblank(intel_crtc_for_pipe(dev_priv, pipe));

	ret = snb_pcode_write_timeout(&dev_priv->uncore, SKL_PCODE_CDCLK_CONTROL,
				      cdclk_config->voltage_level, 500, 20);
	if (ret) {
		drm_err(&dev_priv->drm,
			"PCode CDCLK freq set failed, (err %d, freq %d)\n",
			ret, cdclk);
		return;
	}

	intel_update_cdclk(dev_priv);

	/*
	 * Can't read out the voltage level :(
	 * Let's just assume everything is as expected.
	 */
	dev_priv->cdclk.hw.voltage_level = cdclk_config->voltage_level;
}

static void bxt_sanitize_cdclk(struct drm_i915_private *dev_priv)
{
	u32 cdctl, expected;
	int cdclk, clock, vco;

	intel_update_cdclk(dev_priv);
	intel_cdclk_dump_config(dev_priv, &dev_priv->cdclk.hw, "Current CDCLK");

	if (dev_priv->cdclk.hw.vco == 0 ||
	    dev_priv->cdclk.hw.cdclk == dev_priv->cdclk.hw.bypass)
		goto sanitize;

	/* DPLL okay; verify the cdclock
	 *
	 * Some BIOS versions leave an incorrect decimal frequency value and
	 * set reserved MBZ bits in CDCLK_CTL at least during exiting from S4,
	 * so sanitize this register.
	 */
	cdctl = intel_de_read(dev_priv, CDCLK_CTL);
	/*
	 * Let's ignore the pipe field, since BIOS could have configured the
	 * dividers both synching to an active pipe, or asynchronously
	 * (PIPE_NONE).
	 */
	cdctl &= ~bxt_cdclk_cd2x_pipe(dev_priv, INVALID_PIPE);

	/* Make sure this is a legal cdclk value for the platform */
	cdclk = bxt_calc_cdclk(dev_priv, dev_priv->cdclk.hw.cdclk);
	if (cdclk != dev_priv->cdclk.hw.cdclk)
		goto sanitize;

	/* Make sure the VCO is correct for the cdclk */
	vco = bxt_calc_cdclk_pll_vco(dev_priv, cdclk);
	if (vco != dev_priv->cdclk.hw.vco)
		goto sanitize;

	expected = skl_cdclk_decimal(cdclk);

	/* Figure out what CD2X divider we should be using for this cdclk */
	if (has_cdclk_squasher(dev_priv))
		clock = dev_priv->cdclk.hw.vco / 2;
	else
		clock = dev_priv->cdclk.hw.cdclk;

	expected |= bxt_cdclk_cd2x_div_sel(dev_priv, clock,
					   dev_priv->cdclk.hw.vco);

	if (cdctl == expected)
		/* All well; nothing to sanitize */
		return;

sanitize:
	drm_dbg_kms(&dev_priv->drm, "Sanitizing cdclk programmed by pre-os\n");

	/* force cdclk programming */
	dev_priv->cdclk.hw.cdclk = 0;

	/* force full PLL disable + enable */
	dev_priv->cdclk.hw.vco = -1;
}

static void bxt_cdclk_init_hw(struct drm_i915_private *dev_priv)
{
	struct intel_cdclk_config cdclk_config;

	bxt_sanitize_cdclk(dev_priv);

	if (dev_priv->cdclk.hw.cdclk != 0 &&
	    dev_priv->cdclk.hw.vco != 0)
		return;

	cdclk_config = dev_priv->cdclk.hw;

	/*
	 * FIXME:
	 * - The initial CDCLK needs to be read from VBT.
	 *   Need to make this change after VBT has changes for BXT.
	 */
	cdclk_config.cdclk = bxt_calc_cdclk(dev_priv, 0);
	cdclk_config.vco = bxt_calc_cdclk_pll_vco(dev_priv, cdclk_config.cdclk);
	cdclk_config.voltage_level =
		intel_cdclk_calc_voltage_level(dev_priv, cdclk_config.cdclk);

	if (DISPLAY_VER(dev_priv) >= 14)
		mtl_set_cdclk(dev_priv, &cdclk_config, INVALID_PIPE);
	else
		bxt_set_cdclk(dev_priv, &cdclk_config, INVALID_PIPE);
}

static void bxt_cdclk_uninit_hw(struct drm_i915_private *dev_priv)
{
	struct intel_cdclk_config cdclk_config = dev_priv->cdclk.hw;

	cdclk_config.cdclk = cdclk_config.bypass;
	cdclk_config.vco = 0;
	cdclk_config.voltage_level =
		intel_cdclk_calc_voltage_level(dev_priv, cdclk_config.cdclk);

	if (DISPLAY_VER(dev_priv) >= 14)
		mtl_set_cdclk(dev_priv, &cdclk_config, INVALID_PIPE);
	else
		bxt_set_cdclk(dev_priv, &cdclk_config, INVALID_PIPE);
}

/**
 * intel_cdclk_init_hw - Initialize CDCLK hardware
 * @i915: i915 device
 *
 * Initialize CDCLK. This consists mainly of initializing dev_priv->cdclk.hw and
 * sanitizing the state of the hardware if needed. This is generally done only
 * during the display core initialization sequence, after which the DMC will
 * take care of turning CDCLK off/on as needed.
 */
void intel_cdclk_init_hw(struct drm_i915_private *i915)
{
	bxt_cdclk_init_hw(i915);
}

/**
 * intel_cdclk_uninit_hw - Uninitialize CDCLK hardware
 * @i915: i915 device
 *
 * Uninitialize CDCLK. This is done only during the display core
 * uninitialization sequence.
 */
void intel_cdclk_uninit_hw(struct drm_i915_private *i915)
{
	bxt_cdclk_uninit_hw(i915);
}

static bool intel_cdclk_can_crawl(struct drm_i915_private *dev_priv,
				  const struct intel_cdclk_config *a,
				  const struct intel_cdclk_config *b)
{
	int a_div, b_div;

	if (!HAS_CDCLK_CRAWL(dev_priv))
		return false;

	/*
	 * The vco and cd2x divider will change independently
	 * from each, so we disallow cd2x change when crawling.
	 */
	a_div = DIV_ROUND_CLOSEST(a->vco, a->cdclk);
	b_div = DIV_ROUND_CLOSEST(b->vco, b->cdclk);

	return a->vco != 0 && b->vco != 0 &&
		a->vco != b->vco &&
		a_div == b_div &&
		a->ref == b->ref;
}

static bool intel_cdclk_can_squash(struct drm_i915_private *dev_priv,
				   const struct intel_cdclk_config *a,
				   const struct intel_cdclk_config *b)
{
	/*
	 * FIXME should store a bit more state in intel_cdclk_config
	 * to differentiate squasher vs. cd2x divider properly. For
	 * the moment all platforms with squasher use a fixed cd2x
	 * divider.
	 */
	if (!has_cdclk_squasher(dev_priv))
		return false;

	return a->cdclk != b->cdclk &&
		a->vco != 0 &&
		a->vco == b->vco &&
		a->ref == b->ref;
}

/*
 * MTL Introduces cases where squashing and crawling alone can't satisfy
 *  we will still have to use a combination of both squashing+crawling
 * to achieve the overall transition without triggering
 * a full modeset.
 */
static bool intel_cdclk_can_squash_and_crawl(struct drm_i915_private *dev_priv,
					     const struct intel_cdclk_config *old,
					     struct intel_cdclk_config *new)
{
	if (!HAS_SQUASH_AND_CRAWL(dev_priv))
		return false;

	if (old->cdclk == 0)
		return false;

	if (old->cdclk > MTL_CDCLK_THRESHOLD && new->cdclk <= MTL_CDCLK_THRESHOLD)
		return true;

	/*
	 * - For transitioning from a CDCLK less than the threshold(307200) to one
	 * that is higher than threshold, squash till threshold and then crawl to
	 * the desired frequency
	 */
	if (old->cdclk < MTL_CDCLK_THRESHOLD && new->cdclk > MTL_CDCLK_THRESHOLD)
		return true;

	return old->cdclk != new->cdclk &&
		old->vco != 0 && new->vco != 0;
}

/**
 * intel_cdclk_needs_modeset - Determine if changong between the CDCLK
 *                             configurations requires a modeset on all pipes
 * @a: first CDCLK configuration
 * @b: second CDCLK configuration
 *
 * Returns:
 * True if changing between the two CDCLK configurations
 * requires all pipes to be off, false if not.
 */
bool intel_cdclk_needs_modeset(const struct intel_cdclk_config *a,
			       const struct intel_cdclk_config *b)
{
	return a->cdclk != b->cdclk ||
		a->vco != b->vco ||
		a->ref != b->ref;
}

/**
 * intel_cdclk_can_cd2x_update - Determine if changing between the two CDCLK
 *                               configurations requires only a cd2x divider update
 * @dev_priv: i915 device
 * @a: first CDCLK configuration
 * @b: second CDCLK configuration
 *
 * Returns:
 * True if changing between the two CDCLK configurations
 * can be done with just a cd2x divider update, false if not.
 */
static bool intel_cdclk_can_cd2x_update(struct drm_i915_private *dev_priv,
					const struct intel_cdclk_config *a,
					const struct intel_cdclk_config *b)
{
	/*
	 * FIXME should store a bit more state in intel_cdclk_config
	 * to differentiate squasher vs. cd2x divider properly. For
	 * the moment all platforms with squasher use a fixed cd2x
	 * divider.
	 */
	if (has_cdclk_squasher(dev_priv))
		return false;

	return a->cdclk != b->cdclk &&
		a->vco != 0 &&
		a->vco == b->vco &&
		a->ref == b->ref;
}

/**
 * intel_cdclk_changed - Determine if two CDCLK configurations are different
 * @a: first CDCLK configuration
 * @b: second CDCLK configuration
 *
 * Returns:
 * True if the CDCLK configurations don't match, false if they do.
 */
static bool intel_cdclk_changed(const struct intel_cdclk_config *a,
				const struct intel_cdclk_config *b)
{
	return intel_cdclk_needs_modeset(a, b) ||
		a->voltage_level != b->voltage_level;
}

void intel_cdclk_dump_config(struct drm_i915_private *i915,
			     const struct intel_cdclk_config *cdclk_config,
			     const char *context)
{
	drm_dbg_kms(&i915->drm, "%s %d kHz, VCO %d kHz, ref %d kHz, bypass %d kHz, voltage level %d\n",
		    context, cdclk_config->cdclk, cdclk_config->vco,
		    cdclk_config->ref, cdclk_config->bypass,
		    cdclk_config->voltage_level);
}

/**
 * intel_set_cdclk - Push the CDCLK configuration to the hardware
 * @dev_priv: i915 device
 * @cdclk_config: new CDCLK configuration
 * @pipe: pipe with which to synchronize the update
 *
 * Program the hardware based on the passed in CDCLK state,
 * if necessary.
 */
static void intel_set_cdclk(struct drm_i915_private *dev_priv,
			    const struct intel_cdclk_config *cdclk_config,
			    enum pipe pipe)
{
	struct intel_encoder *encoder;

	if (!intel_cdclk_changed(&dev_priv->cdclk.hw, cdclk_config))
		return;

	if (drm_WARN_ON_ONCE(&dev_priv->drm, !dev_priv->cdclk_funcs->set_cdclk))
		return;

	intel_cdclk_dump_config(dev_priv, cdclk_config, "Changing CDCLK to");

	for_each_intel_encoder_with_psr(&dev_priv->drm, encoder) {
		struct intel_dp *intel_dp = enc_to_intel_dp(encoder);

		intel_psr_pause(intel_dp);
	}

	intel_audio_cdclk_change_pre(dev_priv);

	/*
	 * Lock aux/gmbus while we change cdclk in case those
	 * functions use cdclk. Not all platforms/ports do,
	 * but we'll lock them all for simplicity.
	 */
	mutex_lock(&dev_priv->gmbus_mutex);
	for_each_intel_dp(&dev_priv->drm, encoder) {
		struct intel_dp *intel_dp = enc_to_intel_dp(encoder);

		mutex_lock_nest_lock(&intel_dp->aux.hw_mutex,
				     &dev_priv->gmbus_mutex);
	}

	intel_cdclk_set_cdclk(dev_priv, cdclk_config, pipe);

	for_each_intel_dp(&dev_priv->drm, encoder) {
		struct intel_dp *intel_dp = enc_to_intel_dp(encoder);

		mutex_unlock(&intel_dp->aux.hw_mutex);
	}
	mutex_unlock(&dev_priv->gmbus_mutex);

	for_each_intel_encoder_with_psr(&dev_priv->drm, encoder) {
		struct intel_dp *intel_dp = enc_to_intel_dp(encoder);

		intel_psr_resume(intel_dp);
	}

	intel_audio_cdclk_change_post(dev_priv);

	if (drm_WARN(&dev_priv->drm,
		     intel_cdclk_changed(&dev_priv->cdclk.hw, cdclk_config),
		     "cdclk state doesn't match!\n")) {
		intel_cdclk_dump_config(dev_priv, &dev_priv->cdclk.hw, "[hw state]");
		intel_cdclk_dump_config(dev_priv, cdclk_config, "[sw state]");
	}
}

/**
 * intel_set_cdclk_pre_plane_update - Push the CDCLK state to the hardware
 * @state: intel atomic state
 *
 * Program the hardware before updating the HW plane state based on the
 * new CDCLK state, if necessary.
 */
void
intel_set_cdclk_pre_plane_update(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	const struct intel_cdclk_state *old_cdclk_state =
		intel_atomic_get_old_cdclk_state(state);
	const struct intel_cdclk_state *new_cdclk_state =
		intel_atomic_get_new_cdclk_state(state);
	enum pipe pipe = new_cdclk_state->pipe;

	if (!intel_cdclk_changed(&old_cdclk_state->actual,
				 &new_cdclk_state->actual))
		return;

	if (pipe == INVALID_PIPE ||
	    old_cdclk_state->actual.cdclk <= new_cdclk_state->actual.cdclk) {
		drm_WARN_ON(&dev_priv->drm, !new_cdclk_state->base.changed);

		intel_set_cdclk(dev_priv, &new_cdclk_state->actual, pipe);
	}
}

/**
 * intel_set_cdclk_post_plane_update - Push the CDCLK state to the hardware
 * @state: intel atomic state
 *
 * Program the hardware after updating the HW plane state based on the
 * new CDCLK state, if necessary.
 */
void
intel_set_cdclk_post_plane_update(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	const struct intel_cdclk_state *old_cdclk_state =
		intel_atomic_get_old_cdclk_state(state);
	const struct intel_cdclk_state *new_cdclk_state =
		intel_atomic_get_new_cdclk_state(state);
	enum pipe pipe = new_cdclk_state->pipe;

	if (!intel_cdclk_changed(&old_cdclk_state->actual,
				 &new_cdclk_state->actual))
		return;

	if (pipe != INVALID_PIPE &&
	    old_cdclk_state->actual.cdclk > new_cdclk_state->actual.cdclk) {
		drm_WARN_ON(&dev_priv->drm, !new_cdclk_state->base.changed);

		intel_set_cdclk(dev_priv, &new_cdclk_state->actual, pipe);
	}
}

static int intel_pixel_rate_to_cdclk(const struct intel_crtc_state *crtc_state)
{
	int pixel_rate = crtc_state->pixel_rate;

	return DIV_ROUND_UP(pixel_rate, 2);
}

static int intel_planes_min_cdclk(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	struct intel_plane *plane;
	int min_cdclk = 0;

	for_each_intel_plane_on_crtc(&dev_priv->drm, crtc, plane)
		min_cdclk = max(crtc_state->min_cdclk[plane->id], min_cdclk);

	return min_cdclk;
}

int intel_crtc_compute_min_cdclk(const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv =
		to_i915(crtc_state->uapi.crtc->dev);
	int min_cdclk;

	if (!crtc_state->hw.enable)
		return 0;

	min_cdclk = intel_pixel_rate_to_cdclk(crtc_state);

	/*
	 * According to BSpec, "The CD clock frequency must be at least twice
	 * the frequency of the Azalia BCLK." and BCLK is 96 MHz by default.
	 */
	if (crtc_state->has_audio)
		min_cdclk = max(2 * 96000, min_cdclk);

	/* Account for additional needs from the planes */
	min_cdclk = max(intel_planes_min_cdclk(crtc_state), min_cdclk);

	/*
	 * When we decide to use only one VDSC engine, since
	 * each VDSC operates with 1 ppc throughput, pixel clock
	 * cannot be higher than the VDSC clock (cdclk)
	 */
	if (crtc_state->dsc.compression_enable && !crtc_state->dsc.dsc_split)
		min_cdclk = max(min_cdclk, (int)crtc_state->pixel_rate);

	/*
	 * HACK. Currently for TGL platforms we calculate
	 * min_cdclk initially based on pixel_rate divided
	 * by 2, accounting for also plane requirements,
	 * however in some cases the lowest possible CDCLK
	 * doesn't work and causing the underruns.
	 * Explicitly stating here that this seems to be currently
	 * rather a Hack, than final solution.
	 */
	if (IS_TIGERLAKE(dev_priv) || IS_DG2(dev_priv)) {
		/*
		 * Clamp to max_cdclk_freq in case pixel rate is higher,
		 * in order not to break an 8K, but still leave W/A at place.
		 */
		min_cdclk = max_t(int, min_cdclk,
				  min_t(int, crtc_state->pixel_rate,
					dev_priv->max_cdclk_freq));
	}

	return min_cdclk;
}

static int intel_compute_min_cdclk(struct intel_cdclk_state *cdclk_state)
{
	struct intel_atomic_state *state = cdclk_state->base.state;
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	const struct intel_bw_state *bw_state;
	struct intel_crtc *crtc;
	struct intel_crtc_state *crtc_state;
	int min_cdclk, i;
	enum pipe pipe;

	for_each_new_intel_crtc_in_state(state, crtc, crtc_state, i) {
		int ret;

		min_cdclk = intel_crtc_compute_min_cdclk(crtc_state);
		if (min_cdclk < 0)
			return min_cdclk;

		if (cdclk_state->min_cdclk[crtc->pipe] == min_cdclk)
			continue;

		cdclk_state->min_cdclk[crtc->pipe] = min_cdclk;

		ret = intel_atomic_lock_global_state(&cdclk_state->base);
		if (ret)
			return ret;
	}

	bw_state = intel_atomic_get_new_bw_state(state);
	if (bw_state) {
		min_cdclk = intel_bw_min_cdclk(dev_priv, bw_state);

		if (cdclk_state->bw_min_cdclk != min_cdclk) {
			int ret;

			cdclk_state->bw_min_cdclk = min_cdclk;

			ret = intel_atomic_lock_global_state(&cdclk_state->base);
			if (ret)
				return ret;
		}
	}

	min_cdclk = max(cdclk_state->force_min_cdclk,
			cdclk_state->bw_min_cdclk);
	for_each_pipe(dev_priv, pipe)
		min_cdclk = max(cdclk_state->min_cdclk[pipe], min_cdclk);

	if (min_cdclk > dev_priv->max_cdclk_freq) {
		drm_dbg_kms(&dev_priv->drm,
			    "required cdclk (%d kHz) exceeds max (%d kHz)\n",
			    min_cdclk, dev_priv->max_cdclk_freq);
		return -EINVAL;
	}

	return min_cdclk;
}

/*
 * Account for port clock min voltage level requirements.
 * This only really does something on DISPLA_VER >= 11 but can be
 * called on earlier platforms as well.
 *
 * Note that this functions assumes that 0 is
 * the lowest voltage value, and higher values
 * correspond to increasingly higher voltages.
 *
 * Should that relationship no longer hold on
 * future platforms this code will need to be
 * adjusted.
 */
static int bxt_compute_min_voltage_level(struct intel_cdclk_state *cdclk_state)
{
	struct intel_atomic_state *state = cdclk_state->base.state;
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	struct intel_crtc *crtc;
	struct intel_crtc_state *crtc_state;
	u8 min_voltage_level;
	int i;
	enum pipe pipe;

	for_each_new_intel_crtc_in_state(state, crtc, crtc_state, i) {
		int ret;

		if (crtc_state->hw.enable)
			min_voltage_level = crtc_state->min_voltage_level;
		else
			min_voltage_level = 0;

		if (cdclk_state->min_voltage_level[crtc->pipe] == min_voltage_level)
			continue;

		cdclk_state->min_voltage_level[crtc->pipe] = min_voltage_level;

		ret = intel_atomic_lock_global_state(&cdclk_state->base);
		if (ret)
			return ret;
	}

	min_voltage_level = 0;
	for_each_pipe(dev_priv, pipe)
		min_voltage_level = max(cdclk_state->min_voltage_level[pipe],
					min_voltage_level);

	return min_voltage_level;
}

static int bxt_modeset_calc_cdclk(struct intel_cdclk_state *cdclk_state)
{
	struct intel_atomic_state *state = cdclk_state->base.state;
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	int min_cdclk, min_voltage_level, cdclk, vco;

	min_cdclk = intel_compute_min_cdclk(cdclk_state);
	if (min_cdclk < 0)
		return min_cdclk;

	min_voltage_level = bxt_compute_min_voltage_level(cdclk_state);
	if (min_voltage_level < 0)
		return min_voltage_level;

	cdclk = bxt_calc_cdclk(dev_priv, min_cdclk);
	vco = bxt_calc_cdclk_pll_vco(dev_priv, cdclk);

	cdclk_state->logical.vco = vco;
	cdclk_state->logical.cdclk = cdclk;
	cdclk_state->logical.voltage_level =
		max_t(int, min_voltage_level,
		      intel_cdclk_calc_voltage_level(dev_priv, cdclk));

	if (!cdclk_state->active_pipes) {
		cdclk = bxt_calc_cdclk(dev_priv, cdclk_state->force_min_cdclk);
		vco = bxt_calc_cdclk_pll_vco(dev_priv, cdclk);

		cdclk_state->actual.vco = vco;
		cdclk_state->actual.cdclk = cdclk;
		cdclk_state->actual.voltage_level =
			intel_cdclk_calc_voltage_level(dev_priv, cdclk);
	} else {
		cdclk_state->actual = cdclk_state->logical;
	}

	return 0;
}

static struct intel_global_state *intel_cdclk_duplicate_state(struct intel_global_obj *obj)
{
	struct intel_cdclk_state *cdclk_state;

	cdclk_state = kmemdup(obj->state, sizeof(*cdclk_state), GFP_KERNEL);
	if (!cdclk_state)
		return NULL;

	cdclk_state->pipe = INVALID_PIPE;

	return &cdclk_state->base;
}

static void intel_cdclk_destroy_state(struct intel_global_obj *obj,
				      struct intel_global_state *state)
{
	kfree(state);
}

static const struct intel_global_state_funcs intel_cdclk_funcs = {
	.atomic_duplicate_state = intel_cdclk_duplicate_state,
	.atomic_destroy_state = intel_cdclk_destroy_state,
};

struct intel_cdclk_state *
intel_atomic_get_cdclk_state(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	struct intel_global_state *cdclk_state;

	cdclk_state = intel_atomic_get_global_obj_state(state, &dev_priv->cdclk.obj);
	if (IS_ERR(cdclk_state))
		return ERR_CAST(cdclk_state);

	return to_intel_cdclk_state(cdclk_state);
}

int intel_cdclk_atomic_check(struct intel_atomic_state *state,
			     bool *need_cdclk_calc)
{
	const struct intel_cdclk_state *old_cdclk_state;
	const struct intel_cdclk_state *new_cdclk_state;
	struct intel_plane_state *plane_state;
	struct intel_plane *plane;
	int ret;
	int i;

	/*
	 * active_planes bitmask has been updated, and potentially affected
	 * planes are part of the state. We can now compute the minimum cdclk
	 * for each plane.
	 */
	for_each_new_intel_plane_in_state(state, plane, plane_state, i) {
		ret = intel_plane_calc_min_cdclk(state, plane, need_cdclk_calc);
		if (ret)
			return ret;
	}

	ret = intel_bw_calc_min_cdclk(state, need_cdclk_calc);
	if (ret)
		return ret;

	old_cdclk_state = intel_atomic_get_old_cdclk_state(state);
	new_cdclk_state = intel_atomic_get_new_cdclk_state(state);

	if (new_cdclk_state &&
	    old_cdclk_state->force_min_cdclk != new_cdclk_state->force_min_cdclk)
		*need_cdclk_calc = true;

	return 0;
}

int intel_cdclk_init(struct drm_i915_private *dev_priv)
{
	struct intel_cdclk_state *cdclk_state;

	cdclk_state = kzalloc(sizeof(*cdclk_state), GFP_KERNEL);
	if (!cdclk_state)
		return -ENOMEM;

	intel_atomic_global_obj_init(dev_priv, &dev_priv->cdclk.obj,
				     &cdclk_state->base, &intel_cdclk_funcs);

	return 0;
}

int intel_modeset_calc_cdclk(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	const struct intel_cdclk_state *old_cdclk_state;
	struct intel_cdclk_state *new_cdclk_state;
	enum pipe pipe = INVALID_PIPE;
	bool can_fastset = false;
	int ret;

	new_cdclk_state = intel_atomic_get_cdclk_state(state);
	if (IS_ERR(new_cdclk_state))
		return PTR_ERR(new_cdclk_state);

	old_cdclk_state = intel_atomic_get_old_cdclk_state(state);

	new_cdclk_state->active_pipes =
		intel_calc_active_pipes(state, old_cdclk_state->active_pipes);

	ret = intel_cdclk_modeset_calc_cdclk(dev_priv, new_cdclk_state);
	if (ret)
		return ret;

	if (intel_cdclk_changed(&old_cdclk_state->actual,
				&new_cdclk_state->actual)) {
		/*
		 * Also serialize commits across all crtcs
		 * if the actual hw needs to be poked.
		 */
		ret = intel_atomic_serialize_global_state(&new_cdclk_state->base);
		if (ret)
			return ret;
	} else if (old_cdclk_state->active_pipes != new_cdclk_state->active_pipes ||
		   old_cdclk_state->force_min_cdclk != new_cdclk_state->force_min_cdclk ||
		   intel_cdclk_changed(&old_cdclk_state->logical,
				       &new_cdclk_state->logical)) {
		ret = intel_atomic_lock_global_state(&new_cdclk_state->base);
		if (ret)
			return ret;
	} else {
		return 0;
	}

	if (is_power_of_2(new_cdclk_state->active_pipes) &&
	    intel_cdclk_can_cd2x_update(dev_priv,
					&old_cdclk_state->actual,
					&new_cdclk_state->actual)) {
		struct intel_crtc *crtc;
		struct intel_crtc_state *crtc_state;

		pipe = ilog2(new_cdclk_state->active_pipes);
		crtc = intel_crtc_for_pipe(dev_priv, pipe);

		crtc_state = intel_atomic_get_crtc_state(&state->base, crtc);
		if (IS_ERR(crtc_state))
			return PTR_ERR(crtc_state);

		if (drm_atomic_crtc_needs_modeset(&crtc_state->uapi))
			pipe = INVALID_PIPE;
	}

	if (DISPLAY_VER(dev_priv) >= 14) {
		if (intel_cdclk_can_squash_and_crawl(dev_priv,
						     &old_cdclk_state->actual,
						     &new_cdclk_state->actual)) {
			drm_dbg_kms(&dev_priv->drm,
				    "Can change cdclk via squasher and crawler combinations\n");
			can_fastset = true;
		}
	} else {
		if (intel_cdclk_can_squash(dev_priv,
					   &old_cdclk_state->actual,
					   &new_cdclk_state->actual)) {
			drm_dbg_kms(&dev_priv->drm, "Can change cdclk via squasher\n");
			can_fastset = true;
		} else if (intel_cdclk_can_crawl(dev_priv,
						 &old_cdclk_state->actual,
						 &new_cdclk_state->actual)) {
			drm_dbg_kms(&dev_priv->drm, "Can change cdclk via crawl\n");
			can_fastset = true;
		} else if (pipe != INVALID_PIPE) {
			new_cdclk_state->pipe = pipe;

			drm_dbg_kms(&dev_priv->drm,
				    "Can change cdclk cd2x divider with pipe %c active\n",
				    pipe_name(pipe));
			can_fastset = true;
		}
	}

	if (!can_fastset && intel_cdclk_needs_modeset(&old_cdclk_state->actual,
						      &new_cdclk_state->actual)) {
		/* All pipes must be switched off while we change the cdclk. */
		ret = intel_modeset_all_pipes(state);
		if (ret)
			return ret;

		drm_dbg_kms(&dev_priv->drm,
			    "Modeset required for cdclk change\n");
	}

	drm_dbg_kms(&dev_priv->drm,
		    "New cdclk calculated to be logical %u kHz, actual %u kHz\n",
		    new_cdclk_state->logical.cdclk,
		    new_cdclk_state->actual.cdclk);
	drm_dbg_kms(&dev_priv->drm,
		    "New voltage level calculated to be logical %u, actual %u\n",
		    new_cdclk_state->logical.voltage_level,
		    new_cdclk_state->actual.voltage_level);

	return 0;
}

static int intel_compute_max_dotclk(struct drm_i915_private *dev_priv)
{
	int max_cdclk_freq = dev_priv->max_cdclk_freq;

	return 2 * max_cdclk_freq;
}

/**
 * intel_update_max_cdclk - Determine the maximum support CDCLK frequency
 * @dev_priv: i915 device
 *
 * Determine the maximum CDCLK frequency the platform supports, and also
 * derive the maximum dot clock frequency the maximum CDCLK frequency
 * allows.
 */
void intel_update_max_cdclk(struct drm_i915_private *dev_priv)
{
	if (dev_priv->cdclk.hw.ref == 24000)
		dev_priv->max_cdclk_freq = 648000;
	else
		dev_priv->max_cdclk_freq = 652800;

	dev_priv->max_dotclk_freq = intel_compute_max_dotclk(dev_priv);

	drm_dbg(&dev_priv->drm, "Max CD clock rate: %d kHz\n",
		dev_priv->max_cdclk_freq);

	drm_dbg(&dev_priv->drm, "Max dotclock rate: %d kHz\n",
		dev_priv->max_dotclk_freq);
}

/**
 * intel_update_cdclk - Determine the current CDCLK frequency
 * @dev_priv: i915 device
 *
 * Determine the current CDCLK frequency.
 */
void intel_update_cdclk(struct drm_i915_private *dev_priv)
{
	intel_cdclk_get_cdclk(dev_priv, &dev_priv->cdclk.hw);
}

static int dg1_rawclk(struct drm_i915_private *dev_priv)
{
	/*
	 * DG1 always uses a 38.4 MHz rawclk.  The bspec tells us
	 * "Program Numerator=2, Denominator=4, Divider=37 decimal."
	 */
	intel_de_write(dev_priv, PCH_RAWCLK_FREQ,
		       CNP_RAWCLK_DEN(4) | CNP_RAWCLK_DIV(37) | ICP_RAWCLK_NUM(2));

	return 38400;
}

static int cnp_rawclk(struct drm_i915_private *dev_priv)
{
	u32 rawclk;
	int divider, fraction;

	if (intel_de_read(dev_priv, SFUSE_STRAP) & SFUSE_STRAP_RAW_FREQUENCY) {
		/* 24 MHz */
		divider = 24000;
		fraction = 0;
	} else {
		/* 19.2 MHz */
		divider = 19000;
		fraction = 200;
	}

	rawclk = CNP_RAWCLK_DIV(divider / 1000);
	if (fraction) {
		int numerator = 1;

		rawclk |= CNP_RAWCLK_DEN(DIV_ROUND_CLOSEST(numerator * 1000,
							   fraction) - 1);
		rawclk |= ICP_RAWCLK_NUM(numerator);
	}

	intel_de_write(dev_priv, PCH_RAWCLK_FREQ, rawclk);
	return divider + fraction;
}

/**
 * intel_read_rawclk - Determine the current RAWCLK frequency
 * @dev_priv: i915 device
 *
 * Determine the current RAWCLK frequency. RAWCLK is a fixed
 * frequency clock so this needs to done only once.
 */
u32 intel_read_rawclk(struct drm_i915_private *dev_priv)
{
	u32 freq;

	if (INTEL_PCH_TYPE(dev_priv) >= PCH_DG1)
		freq = dg1_rawclk(dev_priv);
	else if (INTEL_PCH_TYPE(dev_priv) >= PCH_MTP)
		/*
		 * MTL always uses a 38.4 MHz rawclk.  The bspec tells us
		 * "RAWCLK_FREQ defaults to the values for 38.4 and does
		 * not need to be programmed."
		 */
		freq = 38400;
	else
		freq = cnp_rawclk(dev_priv);

	return freq;
}

static const struct intel_cdclk_funcs mtl_cdclk_funcs = {
	.get_cdclk = mtl_get_cdclk,
	.set_cdclk = mtl_set_cdclk,
	.modeset_calc_cdclk = bxt_modeset_calc_cdclk,
	.calc_voltage_level = tgl_calc_voltage_level,
};

static const struct intel_cdclk_funcs tgl_cdclk_funcs = {
	.get_cdclk = bxt_get_cdclk,
	.set_cdclk = bxt_set_cdclk,
	.modeset_calc_cdclk = bxt_modeset_calc_cdclk,
	.calc_voltage_level = tgl_calc_voltage_level,
};

/**
 * intel_init_cdclk_hooks - Initialize CDCLK related modesetting hooks
 * @dev_priv: i915 device
 */
void intel_init_cdclk_hooks(struct drm_i915_private *dev_priv)
{
	if (IS_METEORLAKE(dev_priv)) {
		dev_priv->cdclk_funcs = &mtl_cdclk_funcs;
		dev_priv->cdclk.table = mtl_cdclk_table;
	} else if (IS_DG2(dev_priv)) {
		dev_priv->cdclk_funcs = &tgl_cdclk_funcs;
		dev_priv->cdclk.table = dg2_cdclk_table;
	} else if (IS_ALDERLAKE_P(dev_priv)) {
		dev_priv->cdclk_funcs = &tgl_cdclk_funcs;
		/* Wa_22011320316:adl-p[a0] */
		if (IS_ADLP_DISPLAY_STEP(dev_priv, STEP_A0, STEP_B0))
			dev_priv->cdclk.table = adlp_a_step_cdclk_table;
		else
			dev_priv->cdclk.table = adlp_cdclk_table;
	} else if (IS_ROCKETLAKE(dev_priv)) {
		dev_priv->cdclk_funcs = &tgl_cdclk_funcs;
		dev_priv->cdclk.table = rkl_cdclk_table;
	} else {
		dev_priv->cdclk_funcs = &tgl_cdclk_funcs;
		dev_priv->cdclk.table = icl_cdclk_table;
	}
}
