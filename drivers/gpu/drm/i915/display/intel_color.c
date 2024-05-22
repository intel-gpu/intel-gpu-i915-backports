/*
 * Copyright © 2016 Intel Corporation
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
 *
 */

#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
#include <drm/drm_plane.h>
#endif

#ifdef BPM_DRM_PLANE_ATTACH_CTM_PROPERTY_API_PRESENT
#include "intel_atomic_plane.h"
#endif

#include "intel_color.h"
#include "intel_de.h"
#include "intel_display_types.h"
#include "intel_dpll.h"
#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
#include "intel_sprite.h"
#include "skl_universal_plane.h"
#endif

struct intel_color_funcs {
	int (*color_check)(struct intel_crtc_state *crtc_state);
	/*
	 * Program non-arming double buffered color management registers
	 * before vblank evasion. The registers should then latch after
	 * the arming register is written (by color_commit_arm()) during
	 * the next vblank start, alongside any other double buffered
	 * registers involved with the same commit. This hook is optional.
	 */
	void (*color_commit_noarm)(const struct intel_crtc_state *crtc_state);
	/*
	 * Program arming double buffered color management registers
	 * during vblank evasion. The registers (and whatever other registers
	 * they arm that were written by color_commit_noarm) should then latch
	 * during the next vblank start, alongside any other double buffered
	 * registers involved with the same commit.
	 */
	void (*color_commit_arm)(const struct intel_crtc_state *crtc_state);
	/*
	 * Load LUTs (and other single buffered color management
	 * registers). Will (hopefully) be called during the vblank
	 * following the latching of any double buffered registers
	 * involved with the same commit.
	 */
	void (*load_luts)(const struct intel_crtc_state *crtc_state);
	void (*read_luts)(struct intel_crtc_state *crtc_state);
#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
	/* Add Plane Color callbacks */
	void (*load_plane_csc_matrix)(const struct drm_plane_state *plane_state);
	void (*load_plane_luts)(const struct drm_plane_state *plane_state);
#endif
};

#define CTM_COEFF_SIGN	(1ULL << 63)

#define CTM_COEFF_1_0	(1ULL << 32)
#define CTM_COEFF_2_0	(CTM_COEFF_1_0 << 1)
#define CTM_COEFF_4_0	(CTM_COEFF_2_0 << 1)
#define CTM_COEFF_8_0	(CTM_COEFF_4_0 << 1)
#define CTM_COEFF_0_5	(CTM_COEFF_1_0 >> 1)
#define CTM_COEFF_0_25	(CTM_COEFF_0_5 >> 1)
#define CTM_COEFF_0_125	(CTM_COEFF_0_25 >> 1)

#define CTM_COEFF_LIMITED_RANGE ((235ULL - 16ULL) * CTM_COEFF_1_0 / 255)

#define CTM_COEFF_NEGATIVE(coeff)	(((coeff) & CTM_COEFF_SIGN) != 0)
#define CTM_COEFF_ABS(coeff)		((coeff) & (CTM_COEFF_SIGN - 1))

#define LEGACY_LUT_LENGTH		256

/*
 * ILK+ csc matrix:
 *
 * |R/Cr|   | c0 c1 c2 |   ( |R/Cr|   |preoff0| )   |postoff0|
 * |G/Y | = | c3 c4 c5 | x ( |G/Y | + |preoff1| ) + |postoff1|
 * |B/Cb|   | c6 c7 c8 |   ( |B/Cb|   |preoff2| )   |postoff2|
 *
 * ILK/SNB don't have explicit post offsets, and instead
 * CSC_MODE_YUV_TO_RGB and CSC_BLACK_SCREEN_OFFSET are used:
 *  CSC_MODE_YUV_TO_RGB=0 + CSC_BLACK_SCREEN_OFFSET=0 -> 1/2, 0, 1/2
 *  CSC_MODE_YUV_TO_RGB=0 + CSC_BLACK_SCREEN_OFFSET=1 -> 1/2, 1/16, 1/2
 *  CSC_MODE_YUV_TO_RGB=1 + CSC_BLACK_SCREEN_OFFSET=0 -> 0, 0, 0
 *  CSC_MODE_YUV_TO_RGB=1 + CSC_BLACK_SCREEN_OFFSET=1 -> 1/16, 1/16, 1/16
 */

/*
 * Extract the CSC coefficient from a CTM coefficient (in U32.32 fixed point
 * format). This macro takes the coefficient we want transformed and the
 * number of fractional bits.
 *
 * We only have a 9 bits precision window which slides depending on the value
 * of the CTM coefficient and we write the value from bit 3. We also round the
 * value.
 */
#define ILK_CSC_COEFF_FP(coeff, fbits)	\
	(clamp_val(((coeff) >> (32 - (fbits) - 3)) + 4, 0, 0xfff) & 0xff8)

#define ILK_CSC_COEFF_LIMITED_RANGE 0x0dc0
#define ILK_CSC_COEFF_1_0 0x7800

#define ILK_CSC_POSTOFF_LIMITED_RANGE (16 * (1 << 12) / 255)

#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
#define GAMMA_MODE_LEGACY_PALETTE_8BIT		BIT(0)
#define GAMMA_MODE_PRECISION_PALETTE_10BIT	BIT(1)
#define GAMMA_MODE_INTERPOLATED_12BIT		BIT(2)
#define GAMMA_MODE_MULTI_SEGMENTED_12BIT	BIT(3)
#define GAMMA_MODE_SPLIT_12BIT			BIT(4)
#define GAMMA_MODE_LOGARITHMIC_12BIT		BIT(5) /* XELPD+ */
#endif

#ifndef BPM_DGLUT_24BIT_MTL_NOT_SUPPORTED
#define DEGAMMA_MODE_24BIT			BIT(0) /* MTL/D14+ */
#endif

#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
#define INTEL_GAMMA_MODE_MASK (\
		GAMMA_MODE_LEGACY_PALETTE_8BIT | \
		GAMMA_MODE_PRECISION_PALETTE_10BIT | \
		GAMMA_MODE_INTERPOLATED_12BIT | \
		GAMMA_MODE_MULTI_SEGMENTED_12BIT | \
		GAMMA_MODE_SPLIT_12BIT \
		GAMMA_MODE_LOGARITHMIC_12BIT)
#endif

/* Nop pre/post offsets */
static const u16 ilk_csc_off_zero[3] = {};

/* Limited range RGB post offsets */
static const u16 ilk_csc_postoff_limited_range[3] = {
	ILK_CSC_POSTOFF_LIMITED_RANGE,
	ILK_CSC_POSTOFF_LIMITED_RANGE,
	ILK_CSC_POSTOFF_LIMITED_RANGE,
};

/* Full range RGB -> limited range RGB matrix */
static const u16 ilk_csc_coeff_limited_range[9] = {
	ILK_CSC_COEFF_LIMITED_RANGE, 0, 0,
	0, ILK_CSC_COEFF_LIMITED_RANGE, 0,
	0, 0, ILK_CSC_COEFF_LIMITED_RANGE,
};

/* BT.709 full range RGB -> limited range YCbCr matrix */
static const u16 ilk_csc_coeff_rgb_to_ycbcr[9] = {
	0x1e08, 0x9cc0, 0xb528,
	0x2ba8, 0x09d8, 0x37e8,
	0xbce8, 0x9ad8, 0x1e08,
};

/* Limited range YCbCr post offsets */
static const u16 ilk_csc_postoff_rgb_to_ycbcr[3] = {
	0x0800, 0x0100, 0x0800,
};

static bool lut_is_legacy(const struct drm_property_blob *lut)
{
	return drm_color_lut_size(lut) == LEGACY_LUT_LENGTH;
}

static bool crtc_state_is_legacy_gamma(const struct intel_crtc_state *crtc_state)
{
	return !crtc_state->hw.degamma_lut &&
		!crtc_state->hw.ctm &&
		crtc_state->hw.gamma_lut &&
		lut_is_legacy(crtc_state->hw.gamma_lut);
}

#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
/*
 * Added to accommodate enhanced LUT precision.
 * Max LUT precision is 32 bits.
 */
static u64 drm_color_lut_extract_ext(u64 user_input, u32 bit_precision)
{
	u64 val = user_input & 0xffffffff;
	u32 max;

	if (bit_precision > 32)
		return 0;

	max = 0xffffffff >> (32 - bit_precision);
	/* Round only if we're not using full precision. */
	if (bit_precision < 32) {
		val += 1UL << (32 - bit_precision - 1);
		val >>= 32 - bit_precision;
	}

	return ((user_input & 0xffffffff00000000) |
		clamp_val(val, 0, max));
}
#endif

static void ilk_update_pipe_csc(struct intel_crtc *crtc,
				const u16 preoff[3],
				const u16 coeff[9],
				const u16 postoff[3])
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum pipe pipe = crtc->pipe;

	intel_de_write_fw(dev_priv, PIPE_CSC_PREOFF_HI(pipe), preoff[0]);
	intel_de_write_fw(dev_priv, PIPE_CSC_PREOFF_ME(pipe), preoff[1]);
	intel_de_write_fw(dev_priv, PIPE_CSC_PREOFF_LO(pipe), preoff[2]);

	intel_de_write_fw(dev_priv, PIPE_CSC_COEFF_RY_GY(pipe),
			  coeff[0] << 16 | coeff[1]);
	intel_de_write_fw(dev_priv, PIPE_CSC_COEFF_BY(pipe), coeff[2] << 16);

	intel_de_write_fw(dev_priv, PIPE_CSC_COEFF_RU_GU(pipe),
			  coeff[3] << 16 | coeff[4]);
	intel_de_write_fw(dev_priv, PIPE_CSC_COEFF_BU(pipe), coeff[5] << 16);

	intel_de_write_fw(dev_priv, PIPE_CSC_COEFF_RV_GV(pipe),
			  coeff[6] << 16 | coeff[7]);
	intel_de_write_fw(dev_priv, PIPE_CSC_COEFF_BV(pipe), coeff[8] << 16);

	intel_de_write_fw(dev_priv, PIPE_CSC_POSTOFF_HI(pipe), postoff[0]);
	intel_de_write_fw(dev_priv, PIPE_CSC_POSTOFF_ME(pipe), postoff[1]);
	intel_de_write_fw(dev_priv, PIPE_CSC_POSTOFF_LO(pipe), postoff[2]);
}

static void icl_update_output_csc(struct intel_crtc *crtc,
				  const u16 preoff[3],
				  const u16 coeff[9],
				  const u16 postoff[3])
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum pipe pipe = crtc->pipe;

	intel_de_write_fw(dev_priv, PIPE_CSC_OUTPUT_PREOFF_HI(pipe), preoff[0]);
	intel_de_write_fw(dev_priv, PIPE_CSC_OUTPUT_PREOFF_ME(pipe), preoff[1]);
	intel_de_write_fw(dev_priv, PIPE_CSC_OUTPUT_PREOFF_LO(pipe), preoff[2]);

	intel_de_write_fw(dev_priv, PIPE_CSC_OUTPUT_COEFF_RY_GY(pipe),
			  coeff[0] << 16 | coeff[1]);
	intel_de_write_fw(dev_priv, PIPE_CSC_OUTPUT_COEFF_BY(pipe),
			  coeff[2] << 16);

	intel_de_write_fw(dev_priv, PIPE_CSC_OUTPUT_COEFF_RU_GU(pipe),
			  coeff[3] << 16 | coeff[4]);
	intel_de_write_fw(dev_priv, PIPE_CSC_OUTPUT_COEFF_BU(pipe),
			  coeff[5] << 16);

	intel_de_write_fw(dev_priv, PIPE_CSC_OUTPUT_COEFF_RV_GV(pipe),
			  coeff[6] << 16 | coeff[7]);
	intel_de_write_fw(dev_priv, PIPE_CSC_OUTPUT_COEFF_BV(pipe),
			  coeff[8] << 16);

	intel_de_write_fw(dev_priv, PIPE_CSC_OUTPUT_POSTOFF_HI(pipe), postoff[0]);
	intel_de_write_fw(dev_priv, PIPE_CSC_OUTPUT_POSTOFF_ME(pipe), postoff[1]);
	intel_de_write_fw(dev_priv, PIPE_CSC_OUTPUT_POSTOFF_LO(pipe), postoff[2]);
}

static void ilk_csc_convert_ctm(const struct intel_crtc_state *crtc_state,
				u16 coeffs[9])
{
	const struct drm_color_ctm *ctm = crtc_state->hw.ctm->data;
	const u64 *input = ctm->matrix;
	int i;

	/*
	 * Convert fixed point S31.32 input to format supported by the
	 * hardware.
	 */
	for (i = 0; i < 9; i++) {
		u64 abs_coeff = ((1ULL << 63) - 1) & input[i];

		/*
		 * Clamp input value to min/max supported by
		 * hardware.
		 */
		abs_coeff = clamp_val(abs_coeff, 0, CTM_COEFF_4_0 - 1);

		coeffs[i] = 0;

		/* sign bit */
		if (CTM_COEFF_NEGATIVE(input[i]))
			coeffs[i] |= 1 << 15;

		if (abs_coeff < CTM_COEFF_0_125)
			coeffs[i] |= (3 << 12) |
				ILK_CSC_COEFF_FP(abs_coeff, 12);
		else if (abs_coeff < CTM_COEFF_0_25)
			coeffs[i] |= (2 << 12) |
				ILK_CSC_COEFF_FP(abs_coeff, 11);
		else if (abs_coeff < CTM_COEFF_0_5)
			coeffs[i] |= (1 << 12) |
				ILK_CSC_COEFF_FP(abs_coeff, 10);
		else if (abs_coeff < CTM_COEFF_1_0)
			coeffs[i] |= ILK_CSC_COEFF_FP(abs_coeff, 9);
		else if (abs_coeff < CTM_COEFF_2_0)
			coeffs[i] |= (7 << 12) |
				ILK_CSC_COEFF_FP(abs_coeff, 8);
		else
			coeffs[i] |= (6 << 12) |
				ILK_CSC_COEFF_FP(abs_coeff, 7);
	}
}

static void icl_load_csc_matrix(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);

	if (crtc_state->hw.ctm) {
		u16 coeff[9];

		ilk_csc_convert_ctm(crtc_state, coeff);
		ilk_update_pipe_csc(crtc, ilk_csc_off_zero,
				    coeff, ilk_csc_off_zero);
	}

	if (crtc_state->output_format != INTEL_OUTPUT_FORMAT_RGB) {
		icl_update_output_csc(crtc, ilk_csc_off_zero,
				      ilk_csc_coeff_rgb_to_ycbcr,
				      ilk_csc_postoff_rgb_to_ycbcr);
	} else if (crtc_state->limited_color_range) {
		icl_update_output_csc(crtc, ilk_csc_off_zero,
				      ilk_csc_coeff_limited_range,
				      ilk_csc_postoff_limited_range);
	}
}

/* convert hw value with given bit_precision to lut property val */
static u32 intel_color_lut_pack(u32 val, int bit_precision)
{
	u32 max = 0xffff >> (16 - bit_precision);

	val = clamp_val(val, 0, max);

	if (bit_precision < 16)
		val <<= 16 - bit_precision;

	return val;
}

static u32 i9xx_lut_8(const struct drm_color_lut *color)
{
	return drm_color_lut_extract(color->red, 8) << 16 |
		drm_color_lut_extract(color->green, 8) << 8 |
		drm_color_lut_extract(color->blue, 8);
}

static void i9xx_lut_8_pack(struct drm_color_lut *entry, u32 val)
{
	entry->red = intel_color_lut_pack(REG_FIELD_GET(LGC_PALETTE_RED_MASK, val), 8);
	entry->green = intel_color_lut_pack(REG_FIELD_GET(LGC_PALETTE_GREEN_MASK, val), 8);
	entry->blue = intel_color_lut_pack(REG_FIELD_GET(LGC_PALETTE_BLUE_MASK, val), 8);
}

/* i965+ "10.6" bit interpolated format "even DW" (low 8 bits) */
/* i965+ "10.6" interpolated format "odd DW" (high 8 bits) */
static u32 ilk_lut_10(const struct drm_color_lut *color)
{
	return drm_color_lut_extract(color->red, 10) << 20 |
		drm_color_lut_extract(color->green, 10) << 10 |
		drm_color_lut_extract(color->blue, 10);
}

static void ilk_lut_10_pack(struct drm_color_lut *entry, u32 val)
{
	entry->red = intel_color_lut_pack(REG_FIELD_GET(PREC_PALETTE_RED_MASK, val), 10);
	entry->green = intel_color_lut_pack(REG_FIELD_GET(PREC_PALETTE_GREEN_MASK, val), 10);
	entry->blue = intel_color_lut_pack(REG_FIELD_GET(PREC_PALETTE_BLUE_MASK, val), 10);
}

static void icl_lut_multi_seg_pack(struct drm_color_lut *entry, u32 ldw, u32 udw)
{
	entry->red = REG_FIELD_GET(PAL_PREC_MULTI_SEG_RED_UDW_MASK, udw) << 6 |
				   REG_FIELD_GET(PAL_PREC_MULTI_SEG_RED_LDW_MASK, ldw);
	entry->green = REG_FIELD_GET(PAL_PREC_MULTI_SEG_GREEN_UDW_MASK, udw) << 6 |
				     REG_FIELD_GET(PAL_PREC_MULTI_SEG_GREEN_LDW_MASK, ldw);
	entry->blue = REG_FIELD_GET(PAL_PREC_MULTI_SEG_BLUE_UDW_MASK, udw) << 6 |
				    REG_FIELD_GET(PAL_PREC_MULTI_SEG_BLUE_LDW_MASK, ldw);
}

static void icl_color_commit_noarm(const struct intel_crtc_state *crtc_state)
{
	icl_load_csc_matrix(crtc_state);
}

static void skl_color_commit_arm(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum pipe pipe = crtc->pipe;
	u32 val = 0;

	/*
	 * We don't (yet) allow userspace to control the pipe background color,
	 * so force it to black, but apply pipe gamma and CSC appropriately
	 * so that its handling will match how we program our planes.
	 */
	if (crtc_state->gamma_enable)
		val |= SKL_BOTTOM_COLOR_GAMMA_ENABLE;
	if (crtc_state->csc_enable)
		val |= SKL_BOTTOM_COLOR_CSC_ENABLE;
	intel_de_write(dev_priv, SKL_BOTTOM_COLOR(pipe), val);

	intel_de_write(dev_priv, GAMMA_MODE(crtc->pipe),
		       crtc_state->gamma_mode);

	intel_de_write_fw(dev_priv, PIPE_CSC_MODE(crtc->pipe),
			  crtc_state->csc_mode);
}

static void ilk_load_lut_8(struct intel_crtc *crtc,
			   const struct drm_property_blob *blob)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	const struct drm_color_lut *lut;
	enum pipe pipe = crtc->pipe;
	int i;

	if (!blob)
		return;

	lut = blob->data;

	for (i = 0; i < 256; i++)
		intel_de_write_fw(dev_priv, LGC_PALETTE(pipe, i),
				  i9xx_lut_8(&lut[i]));
}

static int ivb_lut_10_size(u32 prec_index)
{
	if (prec_index & PAL_PREC_SPLIT_MODE)
		return 512;
	else
		return 1024;
}

/* On BDW+ the index auto increment mode actually works */
static void bdw_load_lut_10(struct intel_crtc *crtc,
			    const struct drm_property_blob *blob,
			    u32 prec_index)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	int hw_lut_size = ivb_lut_10_size(prec_index);
	const struct drm_color_lut *lut = blob->data;
	int i, lut_size = drm_color_lut_size(blob);
	enum pipe pipe = crtc->pipe;

	intel_de_write_fw(dev_priv, PREC_PAL_INDEX(pipe),
			  prec_index | PAL_PREC_AUTO_INCREMENT);

	for (i = 0; i < hw_lut_size; i++) {
		/* We discard half the user entries in split gamma mode */
		const struct drm_color_lut *entry =
			&lut[i * (lut_size - 1) / (hw_lut_size - 1)];

		intel_de_write_fw(dev_priv, PREC_PAL_DATA(pipe),
				  ilk_lut_10(entry));
	}

	/*
	 * Reset the index, otherwise it prevents the legacy palette to be
	 * written properly.
	 */
	intel_de_write_fw(dev_priv, PREC_PAL_INDEX(pipe), 0);
}

static void ivb_load_lut_ext_max(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	enum pipe pipe = crtc->pipe;

	/* Program the max register to clamp values > 1.0. */
	intel_dsb_reg_write(crtc_state, PREC_PAL_EXT_GC_MAX(pipe, 0), 1 << 16);
	intel_dsb_reg_write(crtc_state, PREC_PAL_EXT_GC_MAX(pipe, 1), 1 << 16);
	intel_dsb_reg_write(crtc_state, PREC_PAL_EXT_GC_MAX(pipe, 2), 1 << 16);

	/*
	 * Program the gc max 2 register to clamp values > 1.0.
	 * ToDo: Extend the ABI to be able to program values
	 * from 3.0 to 7.0
	 */
	intel_dsb_reg_write(crtc_state, PREC_PAL_EXT2_GC_MAX(pipe, 0), 1 << 16);
	intel_dsb_reg_write(crtc_state, PREC_PAL_EXT2_GC_MAX(pipe, 1), 1 << 16);
	intel_dsb_reg_write(crtc_state, PREC_PAL_EXT2_GC_MAX(pipe, 2), 1 << 16);
}

static int glk_degamma_lut_size(struct drm_i915_private *i915)
{
	if (DISPLAY_VER(i915) >= 13)
		return 131;
	else
		return 35;
}

static void glk_load_degamma_lut(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum pipe pipe = crtc->pipe;
	int i, lut_size = INTEL_INFO(dev_priv)->display.color.degamma_lut_size;
	const struct drm_color_lut *lut = crtc_state->hw.degamma_lut->data;

	/*
	 * When setting the auto-increment bit, the hardware seems to
	 * ignore the index bits, so we need to reset it to index 0
	 * separately.
	 */
	intel_de_write_fw(dev_priv, PRE_CSC_GAMC_INDEX(pipe), 0);
	intel_de_write_fw(dev_priv, PRE_CSC_GAMC_INDEX(pipe),
			  PRE_CSC_GAMC_AUTO_INCREMENT);

	for (i = 0; i < lut_size; i++) {
		/*
		 * First lut_size entries represent range from 0 to 1.0
		 * 3 additional lut entries will represent extended range
		 * inputs 3.0 and 7.0 respectively, currently clamped
		 * at 1.0. Since the precision is 16bit, the user
		 * value can be directly filled to register.
		 * The pipe degamma table in GLK+ onwards doesn't
		 * support different values per channel, so this just
		 * programs green value which will be equal to Red and
		 * Blue into the lut registers.
		 * ToDo: Extend to max 7.0. Enable 32 bit input value
		 * as compared to just 16 to achieve this.
		 */
		intel_de_write_fw(dev_priv, PRE_CSC_GAMC_DATA(pipe),
				  lut[i].green);
	}

	/* Clamp values > 1.0. */
	while (i++ < glk_degamma_lut_size(dev_priv))
		intel_de_write_fw(dev_priv, PRE_CSC_GAMC_DATA(pipe), 1 << 16);

	intel_de_write_fw(dev_priv, PRE_CSC_GAMC_INDEX(pipe), 0);
}

#ifndef BPM_DGLUT_24BIT_MTL_NOT_SUPPORTED
static void mtl_load_legacy_lut(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *i915 = to_i915(crtc->base.dev);
	const struct drm_property_blob *degamma_lut_blob = crtc_state->hw.degamma_lut;
	struct drm_color_lut *degamma_lut = degamma_lut_blob->data;
	enum pipe pipe = crtc->pipe;
	int i, lut_size = drm_color_lut_size(degamma_lut_blob);

	/*
	 * When setting the auto-increment bit, the hardware seems to
	 * ignore the index bits, so we need to reset it to index 0
	 * separately.
	 */
	intel_de_write_fw(i915, PRE_CSC_GAMC_INDEX(pipe), 0);
	intel_de_write_fw(i915, PRE_CSC_GAMC_INDEX(pipe),
			  PRE_CSC_GAMC_AUTO_INCREMENT);

	for (i = 0; i < lut_size; i++) {
		u64 word = mul_u32_u32(degamma_lut[i].green, (1 << 24)) / (1 << 16);
		u32 lut_val = (word & 0xffffff);

		intel_de_write_fw(i915, PRE_CSC_GAMC_DATA(pipe),
				  lut_val);
	}
	/* Clamp values > 1.0. */
	while (i++ < glk_degamma_lut_size(i915))
		intel_de_write_fw(i915, PRE_CSC_GAMC_DATA(pipe), 1 << 24);

	intel_de_write_fw(i915, PRE_CSC_GAMC_INDEX(pipe), 0);
}

static void mtl_load_degamma_lut(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *i915 = to_i915(crtc->base.dev);
	struct drm_color_lut_ext *degamma_lut = crtc_state->hw.degamma_lut->data;
	u32 i, lut_size = INTEL_INFO(i915)->display.color.degamma_lut_size;
	enum pipe pipe = crtc->pipe;

	if (crtc_state->uapi.degamma_mode_type == 0) {
		if (!crtc_state->uapi.advance_degamma_mode_active)
			mtl_load_legacy_lut(crtc_state);
		return;
	}

	/*
	 * When setting the auto-increment bit, the hardware seems to
	 * ignore the index bits, so we need to reset it to index 0
	 * separately.
	 */
	intel_de_write_fw(i915, PRE_CSC_GAMC_INDEX(pipe), 0);
	intel_de_write_fw(i915, PRE_CSC_GAMC_INDEX(pipe),
			  PRE_CSC_GAMC_AUTO_INCREMENT);

	for (i = 0; i < lut_size; i++) {
		u64 word = drm_color_lut_extract_ext(degamma_lut[i].green, 24);
		u32 lut_val = (word & 0xffffff);

		intel_de_write_fw(i915, PRE_CSC_GAMC_DATA(pipe),
				  lut_val);
	}

	/*
	 * Clamp values > 1.0.
	 * TODO: Extend to max 7.0.
	 */
	while (i++ < glk_degamma_lut_size(i915))
		intel_de_write_fw(i915, PRE_CSC_GAMC_DATA(pipe), 1 << 24);

	intel_de_write_fw(i915, PRE_CSC_GAMC_INDEX(pipe), 0);
}
#endif

/* ilk+ "12.4" interpolated format (high 10 bits) */
static u32 ilk_lut_12p4_udw(const struct drm_color_lut *color)
{
	return (color->red >> 6) << 20 | (color->green >> 6) << 10 |
		(color->blue >> 6);
}

/* ilk+ "12.4" interpolated format (low 6 bits) */
static u32 ilk_lut_12p4_ldw(const struct drm_color_lut *color)
{
	return (color->red & 0x3f) << 24 | (color->green & 0x3f) << 14 |
		(color->blue & 0x3f) << 4;
}

static void
icl_load_gcmax(const struct intel_crtc_state *crtc_state,
	       const struct drm_color_lut *color)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
	struct drm_i915_private *i915 = to_i915(crtc_state->uapi.crtc->dev);
#endif
	enum pipe pipe = crtc->pipe;

#ifdef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
	/* FIXME LUT entries are 16 bit only, so we can prog 0xFFFF max */
	intel_dsb_reg_write(crtc_state, PREC_PAL_GC_MAX(pipe, 0), color->red);
	intel_dsb_reg_write(crtc_state, PREC_PAL_GC_MAX(pipe, 1), color->green);
	intel_dsb_reg_write(crtc_state, PREC_PAL_GC_MAX(pipe, 2), color->blue);
#else
	if (DISPLAY_VER(i915) >= 13) {
		/* MAx val from UAPI is 16bit only, so setting fixed for GC max */
		intel_dsb_reg_write(crtc_state, PREC_PAL_GC_MAX(pipe, 0), 1 << 16);
		intel_dsb_reg_write(crtc_state, PREC_PAL_GC_MAX(pipe, 1), 1 << 16);
		intel_dsb_reg_write(crtc_state, PREC_PAL_GC_MAX(pipe, 2), 1 << 16);
	} else {
		/* FIXME LUT entries are 16 bit only, so we can prog 0xFFFF max */
		intel_dsb_reg_write(crtc_state, PREC_PAL_GC_MAX(pipe, 0), color->red);
		intel_dsb_reg_write(crtc_state, PREC_PAL_GC_MAX(pipe, 1), color->green);
		intel_dsb_reg_write(crtc_state, PREC_PAL_GC_MAX(pipe, 2), color->blue);
	}
#endif
}

static void
icl_program_gamma_superfine_segment(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	const struct drm_property_blob *blob = crtc_state->hw.gamma_lut;
	const struct drm_color_lut *lut = blob->data;
	enum pipe pipe = crtc->pipe;
	int i;

	/*
	 * Program Super Fine segment (let's call it seg1)...
	 *
	 * Super Fine segment's step is 1/(8 * 128 * 256) and it has
	 * 9 entries, corresponding to values 0, 1/(8 * 128 * 256),
	 * 2/(8 * 128 * 256) ... 8/(8 * 128 * 256).
	 */
	intel_dsb_reg_write(crtc_state, PREC_PAL_MULTI_SEG_INDEX(pipe),
			    PAL_PREC_AUTO_INCREMENT);

	for (i = 0; i < 9; i++) {
		const struct drm_color_lut *entry = &lut[i];

		intel_dsb_indexed_reg_write(crtc_state, PREC_PAL_MULTI_SEG_DATA(pipe),
					    ilk_lut_12p4_ldw(entry));
		intel_dsb_indexed_reg_write(crtc_state, PREC_PAL_MULTI_SEG_DATA(pipe),
					    ilk_lut_12p4_udw(entry));
	}
}

static void
icl_program_gamma_multi_segment(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	const struct drm_property_blob *blob = crtc_state->hw.gamma_lut;
	const struct drm_color_lut *lut = blob->data;
	const struct drm_color_lut *entry;
	enum pipe pipe = crtc->pipe;
	int i;

	/*
	 * Program Fine segment (let's call it seg2)...
	 *
	 * Fine segment's step is 1/(128 * 256) i.e. 1/(128 * 256), 2/(128 * 256)
	 * ... 256/(128 * 256). So in order to program fine segment of LUT we
	 * need to pick every 8th entry in the LUT, and program 256 indexes.
	 *
	 * PAL_PREC_INDEX[0] and PAL_PREC_INDEX[1] map to seg2[1],
	 * seg2[0] being unused by the hardware.
	 */
	intel_dsb_reg_write(crtc_state, PREC_PAL_INDEX(pipe),
			    PAL_PREC_AUTO_INCREMENT);
	for (i = 1; i < 257; i++) {
		entry = &lut[i * 8];
		intel_dsb_indexed_reg_write(crtc_state, PREC_PAL_DATA(pipe),
					    ilk_lut_12p4_ldw(entry));
		intel_dsb_indexed_reg_write(crtc_state, PREC_PAL_DATA(pipe),
					    ilk_lut_12p4_udw(entry));
	}

	/*
	 * Program Coarse segment (let's call it seg3)...
	 *
	 * Coarse segment starts from index 0 and it's step is 1/256 ie 0,
	 * 1/256, 2/256 ... 256/256. As per the description of each entry in LUT
	 * above, we need to pick every (8 * 128)th entry in LUT, and
	 * program 256 of those.
	 *
	 * Spec is not very clear about if entries seg3[0] and seg3[1] are
	 * being used or not, but we still need to program these to advance
	 * the index.
	 */
	for (i = 0; i < 256; i++) {
		entry = &lut[i * 8 * 128];
		intel_dsb_indexed_reg_write(crtc_state, PREC_PAL_DATA(pipe),
					    ilk_lut_12p4_ldw(entry));
		intel_dsb_indexed_reg_write(crtc_state, PREC_PAL_DATA(pipe),
					    ilk_lut_12p4_udw(entry));
	}

	/* The last entry in the LUT is to be programmed in GCMAX */
	entry = &lut[256 * 8 * 128];
	icl_load_gcmax(crtc_state, entry);
	ivb_load_lut_ext_max(crtc_state);
}

static void icl_load_luts(const struct intel_crtc_state *crtc_state)
{
	const struct drm_property_blob *gamma_lut = crtc_state->hw.gamma_lut;
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);

	if (crtc_state->hw.degamma_lut)
		glk_load_degamma_lut(crtc_state);

	switch (crtc_state->gamma_mode & GAMMA_MODE_MODE_MASK) {
	case GAMMA_MODE_MODE_8BIT:
		ilk_load_lut_8(crtc, gamma_lut);
		break;
	case GAMMA_MODE_MODE_12BIT_MULTI_SEGMENTED:
		icl_program_gamma_superfine_segment(crtc_state);
		icl_program_gamma_multi_segment(crtc_state);
		break;
	case GAMMA_MODE_MODE_10BIT:
		bdw_load_lut_10(crtc, gamma_lut, PAL_PREC_INDEX_VALUE(0));
		ivb_load_lut_ext_max(crtc_state);
		break;
	default:
		MISSING_CASE(crtc_state->gamma_mode);
		break;
	}

	intel_dsb_commit(crtc_state);
}

#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
static void
xelpd_program_logarithmic_gamma_lut(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	const struct drm_property_blob *blob = crtc_state->hw.gamma_lut;
#ifdef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
	const u32 lut_size = INTEL_INFO(dev_priv)->color.gamma_lut_size;
#else
	u32 lut_size;
#endif
	const struct drm_color_lut *lut;
	enum pipe pipe = crtc->pipe;
	u32 i;

	if (!blob || !blob->data)
		return;

#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
	/*
	 * In case of advance gamma i.e logarithmic, lut size
	 * is 513. Till the new UAPI is merged, we need to have
	 * this s/w WA to allow legacy to co-exist with this.
	 * FixMe: Update once the new UAPI is in place
	 */
	if (crtc_state->uapi.advance_gamma_mode_active)
		lut_size = drm_color_lut_size(blob);
	else
		lut_size = INTEL_INFO(dev_priv)->display.color.gamma_lut_size;
#endif

	lut = blob->data;
	intel_dsb_reg_write(crtc_state, PREC_PAL_INDEX(pipe),
			    PAL_PREC_AUTO_INCREMENT);

	for (i = 0; i < lut_size - 3; i++) {
		intel_dsb_indexed_reg_write(crtc_state, PREC_PAL_DATA(pipe),
					    ilk_lut_12p4_ldw(&lut[i]));
		intel_dsb_indexed_reg_write(crtc_state, PREC_PAL_DATA(pipe),
					    ilk_lut_12p4_udw(&lut[i]));
	}

	icl_load_gcmax(crtc_state, &lut[i]);
	ivb_load_lut_ext_max(crtc_state);
}

static void xelpd_load_luts(const struct intel_crtc_state *crtc_state)
{
	const struct drm_property_blob *gamma_lut = crtc_state->hw.gamma_lut;
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *i915 = to_i915(crtc->base.dev);

	if (crtc_state->hw.degamma_lut) {
		if ((DISPLAY_VER(i915) >= 14))
			mtl_load_degamma_lut(crtc_state);
		else
			glk_load_degamma_lut(crtc_state);
	}

	switch (crtc_state->gamma_mode & GAMMA_MODE_MODE_MASK) {
	case GAMMA_MODE_MODE_8BIT:
		ilk_load_lut_8(crtc, gamma_lut);
		break;
	case GAMMA_MODE_MODE_12BIT_LOGARITHMIC:
		xelpd_program_logarithmic_gamma_lut(crtc_state);
		break;
	default:
		bdw_load_lut_10(crtc, gamma_lut, PAL_PREC_INDEX_VALUE(0));
		ivb_load_lut_ext_max(crtc_state);
	}

	intel_dsb_commit(crtc_state);
}
#endif

#ifndef BPM_DRM_PLANE_ATTACH_CTM_PROPERTY_API_PRESENT
void intel_color_load_plane_csc_matrix(const struct drm_plane_state *plane_state)
{
	struct drm_i915_private *dev_priv = to_i915(plane_state->plane->dev);

	if (dev_priv->color_funcs->load_plane_csc_matrix)
		dev_priv->color_funcs->load_plane_csc_matrix(plane_state);
}

void intel_color_load_plane_luts(const struct drm_plane_state *plane_state)
{
	struct drm_i915_private *dev_priv = to_i915(plane_state->plane->dev);

	if (dev_priv->color_funcs->load_plane_luts)
		dev_priv->color_funcs->load_plane_luts(plane_state);
}
#endif

void intel_color_load_luts(const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(crtc_state->uapi.crtc->dev);

	dev_priv->color_funcs->load_luts(crtc_state);
}

void intel_color_commit_noarm(const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(crtc_state->uapi.crtc->dev);

	if (dev_priv->color_funcs->color_commit_noarm)
		dev_priv->color_funcs->color_commit_noarm(crtc_state);
}

void intel_color_commit_arm(const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(crtc_state->uapi.crtc->dev);

	dev_priv->color_funcs->color_commit_arm(crtc_state);
}

static bool intel_can_preload_luts(const struct intel_crtc_state *new_crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(new_crtc_state->uapi.crtc);
	struct intel_atomic_state *state =
		to_intel_atomic_state(new_crtc_state->uapi.state);
	const struct intel_crtc_state *old_crtc_state =
		intel_atomic_get_old_crtc_state(state, crtc);

	return !old_crtc_state->hw.gamma_lut &&
		!old_crtc_state->hw.degamma_lut;
}

int intel_color_check(struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(crtc_state->uapi.crtc->dev);

	return dev_priv->color_funcs->color_check(crtc_state);
}

void intel_color_get_config(struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(crtc_state->uapi.crtc->dev);

	if (dev_priv->color_funcs->read_luts)
		dev_priv->color_funcs->read_luts(crtc_state);
}

#ifndef BPM_DGLUT_24BIT_MTL_NOT_SUPPORTED
static int check_lut_ext_size(const struct drm_property_blob *lut, int expected)
{
	int len;

	if (!lut)
		return 0;

	len = drm_color_lut_ext_size(lut);
	if (len != expected) {
		DRM_DEBUG_KMS("Invalid LUT size; got %d, expected %d\n",
			      len, expected);
		return -EINVAL;
	}

	return 0;
}
#endif

static int check_lut_size(const struct drm_property_blob *lut, int expected)
{
	int len;

	if (!lut)
		return 0;

	len = drm_color_lut_size(lut);
	if (len != expected) {
		DRM_DEBUG_KMS("Invalid LUT size; got %d, expected %d\n",
			      len, expected);
		return -EINVAL;
	}

	return 0;
}

static int check_luts(const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(crtc_state->uapi.crtc->dev);
	const struct drm_property_blob *gamma_lut = crtc_state->hw.gamma_lut;
	const struct drm_property_blob *degamma_lut = crtc_state->hw.degamma_lut;
	int gamma_length, degamma_length;
	u32 gamma_tests, degamma_tests;

	/* Always allow legacy gamma LUT with no further checking. */
	if (crtc_state_is_legacy_gamma(crtc_state))
		return 0;

	/* C8 relies on its palette being stored in the legacy LUT */
	if (crtc_state->c8_planes) {
		drm_dbg_kms(&dev_priv->drm,
			    "C8 pixelformat requires the legacy LUT\n");
		return -EINVAL;
	}

	degamma_length = INTEL_INFO(dev_priv)->display.color.degamma_lut_size;

#ifdef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
	gamma_length = INTEL_INFO(dev_priv)->display.color.gamma_lut_size;
#else
	/*
	 * In case of advance gamma i.e logarithmic, lut size
	 * is 513. Till the new UAPI is merged, we need to have
	 * this s/w WA to allow legacy to co-exist with this.
	 * FixMe: Update once the new UAPI is in place
	 */
	if (gamma_lut && crtc_state->uapi.advance_gamma_mode_active)
		gamma_length = drm_color_lut_size(gamma_lut);
	else
		gamma_length = INTEL_INFO(dev_priv)->display.color.gamma_lut_size;
#endif

	degamma_tests = INTEL_INFO(dev_priv)->display.color.degamma_lut_tests;
	gamma_tests = INTEL_INFO(dev_priv)->display.color.gamma_lut_tests;

#ifdef BPM_DGLUT_24BIT_MTL_NOT_SUPPORTED
	if (check_lut_size(degamma_lut, degamma_length) ||
	    check_lut_size(gamma_lut, gamma_length))
		return -EINVAL;

	if (drm_color_lut_check(degamma_lut, degamma_tests) ||
	    drm_color_lut_check(gamma_lut, gamma_tests))
		return -EINVAL;
#else
	if (check_lut_size(gamma_lut, gamma_length) ||
	    drm_color_lut_check(gamma_lut, gamma_tests))
		return -EINVAL;

	/* If extended degamma property set*/
	if (crtc_state->uapi.advance_degamma_mode_active) {
		if (check_lut_ext_size(degamma_lut, degamma_length) ||
		    drm_color_lut_ext_check(degamma_lut, degamma_tests))
			return -EINVAL;
	} else {
		if (check_lut_size(degamma_lut, degamma_length) ||
		    drm_color_lut_check(degamma_lut, degamma_tests))
			return -EINVAL;
	}
#endif

	return 0;
}

#ifndef BPM_DGLUT_24BIT_MTL_NOT_SUPPORTED
static int mtl_check_degamma_lut(const struct intel_crtc_state *crtc_state)
{
	const struct drm_property_blob *degamma_lut_blob = crtc_state->hw.gamma_lut;

	if (!degamma_lut_blob)
		return 0;

	if (crtc_state->uapi.degamma_mode_type == DEGAMMA_MODE_24BIT &&
	    crtc_state->uapi.advance_degamma_mode_active)
		return 0;

	/* 16 bit LUT value usecase */
	if (crtc_state->uapi.degamma_mode_type == 0)
		return 0;

	DRM_ERROR("%s check failed\n", __func__);

	return -EINVAL;
}
#endif

static u32 icl_gamma_mode(const struct intel_crtc_state *crtc_state)
{
#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
	struct drm_i915_private *i915 = to_i915(crtc_state->uapi.crtc->dev);
#endif
	u32 gamma_mode = 0;

	if (crtc_state->hw.degamma_lut)
		gamma_mode |= PRE_CSC_GAMMA_ENABLE;

	if (crtc_state->hw.gamma_lut &&
	    !crtc_state->c8_planes)
		gamma_mode |= POST_CSC_GAMMA_ENABLE;

#ifdef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
	if (!crtc_state->hw.gamma_lut ||
	    crtc_state_is_legacy_gamma(crtc_state))
		gamma_mode |= GAMMA_MODE_MODE_8BIT;
	else
		gamma_mode |= GAMMA_MODE_MODE_12BIT_MULTI_SEGMENTED;
#else
	if (!crtc_state->hw.gamma_lut ||
	    crtc_state_is_legacy_gamma(crtc_state)) {
		gamma_mode |= GAMMA_MODE_MODE_8BIT;
	} else if (DISPLAY_VER(i915) >= 13) {
		if (crtc_state->uapi.gamma_mode_type ==
				GAMMA_MODE_LOGARITHMIC_12BIT &&
				crtc_state->uapi.advance_gamma_mode_active)
			gamma_mode |= GAMMA_MODE_MODE_12BIT_LOGARITHMIC;
		else
			gamma_mode |= GAMMA_MODE_MODE_10BIT;
	} else {
		gamma_mode |= GAMMA_MODE_MODE_12BIT_MULTI_SEGMENTED;
	}
#endif

	return gamma_mode;
}

static u32 icl_csc_mode(const struct intel_crtc_state *crtc_state)
{
	u32 csc_mode = 0;

	if (crtc_state->hw.ctm)
		csc_mode |= ICL_CSC_ENABLE;

	if (crtc_state->output_format != INTEL_OUTPUT_FORMAT_RGB ||
	    crtc_state->limited_color_range)
		csc_mode |= ICL_OUTPUT_CSC_ENABLE;

	return csc_mode;
}

static u32 dither_after_cc1_12bpc(const struct intel_crtc_state *crtc_state)
{
	u32 gamma_mode = crtc_state->gamma_mode;
	struct drm_i915_private *i915 = to_i915(crtc_state->uapi.crtc->dev);

	if (DISPLAY_VER(i915) >= 13) {
		if (!crtc_state->dither_force_disable &&
		    (crtc_state->pipe_bpp == 36))
			gamma_mode |= GAMMA_MODE_DITHER_AFTER_CC1;
	}

	return gamma_mode;
}

static int icl_color_check(struct intel_crtc_state *crtc_state)
{
#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
	struct drm_device *dev = crtc_state->uapi.crtc->dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
#endif
#ifndef BPM_DGLUT_24BIT_MTL_NOT_SUPPORTED
	struct drm_property *gamma_mode_property = crtc_state->uapi.crtc->gamma_mode_property;
	struct drm_property *degamma_mode_property = crtc_state->uapi.crtc->degamma_mode_property;
#endif
#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
	struct drm_property_enum *prop_enum;
	u32 index = 0;
#endif
	int ret;

	ret = check_luts(crtc_state);
	if (ret)
		return ret;

#ifndef BPM_DGLUT_24BIT_MTL_NOT_SUPPORTED
	if (DISPLAY_VER(dev_priv) >= 14) {
		list_for_each_entry(prop_enum, &degamma_mode_property->enum_list, head) {
			if (prop_enum->value == crtc_state->uapi.degamma_mode) {
				if (!strcmp(prop_enum->name,
					    "extended degamma")) {
					crtc_state->uapi.degamma_mode_type =
						DEGAMMA_MODE_24BIT;
					drm_dbg_kms(dev,
						    "extended degamma enabled\n");
				} else {
					crtc_state->uapi.degamma_mode_type = 0;
					drm_dbg_kms(dev,
						    "extended degamma disabled\n");
				}
				break;
			}
		}

		ret = mtl_check_degamma_lut(crtc_state);
		if (ret)
			return ret;
	}
#endif

#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
	if (DISPLAY_VER(dev_priv) >= 13) {
		list_for_each_entry(prop_enum, &gamma_mode_property->enum_list, head) {
			if (prop_enum->value == crtc_state->uapi.gamma_mode) {
				if (!strcmp(prop_enum->name,
					    "logarithmic gamma")) {
					crtc_state->uapi.gamma_mode_type =
						GAMMA_MODE_LOGARITHMIC_12BIT;
					drm_dbg_kms(dev,
						    "logarithmic gamma enabled\n");
				}
				break;
			}
			index++;
		}
	}
#endif

	crtc_state->gamma_mode = icl_gamma_mode(crtc_state);

	crtc_state->gamma_mode = dither_after_cc1_12bpc(crtc_state);

	crtc_state->csc_mode = icl_csc_mode(crtc_state);

	crtc_state->preload_luts = intel_can_preload_luts(crtc_state);

	return 0;
}

static int icl_gamma_precision(const struct intel_crtc_state *crtc_state)
{
	if ((crtc_state->gamma_mode & POST_CSC_GAMMA_ENABLE) == 0)
		return 0;

	switch (crtc_state->gamma_mode & GAMMA_MODE_MODE_MASK) {
	case GAMMA_MODE_MODE_8BIT:
		return 8;
	case GAMMA_MODE_MODE_10BIT:
		return 10;
	case GAMMA_MODE_MODE_12BIT_MULTI_SEGMENTED:
		return 16;
	default:
		MISSING_CASE(crtc_state->gamma_mode);
		return 0;
	}
}

int intel_color_get_gamma_bit_precision(const struct intel_crtc_state *crtc_state)
{
	return icl_gamma_precision(crtc_state);
}

static bool err_check(struct drm_color_lut *lut1,
		      struct drm_color_lut *lut2, u32 err)
{
	return ((abs((long)lut2->red - lut1->red)) <= err) &&
		((abs((long)lut2->blue - lut1->blue)) <= err) &&
		((abs((long)lut2->green - lut1->green)) <= err);
}

static bool intel_color_lut_entries_equal(struct drm_color_lut *lut1,
					  struct drm_color_lut *lut2,
					  int lut_size, u32 err)
{
	int i;

	for (i = 0; i < lut_size; i++) {
		if (!err_check(&lut1[i], &lut2[i], err))
			return false;
	}

	return true;
}

bool intel_color_lut_equal(struct drm_property_blob *blob1,
			   struct drm_property_blob *blob2,
			   u32 gamma_mode, u32 bit_precision)
{
	struct drm_color_lut *lut1, *lut2;
	int lut_size1, lut_size2;
	u32 err;

	if (!blob1 != !blob2)
		return false;

	if (!blob1)
		return true;

	lut_size1 = drm_color_lut_size(blob1);
	lut_size2 = drm_color_lut_size(blob2);

	/* check sw and hw lut size */
	if (lut_size1 != lut_size2)
		return false;

	lut1 = blob1->data;
	lut2 = blob2->data;

	err = 0xffff >> bit_precision;

	/* check sw and hw lut entry to be equal */
	switch (gamma_mode & GAMMA_MODE_MODE_MASK) {
	case GAMMA_MODE_MODE_8BIT:
	case GAMMA_MODE_MODE_10BIT:
		if (!intel_color_lut_entries_equal(lut1, lut2,
						   lut_size2, err))
			return false;
		break;
	case GAMMA_MODE_MODE_12BIT_MULTI_SEGMENTED:
		if (!intel_color_lut_entries_equal(lut1, lut2,
						   9, err))
			return false;
		break;
	default:
		MISSING_CASE(gamma_mode);
		return false;
	}

	return true;
}

/* On BDW+ the index auto increment mode actually works */
static struct drm_property_blob *bdw_read_lut_10(struct intel_crtc *crtc,
						 u32 prec_index)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	int i, hw_lut_size = ivb_lut_10_size(prec_index);
	int lut_size = INTEL_INFO(dev_priv)->display.color.gamma_lut_size;
	enum pipe pipe = crtc->pipe;
	struct drm_property_blob *blob;
	struct drm_color_lut *lut;

	drm_WARN_ON(&dev_priv->drm, lut_size != hw_lut_size);

	blob = drm_property_create_blob(&dev_priv->drm,
					sizeof(struct drm_color_lut) * lut_size,
					NULL);
	if (IS_ERR(blob))
		return NULL;

	lut = blob->data;

	intel_de_write_fw(dev_priv, PREC_PAL_INDEX(pipe),
			  prec_index | PAL_PREC_AUTO_INCREMENT);

	for (i = 0; i < lut_size; i++) {
		u32 val = intel_de_read_fw(dev_priv, PREC_PAL_DATA(pipe));

		ilk_lut_10_pack(&lut[i], val);
	}

	intel_de_write_fw(dev_priv, PREC_PAL_INDEX(pipe), 0);

	return blob;
}

static struct drm_property_blob *
icl_read_lut_multi_segment(struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	int i, lut_size = INTEL_INFO(dev_priv)->display.color.gamma_lut_size;
	enum pipe pipe = crtc->pipe;
	struct drm_property_blob *blob;
	struct drm_color_lut *lut;

	blob = drm_property_create_blob(&dev_priv->drm,
					sizeof(struct drm_color_lut) * lut_size,
					NULL);
	if (IS_ERR(blob))
		return NULL;

	lut = blob->data;

	intel_de_write_fw(dev_priv, PREC_PAL_MULTI_SEG_INDEX(pipe),
			  PAL_PREC_AUTO_INCREMENT);

	for (i = 0; i < 9; i++) {
		u32 ldw = intel_de_read_fw(dev_priv, PREC_PAL_MULTI_SEG_DATA(pipe));
		u32 udw = intel_de_read_fw(dev_priv, PREC_PAL_MULTI_SEG_DATA(pipe));

		icl_lut_multi_seg_pack(&lut[i], ldw, udw);
	}

	intel_de_write_fw(dev_priv, PREC_PAL_MULTI_SEG_INDEX(pipe), 0);

	/*
	 * FIXME readouts from PAL_PREC_DATA register aren't giving
	 * correct values in the case of fine and coarse segments.
	 * Restricting readouts only for super fine segment as of now.
	 */

	return blob;
}

static struct drm_property_blob *ilk_read_lut_8(struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum pipe pipe = crtc->pipe;
	struct drm_property_blob *blob;
	struct drm_color_lut *lut;
	int i;

	blob = drm_property_create_blob(&dev_priv->drm,
					sizeof(struct drm_color_lut) * LEGACY_LUT_LENGTH,
					NULL);
	if (IS_ERR(blob))
		return NULL;

	lut = blob->data;

	for (i = 0; i < LEGACY_LUT_LENGTH; i++) {
		u32 val = intel_de_read_fw(dev_priv, LGC_PALETTE(pipe, i));

		i9xx_lut_8_pack(&lut[i], val);
	}

	return blob;
}

static void icl_read_luts(struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);

	if ((crtc_state->gamma_mode & POST_CSC_GAMMA_ENABLE) == 0)
		return;

	switch (crtc_state->gamma_mode & GAMMA_MODE_MODE_MASK) {
	case GAMMA_MODE_MODE_8BIT:
		crtc_state->hw.gamma_lut = ilk_read_lut_8(crtc);
		break;
	case GAMMA_MODE_MODE_10BIT:
		crtc_state->hw.gamma_lut = bdw_read_lut_10(crtc, PAL_PREC_INDEX_VALUE(0));
		break;
	case GAMMA_MODE_MODE_12BIT_MULTI_SEGMENTED:
		crtc_state->hw.gamma_lut = icl_read_lut_multi_segment(crtc);
		break;
	default:
		MISSING_CASE(crtc_state->gamma_mode);
		break;
	}
}

#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
static void xelpd_lut_logarithmic_pack(struct drm_color_lut *entry,
				       u32 ldw, u32 udw)
{
	entry->red = REG_FIELD_GET(PAL_PREC_LOGARITHMIC_RED_UDW_MASK, udw) << 6 |
				   REG_FIELD_GET(PAL_PREC_LOGARITHMIC_RED_LDW_MASK, ldw);
	entry->green = REG_FIELD_GET(PAL_PREC_LOGARITHMIC_GREEN_UDW_MASK, udw) << 6 |
				     REG_FIELD_GET(PAL_PREC_LOGARITHMIC_GREEN_LDW_MASK, ldw);
	entry->blue = REG_FIELD_GET(PAL_PREC_LOGARITHMIC_BLUE_UDW_MASK, udw) << 6 |
				    REG_FIELD_GET(PAL_PREC_LOGARITHMIC_BLUE_LDW_MASK, ldw);
}

static struct drm_property_blob *
xelpd_read_lut_logarithmic(struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
#ifdef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
	int i, lut_size = INTEL_INFO(dev_priv)->display.color.gamma_lut_size;
#else
	struct intel_crtc_state *crtc_state =
		to_intel_crtc_state(crtc->base.state);
	const struct drm_property_blob *gamma_lut = crtc_state->hw.gamma_lut;
	int i, lut_size;
#endif
	enum pipe pipe = crtc->pipe;
	struct drm_property_blob *blob;
	struct drm_color_lut *lut;
	u32 gamma_max_val = 0xFFFF;

#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
	/*
	 * In case of advance gamma i.e logarithmic, lut size
	 * is 513. Till the new UAPI is merged, we need to have
	 * this s/w WA to allow legacy to co-exist with this.
	 * FixMe: Update once the new UAPI is in place
	 */
	if (crtc_state->uapi.advance_gamma_mode_active)
		lut_size = drm_color_lut_size(gamma_lut);
	else
		lut_size = INTEL_INFO(dev_priv)->display.color.gamma_lut_size;

#endif
	blob = drm_property_create_blob(&dev_priv->drm,
					sizeof(struct drm_color_lut) * lut_size,
					NULL);
	if (IS_ERR(blob))
		return NULL;

	lut = blob->data;

	intel_de_write(dev_priv, PREC_PAL_INDEX(pipe),
		       PAL_PREC_AUTO_INCREMENT);

	for (i = 0; i < lut_size - 3; i++) {
		u32 ldw = intel_de_read(dev_priv, PREC_PAL_DATA(pipe));
		u32 udw = intel_de_read(dev_priv, PREC_PAL_DATA(pipe));

		xelpd_lut_logarithmic_pack(&lut[i], ldw, udw);
	}

	/* All the extended ranges are now limited to last value of 1.0 */
	while (i < lut_size) {
		lut[i].red = gamma_max_val;
		lut[i].green = gamma_max_val;
		lut[i].blue = gamma_max_val;
		i++;
	};

	intel_de_write(dev_priv, PREC_PAL_INDEX(pipe), 0);

	return blob;
}

static void xelpd_read_luts(struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);

	if ((crtc_state->gamma_mode & POST_CSC_GAMMA_ENABLE) == 0)
		return;

	switch (crtc_state->gamma_mode & GAMMA_MODE_MODE_MASK) {
	case GAMMA_MODE_MODE_8BIT:
		crtc_state->hw.gamma_lut = ilk_read_lut_8(crtc);
		break;
	case GAMMA_MODE_MODE_12BIT_LOGARITHMIC:
		crtc_state->hw.gamma_lut = xelpd_read_lut_logarithmic(crtc);
		break;
	default:
		crtc_state->hw.gamma_lut = bdw_read_lut_10(crtc, PAL_PREC_INDEX_VALUE(0));
	}
}
#endif

#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
#define XELPD_GAMMA_CAPABILITY_FLAG	(DRM_MODE_LUT_GAMMA | \
					 DRM_MODE_LUT_REFLECT_NEGATIVE | \
					 DRM_MODE_LUT_INTERPOLATE | \
					 DRM_MODE_LUT_NON_DECREASING)
 /* FIXME input bpc? */
static const struct drm_color_lut_range xelpd_logarithmic_gamma[] = {
	/* segment 0 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 1,
		.input_bpc = 24, .output_bpc = 16,
		.start = 0, .end = 0,
		.min = 0, .max = 0,
	},
	/* segment 1 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 1,
		.input_bpc = 24, .output_bpc = 16,
		.start = 0, .end = (1 << 0),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 2 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 2,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 0), .end = (1 << 1),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 3 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 2,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 1), .end = (1 << 2),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 4 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 2,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 2), .end = (1 << 3),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 5 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 2,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 3), .end = (1 << 4),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 6 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 4,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 4), .end = (1 << 5),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 7 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 4,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 5), .end = (1 << 6),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 8 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 4,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 6), .end = (1 << 7),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 9 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 8,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 7), .end = (1 << 8),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 10 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 8,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 8), .end = (1 << 9),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 11 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 8,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 9), .end = (1 << 10),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 12 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 16,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 10), .end = (1 << 11),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 13 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 16,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 11), .end = (1 << 12),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 14 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 16,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 12), .end = (1 << 13),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 15 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 32,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 13), .end = (1 << 14),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 16 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 32,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 14), .end = (1 << 15),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 17 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 64,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 15), .end = (1 << 16),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 18 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 64,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 16), .end = (1 << 17),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 19 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 64,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 17), .end = (1 << 18),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 20 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 32,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 18), .end = (1 << 19),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 21 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 32,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 19), .end = (1 << 20),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 22 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 32,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 20), .end = (1 << 21),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 23 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 32,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 21), .end = (1 << 22),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 24 */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG,
		.count = 32,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 22), .end = (1 << 23),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 3 aka. coarse segment / PAL_GC_MAX */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG | DRM_MODE_LUT_REUSE_LAST,
		.count = 1,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 24), .end = (3 << 24),
		.min = 0, .max = 1 << 16,
	},
	/* PAL_EXT_GC_MAX */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG | DRM_MODE_LUT_REUSE_LAST,
		.count = 1,
		.input_bpc = 24, .output_bpc = 16,
		.start = (3 << 24), .end = (7 << 24),
		.min = 0, .max = (8 << 16) - 1,
	},
	/* PAL_EXT2_GC_MAX */
	{
		.flags = XELPD_GAMMA_CAPABILITY_FLAG | DRM_MODE_LUT_REUSE_LAST,
		.count = 1,
		.input_bpc = 24, .output_bpc = 16,
		.start = (7 << 24), .end = (7 << 24),
		.min = 0, .max = (8 << 16) - 1,
	},
};
#endif

#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
static void xelpd_program_plane_degamma_lut(const struct drm_plane_state *state,
					    struct drm_color_lut_ext *degamma_lut,
					    u32 offset)
{
	struct drm_i915_private *dev_priv = to_i915(state->plane->dev);
	enum pipe pipe = to_intel_plane(state->plane)->pipe;
	enum plane_id plane = to_intel_plane(state->plane)->id;
	u32 i, lut_size;

	if (icl_is_hdr_plane(dev_priv, plane)) {
		lut_size = 128;

		intel_de_write_fw(dev_priv, PLANE_PRE_CSC_GAMC_INDEX_ENH(pipe, plane, 0),
				  PLANE_PAL_PREC_AUTO_INCREMENT);

		if (degamma_lut) {
			for (i = 0; i < lut_size; i++) {
				u64 word = drm_color_lut_extract_ext(degamma_lut[i].green, 24);
				u32 lut_val = (word & 0xffffff);

				intel_de_write_fw(dev_priv, PLANE_PRE_CSC_GAMC_DATA_ENH(pipe, plane, 0),
						  lut_val);
			}

			/* Program the max register to clamp values > 1.0. */
			while (i < 131)
				intel_de_write_fw(dev_priv, PLANE_PRE_CSC_GAMC_DATA_ENH(pipe, plane, 0),
						  degamma_lut[i++].green);
		} else {
			for (i = 0; i < lut_size; i++) {
				u32 v = (i * ((1 << 24) - 1)) / (lut_size - 1);

				intel_de_write_fw(dev_priv, PLANE_PRE_CSC_GAMC_DATA_ENH(pipe, plane, 0), v);
			}

			do {
				intel_de_write_fw(dev_priv, PLANE_PRE_CSC_GAMC_DATA_ENH(pipe, plane, 0),
						  1 << 24);
			} while (i++ < 130);
		}

		intel_de_write_fw(dev_priv, PLANE_PRE_CSC_GAMC_INDEX_ENH(pipe, plane, 0), 0);
	} else {
		lut_size = 32;

		/*
		 * First 3 planes are HDR, so reduce by 3 to get to the right
		 * SDR plane offset
		 */
		plane = plane - 3;

		intel_de_write_fw(dev_priv, PLANE_PRE_CSC_GAMC_INDEX(pipe, plane, 0),
				  PLANE_PAL_PREC_AUTO_INCREMENT);

		if (degamma_lut) {
			for (i = 0; i < lut_size; i++)
				intel_de_write_fw(dev_priv, PLANE_PRE_CSC_GAMC_DATA(pipe, plane, 0),
						  degamma_lut[i].green);
			/* Program the max register to clamp values > 1.0. */
			while (i < 35)
				intel_de_write_fw(dev_priv, PLANE_PRE_CSC_GAMC_DATA(pipe, plane, 0),
						  degamma_lut[i++].green);
		} else {
			for (i = 0; i < lut_size; i++) {
				u32 v = (i * ((1 << 16) - 1)) / (lut_size - 1);

				intel_de_write_fw(dev_priv, PLANE_PRE_CSC_GAMC_DATA(pipe, plane, 0), v);
			}

			do {
				intel_de_write_fw(dev_priv, PLANE_PRE_CSC_GAMC_DATA(pipe, plane, 0),
						  1 << 16);
			} while (i++ < 34);
		}

		intel_de_write_fw(dev_priv, PLANE_PRE_CSC_GAMC_INDEX(pipe, plane, 0), 0);
	}
}

static void xelpd_program_plane_gamma_lut(const struct drm_plane_state *state,
					  struct drm_color_lut_ext *gamma_lut,
					  u32 offset)
{
	struct drm_i915_private *dev_priv = to_i915(state->plane->dev);
	enum pipe pipe = to_intel_plane(state->plane)->pipe;
	enum plane_id plane = to_intel_plane(state->plane)->id;
	u32 i, lut_size;

	if (icl_is_hdr_plane(dev_priv, plane)) {
		intel_de_write_fw(dev_priv, PLANE_POST_CSC_GAMC_INDEX_ENH(pipe, plane, 0),
				  offset | PLANE_PAL_PREC_AUTO_INCREMENT);
		if (gamma_lut) {
			lut_size = 32;
			for (i = 0; i < lut_size; i++) {
				u64 word = drm_color_lut_extract_ext(gamma_lut[i].green, 24);
				u32 lut_val = (word & 0xffffff);

				intel_de_write_fw(dev_priv, PLANE_POST_CSC_GAMC_DATA_ENH(pipe, plane, 0),
						  lut_val);
			}

			do {
				/* Program the max register to clamp values > 1.0. */
				intel_de_write_fw(dev_priv, PLANE_POST_CSC_GAMC_DATA_ENH(pipe, plane, 0),
						  gamma_lut[i].green);
			} while (i++ < 34);
		} else {
			lut_size = 32;
			for (i = 0; i < lut_size; i++) {
				u32 v = (i * ((1 << 24) - 1)) / (lut_size - 1);

				intel_de_write_fw(dev_priv, PLANE_POST_CSC_GAMC_DATA_ENH(pipe, plane, 0), v);
			}

			do {
				intel_de_write_fw(dev_priv, PLANE_POST_CSC_GAMC_DATA_ENH(pipe, plane, 0),
						  1 << 24);
			} while (i++ < 34);
		}

		intel_de_write_fw(dev_priv, PLANE_POST_CSC_GAMC_INDEX_ENH(pipe, plane, 0), 0);
	} else {
		lut_size = 32;
		/*
		 * First 3 planes are HDR, so reduce by 3 to get to the right
		 * SDR plane offset
		 */
		plane = plane - 3;

		intel_de_write_fw(dev_priv, PLANE_POST_CSC_GAMC_INDEX(pipe, plane, 0),
				  offset | PLANE_PAL_PREC_AUTO_INCREMENT);

		if (gamma_lut) {
			for (i = 0; i < lut_size; i++)
				intel_de_write_fw(dev_priv, PLANE_POST_CSC_GAMC_DATA(pipe, plane, 0),
						  gamma_lut[i].green & 0xffff);
			/* Program the max register to clamp values > 1.0. */
			while (i < 35)
				intel_de_write_fw(dev_priv, PLANE_POST_CSC_GAMC_DATA(pipe, plane, 0),
						  gamma_lut[i++].green & 0x3ffff);
		} else {
			for (i = 0; i < lut_size; i++) {
				u32 v = (i * ((1 << 16) - 1)) / (lut_size - 1);

				intel_de_write_fw(dev_priv, PLANE_POST_CSC_GAMC_DATA(pipe, plane, 0), v);
			}

			do {
				intel_de_write_fw(dev_priv, PLANE_POST_CSC_GAMC_DATA(pipe, plane, 0),
						  (1 << 16));
			} while (i++ < 34);
		}

		intel_de_write_fw(dev_priv, PLANE_POST_CSC_GAMC_INDEX(pipe, plane, 0), 0);
	}
}
#endif

#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
static void xelpd_plane_load_luts(const struct drm_plane_state *plane_state)
{
	const struct drm_property_blob *degamma_lut_blob =
					plane_state->degamma_lut;
#ifdef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
	struct drm_color_lut_ext *degamma_lut = NULL;
#else
	const struct drm_property_blob *gamma_lut_blob =
					plane_state->gamma_lut;
	struct drm_color_lut_ext *degamma_lut, *gamma_lut;
#endif

	if (degamma_lut_blob) {
		degamma_lut = degamma_lut_blob->data;
		xelpd_program_plane_degamma_lut(plane_state, degamma_lut, 0);
	}

#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
	if (gamma_lut_blob) {
		gamma_lut = gamma_lut_blob->data;
		xelpd_program_plane_gamma_lut(plane_state, gamma_lut, 0);
	}
#endif
}
#endif

#ifndef BPM_DRM_PLANE_ATTACH_CTM_PROPERTY_API_PRESENT
static void xelpd_load_plane_csc_matrix(const struct drm_plane_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->plane->dev);
	enum pipe pipe = to_intel_plane(state->plane)->pipe;
	enum plane_id plane = to_intel_plane(state->plane)->id;
	struct drm_color_ctm *ctm;
	const u64 *input;
	u16 coeffs[9] = {};
	u16 postoff = 0;
	int i;

	if (!icl_is_hdr_plane(dev_priv, plane) || !state->ctm)
		return;

	ctm = state->ctm->data;
	input = ctm->matrix;

	/*
	 * Convert fixed point S31.32 input to format supported by the
	 * hardware.
	 */
	for (i = 0; i < ARRAY_SIZE(coeffs); i++) {
		u64 abs_coeff = ((1ULL << 63) - 1) & input[i];

		/*
		 * Clamp input value to min/max supported by
		 * hardware.
		 */
		abs_coeff = clamp_val(abs_coeff, 0, CTM_COEFF_4_0 - 1);

		/* sign bit */
		if (CTM_COEFF_NEGATIVE(input[i]))
			coeffs[i] |= 1 << 15;

		if (abs_coeff < CTM_COEFF_0_125)
			coeffs[i] |= (3 << 12) |
				ILK_CSC_COEFF_FP(abs_coeff, 12);
		else if (abs_coeff < CTM_COEFF_0_25)
			coeffs[i] |= (2 << 12) |
				ILK_CSC_COEFF_FP(abs_coeff, 11);
		else if (abs_coeff < CTM_COEFF_0_5)
			coeffs[i] |= (1 << 12) |
				ILK_CSC_COEFF_FP(abs_coeff, 10);
		else if (abs_coeff < CTM_COEFF_1_0)
			coeffs[i] |= ILK_CSC_COEFF_FP(abs_coeff, 9);
		else if (abs_coeff < CTM_COEFF_2_0)
			coeffs[i] |= (7 << 12) |
				ILK_CSC_COEFF_FP(abs_coeff, 8);
		else
			coeffs[i] |= (6 << 12) |
				ILK_CSC_COEFF_FP(abs_coeff, 7);
	}

	intel_de_write_fw(dev_priv, PLANE_CSC_COEFF(pipe, plane, 0),
			  coeffs[0] << 16 | coeffs[1]);
	intel_de_write_fw(dev_priv, PLANE_CSC_COEFF(pipe, plane, 1),
			  coeffs[2] << 16);

	intel_de_write_fw(dev_priv, PLANE_CSC_COEFF(pipe, plane, 2),
			  coeffs[3] << 16 | coeffs[4]);
	intel_de_write_fw(dev_priv, PLANE_CSC_COEFF(pipe, plane, 3),
			  coeffs[5] << 16);

	intel_de_write_fw(dev_priv, PLANE_CSC_COEFF(pipe, plane, 4),
			  coeffs[6] << 16 | coeffs[7]);
	intel_de_write_fw(dev_priv, PLANE_CSC_COEFF(pipe, plane, 5),
			  coeffs[8] << 16);

	intel_de_write_fw(dev_priv, PLANE_CSC_PREOFF(pipe, plane, 0), 0);
	intel_de_write_fw(dev_priv, PLANE_CSC_PREOFF(pipe, plane, 1), 0);
	intel_de_write_fw(dev_priv, PLANE_CSC_PREOFF(pipe, plane, 2), 0);

	intel_de_write_fw(dev_priv, PLANE_CSC_POSTOFF(pipe, plane, 0), postoff);
	intel_de_write_fw(dev_priv, PLANE_CSC_POSTOFF(pipe, plane, 1), postoff);
	intel_de_write_fw(dev_priv, PLANE_CSC_POSTOFF(pipe, plane, 2), postoff);
}
#endif

#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
static const struct intel_color_funcs xelpd_color_funcs = {
	.color_check = icl_color_check,
	.color_commit_noarm = icl_color_commit_noarm,
	.color_commit_arm = skl_color_commit_arm,
	.load_luts = xelpd_load_luts,
#ifdef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
	.read_luts = icl_read_luts,
#else
	.read_luts = xelpd_read_luts,
#endif
#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
	.load_plane_luts = xelpd_plane_load_luts,
#endif
#ifndef BPM_DRM_PLANE_ATTACH_CTM_PROPERTY_API_PRESENT
	.load_plane_csc_matrix = xelpd_load_plane_csc_matrix,
#endif
};
#endif

static const struct intel_color_funcs icl_color_funcs = {
	.color_check = icl_color_check,
	.color_commit_noarm = icl_color_commit_noarm,
	.color_commit_arm = skl_color_commit_arm,
	.load_luts = icl_load_luts,
	.read_luts = icl_read_luts,
};

#ifdef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
__maybe_unused
#endif
#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
 /* FIXME input bpc? */
static const struct drm_color_lut_range xelpd_degamma_hdr[] = {
	/* segment 1 */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 128,
		.input_bpc = 24, .output_bpc = 16,
		.start = 0, .end = (1 << 24) - 1,
		.min = 0, .max = (1 << 24) - 1,
	},
	/* segment 2 */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 24) - 1, .end = 1 << 24,
		.min = 0, .max = (1 << 27) - 1,
	},
	/* Segment 3 */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 24, .output_bpc = 16,
		.start = 1 << 24, .end = 3 << 24,
		.min = 0, .max = (1 << 27) - 1,
	},
	/* Segment 4 */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 24, .output_bpc = 16,
		.start = 3 << 24, .end = 7 << 24,
		.min = 0, .max = (1 << 27) - 1,
	},
};
#endif

 /* FIXME input bpc? */
#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
static const struct drm_color_lut_range xelpd_gamma_degamma_sdr[] = {
	/* segment 1 */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 32,
		.input_bpc = 16, .output_bpc = 16,
		.start = 0, .end = (1 << 16) - (1 << 16) / 33,
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 2 */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 16, .output_bpc = 16,
		.start = (1 << 16) - (1 << 16) / 33, .end = 1 << 16,
		.min = 0, .max = 1 << 16,
	},
	/* Segment 3 */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 16, .output_bpc = 16,
		.start = 1 << 16, .end = 3 << 16,
		.min = 0, .max = (8 << 16) - 1,
	},
	/* Segment 4 */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 16, .output_bpc = 16,
		.start = 3 << 16, .end = 7 << 16,
		.min = 0, .max = (8 << 16) - 1,
	},
};

 /* FIXME input bpc? */
static const struct drm_color_lut_range xelpd_gamma_hdr[] = {
	/*
	 * ToDo: Add Segment 1
	 * There is an optional fine segment added with 9 lut values
	 * Will be added later
	 */

	/* segment 2 */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 32,
		.input_bpc = 24, .output_bpc = 16,
		.start = 0, .end = (1 << 24) - 1,
		.min = 0, .max = (1 << 24) - 1,
	},
	/* segment 3 */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 24) - 1, .end = 1 << 24,
		.min = 0, .max = 1 << 24,
	},
	/* Segment 4 */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 24, .output_bpc = 16,
		.start = 1 << 24, .end = 3 << 24,
		.min = 0, .max = (3 << 24),
	},
	/* Segment 5 */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 24, .output_bpc = 16,
		.start = 3 << 24, .end = 7 << 24,
		.min = 0, .max = (7 << 24),
	},
};
#endif

#ifndef BPM_DGLUT_24BIT_MTL_NOT_SUPPORTED
static const struct drm_color_lut_range mtl_24bit_degamma[] = {
	/* segment 0 */
	{
		.flags = (DRM_MODE_LUT_DEGAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 128,
		.input_bpc = 24, .output_bpc = 16,
		.start = 0, .end = (1 << 24) - 1,
		.min = 0, .max = (1 << 24) - 1,
	}
};
#endif

#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
int intel_color_plane_init(struct drm_plane *plane)
{
	struct drm_i915_private *dev_priv = to_i915(plane->dev);
	int ret = 0;

	if (DISPLAY_VER(dev_priv) >= 13) {
		drm_plane_create_color_mgmt_properties(plane->dev, plane, 2);
		ret = drm_plane_color_add_gamma_degamma_mode_range(plane, "no degamma",
								   NULL, 0,
								   LUT_TYPE_DEGAMMA);
		if (ret)
			return ret;

#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
		ret = drm_plane_color_add_gamma_degamma_mode_range(plane, "no gamma",
								   NULL, 0,
								   LUT_TYPE_GAMMA);
		if (ret)
			return ret;
#endif

#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
		if (icl_is_hdr_plane(dev_priv, to_intel_plane(plane)->id)) {
			ret = drm_plane_color_add_gamma_degamma_mode_range(plane, "plane degamma",
									   xelpd_degamma_hdr,
									   sizeof(xelpd_degamma_hdr),
									   LUT_TYPE_DEGAMMA);
			if (ret)
				return ret;

			ret = drm_plane_color_add_gamma_degamma_mode_range(plane, "plane gamma",
									   xelpd_gamma_hdr,
									   sizeof(xelpd_gamma_hdr),
									   LUT_TYPE_GAMMA);
			if (ret)
				return ret;
		} else {
			ret = drm_plane_color_add_gamma_degamma_mode_range(plane, "plane degamma",
									   xelpd_gamma_degamma_sdr,
									   sizeof(xelpd_gamma_degamma_sdr),
									   LUT_TYPE_DEGAMMA);
			if (ret)
				return ret;

			ret = drm_plane_color_add_gamma_degamma_mode_range(plane, "plane gamma",
									   xelpd_gamma_degamma_sdr,
									   sizeof(xelpd_gamma_degamma_sdr),
									   LUT_TYPE_GAMMA);
			if (ret)
				return ret;
		}
	}
#else
	ret = drm_plane_color_add_gamma_degamma_mode_range(plane, "plane degamma",
							   xelpd_degamma_hdr,
							   sizeof(xelpd_degamma_hdr),
							   LUT_TYPE_DEGAMMA);
#endif

#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
	drm_plane_attach_degamma_properties(plane);

	if (icl_is_hdr_plane(dev_priv, to_intel_plane(plane)->id))
		drm_plane_attach_ctm_property(plane);

	drm_plane_attach_gamma_properties(plane);
#else
	drm_plane_attach_degamma_properties(plane);
#ifndef BPM_DRM_PLANE_ATTACH_CTM_PROPERTY_API_PRESENT
	if (icl_is_hdr_plane(dev_priv, to_intel_plane(plane)->id))
		drm_plane_attach_ctm_property(plane);
#endif
#endif

	return ret;
}
#endif

void intel_color_init(struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);

	drm_mode_crtc_set_gamma_size(&crtc->base, 256);

#ifndef BPM_DRM_GAMMA_DEGAMMA_API_PRESENT
	if (DISPLAY_VER(dev_priv) >= 13) {
		dev_priv->color_funcs = &xelpd_color_funcs;
		drm_color_create_gamma_mode_property(&crtc->base, 2);
		drm_color_add_gamma_degamma_mode_range(&crtc->base,
						       "no gamma", NULL, 0,
						       LUT_TYPE_GAMMA);
		drm_color_add_gamma_degamma_mode_range(&crtc->base,
						       "logarithmic gamma",
						       xelpd_logarithmic_gamma,
						       sizeof(xelpd_logarithmic_gamma),
						       LUT_TYPE_GAMMA);
		drm_crtc_attach_gamma_degamma_mode_property(&crtc->base,
							    LUT_TYPE_GAMMA);

		if (DISPLAY_VER(dev_priv) >= 14) {
			drm_color_create_degamma_mode_property(&crtc->base, 2);
			drm_color_add_gamma_degamma_mode_range(&crtc->base,
							       "no degamma", NULL, 0,
							       LUT_TYPE_DEGAMMA);
			drm_color_add_gamma_degamma_mode_range(&crtc->base,
							       "extended degamma",
							       mtl_24bit_degamma,
							       sizeof(mtl_24bit_degamma),
							       LUT_TYPE_DEGAMMA);
			drm_crtc_attach_gamma_degamma_mode_property(&crtc->base,
								    LUT_TYPE_DEGAMMA);
		}
	} else  {
		dev_priv->color_funcs = &icl_color_funcs;
	}
#else
#ifndef BPM_DGLUT_24BIT_MTL_NOT_SUPPORTED
	if (DISPLAY_VER(dev_priv) >= 14) {
		drm_color_create_degamma_mode_property(&crtc->base, 2);
		drm_color_add_gamma_degamma_mode_range(&crtc->base,
						       "no degamma", NULL, 0,
						       LUT_TYPE_DEGAMMA);
		drm_color_add_gamma_degamma_mode_range(&crtc->base,
						       "extended degamma",
						       mtl_24bit_degamma,
						       sizeof(mtl_24bit_degamma),
						       LUT_TYPE_DEGAMMA);
		drm_crtc_attach_gamma_degamma_mode_property(&crtc->base,
							    LUT_TYPE_DEGAMMA);
	}
#endif
	dev_priv->color_funcs = &icl_color_funcs;
#endif
	drm_crtc_enable_color_mgmt(&crtc->base,
				   INTEL_INFO(dev_priv)->display.color.degamma_lut_size,
				   true,
				   INTEL_INFO(dev_priv)->display.color.gamma_lut_size);
}
