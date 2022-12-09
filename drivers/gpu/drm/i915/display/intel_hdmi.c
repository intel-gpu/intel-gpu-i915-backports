/*
 * Copyright 2006 Dave Airlie <airlied@linux.ie>
 * Copyright © 2006-2009 Intel Corporation
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
 * Authors:
 *	Eric Anholt <eric@anholt.net>
 *	Jesse Barnes <jesse.barnes@intel.com>
 */

#include <linux/delay.h>
#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
#include <linux/gcd.h>
#endif
#include <linux/hdmi.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/string_helpers.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_edid.h>
#include <drm/drm_hdcp.h>
#include <drm/drm_scdc_helper.h>
#include <drm/intel_lpe_audio.h>
#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
#include <drm/drm_frl_dfm_helper.h>
#endif

#include "i915_debugfs.h"
#include "i915_drv.h"
#include "intel_atomic.h"
#include "intel_connector.h"
#include "intel_cx0_phy.h"
#include "intel_ddi.h"
#include "intel_de.h"
#include "intel_display_types.h"
#include "intel_dp.h"
#include "intel_gmbus.h"
#include "intel_hdcp.h"
#include "intel_hdcp_regs.h"
#include "intel_hdmi.h"
#include "intel_lspcon.h"
#include "intel_panel.h"
#include "intel_snps_phy.h"
#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
#include "intel_vdsc.h"
#endif

#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
static int
get_dsc_compressed_bpp(int num_slices, int slice_width, int hdmi_max_chunk_bytes,
		       int src_fractional_bpp, int min_dsc_bpp, int max_dsc_bpp);

static void
get_dsc_min_max_bpp(enum intel_output_format output_format, u8 bpc,
		    bool hdmi_all_bpp, int *min_dsc_bpp, int *max_dsc_bpp);

static int get_dsc_slice_count(struct intel_hdmi *intel_hdmi,
			       const struct drm_display_mode *mode,
			       enum intel_output_format output_format,
			       bool use_bigjoiner);
#endif

inline struct drm_i915_private *intel_hdmi_to_i915(struct intel_hdmi *intel_hdmi)
{
	return to_i915(hdmi_to_dig_port(intel_hdmi)->base.base.dev);
}

static void
assert_hdmi_port_disabled(struct intel_hdmi *intel_hdmi)
{
	struct drm_i915_private *dev_priv = intel_hdmi_to_i915(intel_hdmi);
	u32 enabled_bits;

	enabled_bits = HAS_DDI(dev_priv) ? DDI_BUF_CTL_ENABLE : SDVO_ENABLE;

	drm_WARN(&dev_priv->drm,
		 intel_de_read(dev_priv, intel_hdmi->hdmi_reg) & enabled_bits,
		 "HDMI port enabled, expecting disabled\n");
}

static void
assert_hdmi_transcoder_func_disabled(struct drm_i915_private *dev_priv,
				     enum transcoder cpu_transcoder)
{
	drm_WARN(&dev_priv->drm,
		 intel_de_read(dev_priv, TRANS_DDI_FUNC_CTL(cpu_transcoder)) &
		 TRANS_DDI_FUNC_ENABLE,
		 "HDMI transcoder function enabled, expecting disabled\n");
}

static u32 g4x_infoframe_index(unsigned int type)
{
	switch (type) {
	case HDMI_PACKET_TYPE_GAMUT_METADATA:
		return VIDEO_DIP_SELECT_GAMUT;
	case HDMI_INFOFRAME_TYPE_AVI:
		return VIDEO_DIP_SELECT_AVI;
	case HDMI_INFOFRAME_TYPE_SPD:
		return VIDEO_DIP_SELECT_SPD;
	case HDMI_INFOFRAME_TYPE_VENDOR:
		return VIDEO_DIP_SELECT_VENDOR;
	default:
		MISSING_CASE(type);
		return 0;
	}
}

static u32 g4x_infoframe_enable(unsigned int type)
{
	switch (type) {
	case HDMI_PACKET_TYPE_GENERAL_CONTROL:
		return VIDEO_DIP_ENABLE_GCP;
	case HDMI_PACKET_TYPE_GAMUT_METADATA:
		return VIDEO_DIP_ENABLE_GAMUT;
	case DP_SDP_VSC:
		return 0;
	case HDMI_INFOFRAME_TYPE_AVI:
		return VIDEO_DIP_ENABLE_AVI;
	case HDMI_INFOFRAME_TYPE_SPD:
		return VIDEO_DIP_ENABLE_SPD;
	case HDMI_INFOFRAME_TYPE_VENDOR:
		return VIDEO_DIP_ENABLE_VENDOR;
	case HDMI_INFOFRAME_TYPE_DRM:
		return 0;
	default:
		MISSING_CASE(type);
		return 0;
	}
}

static u32 hsw_infoframe_enable(unsigned int type)
{
	switch (type) {
	case HDMI_PACKET_TYPE_GENERAL_CONTROL:
		return VIDEO_DIP_ENABLE_GCP_HSW;
	case HDMI_PACKET_TYPE_GAMUT_METADATA:
		return VIDEO_DIP_ENABLE_GMP_HSW;
	case DP_SDP_VSC:
		return VIDEO_DIP_ENABLE_VSC_HSW;
	case DP_SDP_PPS:
		return VDIP_ENABLE_PPS;
	case HDMI_INFOFRAME_TYPE_AVI:
		return VIDEO_DIP_ENABLE_AVI_HSW;
	case HDMI_INFOFRAME_TYPE_SPD:
		return VIDEO_DIP_ENABLE_SPD_HSW;
	case HDMI_INFOFRAME_TYPE_VENDOR:
		return VIDEO_DIP_ENABLE_VS_HSW;
	case HDMI_INFOFRAME_TYPE_DRM:
		return VIDEO_DIP_ENABLE_DRM_GLK;
	default:
		MISSING_CASE(type);
		return 0;
	}
}

static i915_reg_t
hsw_dip_data_reg(struct drm_i915_private *dev_priv,
		 enum transcoder cpu_transcoder,
		 unsigned int type,
		 int i)
{
	switch (type) {
	case HDMI_PACKET_TYPE_GAMUT_METADATA:
		return HSW_TVIDEO_DIP_GMP_DATA(cpu_transcoder, i);
	case DP_SDP_VSC:
		return HSW_TVIDEO_DIP_VSC_DATA(cpu_transcoder, i);
	case DP_SDP_PPS:
		return ICL_VIDEO_DIP_PPS_DATA(cpu_transcoder, i);
	case HDMI_INFOFRAME_TYPE_AVI:
		return HSW_TVIDEO_DIP_AVI_DATA(cpu_transcoder, i);
	case HDMI_INFOFRAME_TYPE_SPD:
		return HSW_TVIDEO_DIP_SPD_DATA(cpu_transcoder, i);
	case HDMI_INFOFRAME_TYPE_VENDOR:
		return HSW_TVIDEO_DIP_VS_DATA(cpu_transcoder, i);
	case HDMI_INFOFRAME_TYPE_DRM:
		return GLK_TVIDEO_DIP_DRM_DATA(cpu_transcoder, i);
	default:
		MISSING_CASE(type);
		return INVALID_MMIO_REG;
	}
}

static int hsw_dip_data_size(struct drm_i915_private *dev_priv,
			     unsigned int type)
{
	switch (type) {
	case DP_SDP_VSC:
		return VIDEO_DIP_VSC_DATA_SIZE;
	case DP_SDP_PPS:
		return VIDEO_DIP_PPS_DATA_SIZE;
	case HDMI_PACKET_TYPE_GAMUT_METADATA:
		if (DISPLAY_VER(dev_priv) >= 11)
			return VIDEO_DIP_GMP_DATA_SIZE;
		else
			return VIDEO_DIP_DATA_SIZE;
	default:
		return VIDEO_DIP_DATA_SIZE;
	}
}

static void g4x_write_infoframe(struct intel_encoder *encoder,
				const struct intel_crtc_state *crtc_state,
				unsigned int type,
				const void *frame, ssize_t len)
{
	const u32 *data = frame;
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	u32 val = intel_de_read(dev_priv, VIDEO_DIP_CTL);
	int i;

	drm_WARN(&dev_priv->drm, !(val & VIDEO_DIP_ENABLE),
		 "Writing DIP with CTL reg disabled\n");

	val &= ~(VIDEO_DIP_SELECT_MASK | 0xf); /* clear DIP data offset */
	val |= g4x_infoframe_index(type);

	val &= ~g4x_infoframe_enable(type);

	intel_de_write(dev_priv, VIDEO_DIP_CTL, val);

	for (i = 0; i < len; i += 4) {
		intel_de_write(dev_priv, VIDEO_DIP_DATA, *data);
		data++;
	}
	/* Write every possible data byte to force correct ECC calculation. */
	for (; i < VIDEO_DIP_DATA_SIZE; i += 4)
		intel_de_write(dev_priv, VIDEO_DIP_DATA, 0);

	val |= g4x_infoframe_enable(type);
	val &= ~VIDEO_DIP_FREQ_MASK;
	val |= VIDEO_DIP_FREQ_VSYNC;

	intel_de_write(dev_priv, VIDEO_DIP_CTL, val);
	intel_de_posting_read(dev_priv, VIDEO_DIP_CTL);
}

static void g4x_read_infoframe(struct intel_encoder *encoder,
			       const struct intel_crtc_state *crtc_state,
			       unsigned int type,
			       void *frame, ssize_t len)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	u32 val, *data = frame;
	int i;

	val = intel_de_read(dev_priv, VIDEO_DIP_CTL);

	val &= ~(VIDEO_DIP_SELECT_MASK | 0xf); /* clear DIP data offset */
	val |= g4x_infoframe_index(type);

	intel_de_write(dev_priv, VIDEO_DIP_CTL, val);

	for (i = 0; i < len; i += 4)
		*data++ = intel_de_read(dev_priv, VIDEO_DIP_DATA);
}

static u32 g4x_infoframes_enabled(struct intel_encoder *encoder,
				  const struct intel_crtc_state *pipe_config)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	u32 val = intel_de_read(dev_priv, VIDEO_DIP_CTL);

	if ((val & VIDEO_DIP_ENABLE) == 0)
		return 0;

	if ((val & VIDEO_DIP_PORT_MASK) != VIDEO_DIP_PORT(encoder->port))
		return 0;

	return val & (VIDEO_DIP_ENABLE_AVI |
		      VIDEO_DIP_ENABLE_VENDOR | VIDEO_DIP_ENABLE_SPD);
}

static void ibx_write_infoframe(struct intel_encoder *encoder,
				const struct intel_crtc_state *crtc_state,
				unsigned int type,
				const void *frame, ssize_t len)
{
	const u32 *data = frame;
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	i915_reg_t reg = TVIDEO_DIP_CTL(crtc->pipe);
	u32 val = intel_de_read(dev_priv, reg);
	int i;

	drm_WARN(&dev_priv->drm, !(val & VIDEO_DIP_ENABLE),
		 "Writing DIP with CTL reg disabled\n");

	val &= ~(VIDEO_DIP_SELECT_MASK | 0xf); /* clear DIP data offset */
	val |= g4x_infoframe_index(type);

	val &= ~g4x_infoframe_enable(type);

	intel_de_write(dev_priv, reg, val);

	for (i = 0; i < len; i += 4) {
		intel_de_write(dev_priv, TVIDEO_DIP_DATA(crtc->pipe),
			       *data);
		data++;
	}
	/* Write every possible data byte to force correct ECC calculation. */
	for (; i < VIDEO_DIP_DATA_SIZE; i += 4)
		intel_de_write(dev_priv, TVIDEO_DIP_DATA(crtc->pipe), 0);

	val |= g4x_infoframe_enable(type);
	val &= ~VIDEO_DIP_FREQ_MASK;
	val |= VIDEO_DIP_FREQ_VSYNC;

	intel_de_write(dev_priv, reg, val);
	intel_de_posting_read(dev_priv, reg);
}

static void ibx_read_infoframe(struct intel_encoder *encoder,
			       const struct intel_crtc_state *crtc_state,
			       unsigned int type,
			       void *frame, ssize_t len)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	u32 val, *data = frame;
	int i;

	val = intel_de_read(dev_priv, TVIDEO_DIP_CTL(crtc->pipe));

	val &= ~(VIDEO_DIP_SELECT_MASK | 0xf); /* clear DIP data offset */
	val |= g4x_infoframe_index(type);

	intel_de_write(dev_priv, TVIDEO_DIP_CTL(crtc->pipe), val);

	for (i = 0; i < len; i += 4)
		*data++ = intel_de_read(dev_priv, TVIDEO_DIP_DATA(crtc->pipe));
}

static u32 ibx_infoframes_enabled(struct intel_encoder *encoder,
				  const struct intel_crtc_state *pipe_config)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	enum pipe pipe = to_intel_crtc(pipe_config->uapi.crtc)->pipe;
	i915_reg_t reg = TVIDEO_DIP_CTL(pipe);
	u32 val = intel_de_read(dev_priv, reg);

	if ((val & VIDEO_DIP_ENABLE) == 0)
		return 0;

	if ((val & VIDEO_DIP_PORT_MASK) != VIDEO_DIP_PORT(encoder->port))
		return 0;

	return val & (VIDEO_DIP_ENABLE_AVI |
		      VIDEO_DIP_ENABLE_VENDOR | VIDEO_DIP_ENABLE_GAMUT |
		      VIDEO_DIP_ENABLE_SPD | VIDEO_DIP_ENABLE_GCP);
}

static void cpt_write_infoframe(struct intel_encoder *encoder,
				const struct intel_crtc_state *crtc_state,
				unsigned int type,
				const void *frame, ssize_t len)
{
	const u32 *data = frame;
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	i915_reg_t reg = TVIDEO_DIP_CTL(crtc->pipe);
	u32 val = intel_de_read(dev_priv, reg);
	int i;

	drm_WARN(&dev_priv->drm, !(val & VIDEO_DIP_ENABLE),
		 "Writing DIP with CTL reg disabled\n");

	val &= ~(VIDEO_DIP_SELECT_MASK | 0xf); /* clear DIP data offset */
	val |= g4x_infoframe_index(type);

	/* The DIP control register spec says that we need to update the AVI
	 * infoframe without clearing its enable bit */
	if (type != HDMI_INFOFRAME_TYPE_AVI)
		val &= ~g4x_infoframe_enable(type);

	intel_de_write(dev_priv, reg, val);

	for (i = 0; i < len; i += 4) {
		intel_de_write(dev_priv, TVIDEO_DIP_DATA(crtc->pipe),
			       *data);
		data++;
	}
	/* Write every possible data byte to force correct ECC calculation. */
	for (; i < VIDEO_DIP_DATA_SIZE; i += 4)
		intel_de_write(dev_priv, TVIDEO_DIP_DATA(crtc->pipe), 0);

	val |= g4x_infoframe_enable(type);
	val &= ~VIDEO_DIP_FREQ_MASK;
	val |= VIDEO_DIP_FREQ_VSYNC;

	intel_de_write(dev_priv, reg, val);
	intel_de_posting_read(dev_priv, reg);
}

static void cpt_read_infoframe(struct intel_encoder *encoder,
			       const struct intel_crtc_state *crtc_state,
			       unsigned int type,
			       void *frame, ssize_t len)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	u32 val, *data = frame;
	int i;

	val = intel_de_read(dev_priv, TVIDEO_DIP_CTL(crtc->pipe));

	val &= ~(VIDEO_DIP_SELECT_MASK | 0xf); /* clear DIP data offset */
	val |= g4x_infoframe_index(type);

	intel_de_write(dev_priv, TVIDEO_DIP_CTL(crtc->pipe), val);

	for (i = 0; i < len; i += 4)
		*data++ = intel_de_read(dev_priv, TVIDEO_DIP_DATA(crtc->pipe));
}

static u32 cpt_infoframes_enabled(struct intel_encoder *encoder,
				  const struct intel_crtc_state *pipe_config)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	enum pipe pipe = to_intel_crtc(pipe_config->uapi.crtc)->pipe;
	u32 val = intel_de_read(dev_priv, TVIDEO_DIP_CTL(pipe));

	if ((val & VIDEO_DIP_ENABLE) == 0)
		return 0;

	return val & (VIDEO_DIP_ENABLE_AVI |
		      VIDEO_DIP_ENABLE_VENDOR | VIDEO_DIP_ENABLE_GAMUT |
		      VIDEO_DIP_ENABLE_SPD | VIDEO_DIP_ENABLE_GCP);
}

static void vlv_write_infoframe(struct intel_encoder *encoder,
				const struct intel_crtc_state *crtc_state,
				unsigned int type,
				const void *frame, ssize_t len)
{
	const u32 *data = frame;
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	i915_reg_t reg = VLV_TVIDEO_DIP_CTL(crtc->pipe);
	u32 val = intel_de_read(dev_priv, reg);
	int i;

	drm_WARN(&dev_priv->drm, !(val & VIDEO_DIP_ENABLE),
		 "Writing DIP with CTL reg disabled\n");

	val &= ~(VIDEO_DIP_SELECT_MASK | 0xf); /* clear DIP data offset */
	val |= g4x_infoframe_index(type);

	val &= ~g4x_infoframe_enable(type);

	intel_de_write(dev_priv, reg, val);

	for (i = 0; i < len; i += 4) {
		intel_de_write(dev_priv,
			       VLV_TVIDEO_DIP_DATA(crtc->pipe), *data);
		data++;
	}
	/* Write every possible data byte to force correct ECC calculation. */
	for (; i < VIDEO_DIP_DATA_SIZE; i += 4)
		intel_de_write(dev_priv,
			       VLV_TVIDEO_DIP_DATA(crtc->pipe), 0);

	val |= g4x_infoframe_enable(type);
	val &= ~VIDEO_DIP_FREQ_MASK;
	val |= VIDEO_DIP_FREQ_VSYNC;

	intel_de_write(dev_priv, reg, val);
	intel_de_posting_read(dev_priv, reg);
}

static void vlv_read_infoframe(struct intel_encoder *encoder,
			       const struct intel_crtc_state *crtc_state,
			       unsigned int type,
			       void *frame, ssize_t len)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	u32 val, *data = frame;
	int i;

	val = intel_de_read(dev_priv, VLV_TVIDEO_DIP_CTL(crtc->pipe));

	val &= ~(VIDEO_DIP_SELECT_MASK | 0xf); /* clear DIP data offset */
	val |= g4x_infoframe_index(type);

	intel_de_write(dev_priv, VLV_TVIDEO_DIP_CTL(crtc->pipe), val);

	for (i = 0; i < len; i += 4)
		*data++ = intel_de_read(dev_priv,
				        VLV_TVIDEO_DIP_DATA(crtc->pipe));
}

static u32 vlv_infoframes_enabled(struct intel_encoder *encoder,
				  const struct intel_crtc_state *pipe_config)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	enum pipe pipe = to_intel_crtc(pipe_config->uapi.crtc)->pipe;
	u32 val = intel_de_read(dev_priv, VLV_TVIDEO_DIP_CTL(pipe));

	if ((val & VIDEO_DIP_ENABLE) == 0)
		return 0;

	if ((val & VIDEO_DIP_PORT_MASK) != VIDEO_DIP_PORT(encoder->port))
		return 0;

	return val & (VIDEO_DIP_ENABLE_AVI |
		      VIDEO_DIP_ENABLE_VENDOR | VIDEO_DIP_ENABLE_GAMUT |
		      VIDEO_DIP_ENABLE_SPD | VIDEO_DIP_ENABLE_GCP);
}

void hsw_write_infoframe(struct intel_encoder *encoder,
			 const struct intel_crtc_state *crtc_state,
			 unsigned int type,
			 const void *frame, ssize_t len)
{
	const u32 *data = frame;
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;
	i915_reg_t ctl_reg = HSW_TVIDEO_DIP_CTL(cpu_transcoder);
	int data_size;
	int i;
	u32 val = intel_de_read(dev_priv, ctl_reg);

	data_size = hsw_dip_data_size(dev_priv, type);

	drm_WARN_ON(&dev_priv->drm, len > data_size);

	val &= ~hsw_infoframe_enable(type);
	intel_de_write(dev_priv, ctl_reg, val);

	for (i = 0; i < len; i += 4) {
		intel_de_write(dev_priv,
			       hsw_dip_data_reg(dev_priv, cpu_transcoder, type, i >> 2),
			       *data);
		data++;
	}
	/* Write every possible data byte to force correct ECC calculation. */
	for (; i < data_size; i += 4)
		intel_de_write(dev_priv,
			       hsw_dip_data_reg(dev_priv, cpu_transcoder, type, i >> 2),
			       0);

	/* Wa_14013475917 */
	if ((DISPLAY_VER(dev_priv) == 13 ||
	     IS_MTL_DISPLAY_STEP(dev_priv, STEP_A0, STEP_B0)) && crtc_state->has_psr &&
	    type == DP_SDP_VSC)
		return;
#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
	/*
	 * VIDEO_DIP_CTL's PPS bit is not to be set for HDMI CVTEM PPS, though
	 * the DP_SDP_DATA is used to send the packets.
	 */
	if (DISPLAY_VER(dev_priv) >= 14 && crtc_state->cvt_emp.enabled &&
	    type == DP_SDP_PPS)
		return;
#endif
	val |= hsw_infoframe_enable(type);
	intel_de_write(dev_priv, ctl_reg, val);
	intel_de_posting_read(dev_priv, ctl_reg);
}

void hsw_read_infoframe(struct intel_encoder *encoder,
			const struct intel_crtc_state *crtc_state,
			unsigned int type, void *frame, ssize_t len)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;
	u32 *data = frame;
	int i;

	for (i = 0; i < len; i += 4)
		*data++ = intel_de_read(dev_priv,
				        hsw_dip_data_reg(dev_priv, cpu_transcoder, type, i >> 2));
}

static u32 hsw_infoframes_enabled(struct intel_encoder *encoder,
				  const struct intel_crtc_state *pipe_config)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	u32 val = intel_de_read(dev_priv,
				HSW_TVIDEO_DIP_CTL(pipe_config->cpu_transcoder));
	u32 mask;

	mask = (VIDEO_DIP_ENABLE_VSC_HSW | VIDEO_DIP_ENABLE_AVI_HSW |
		VIDEO_DIP_ENABLE_GCP_HSW | VIDEO_DIP_ENABLE_VS_HSW |
		VIDEO_DIP_ENABLE_GMP_HSW | VIDEO_DIP_ENABLE_SPD_HSW);

	if (DISPLAY_VER(dev_priv) >= 10)
		mask |= VIDEO_DIP_ENABLE_DRM_GLK;

	return val & mask;
}

static const u8 infoframe_type_to_idx[] = {
	HDMI_PACKET_TYPE_GENERAL_CONTROL,
	HDMI_PACKET_TYPE_GAMUT_METADATA,
	DP_SDP_VSC,
	HDMI_INFOFRAME_TYPE_AVI,
	HDMI_INFOFRAME_TYPE_SPD,
	HDMI_INFOFRAME_TYPE_VENDOR,
	HDMI_INFOFRAME_TYPE_DRM,
};

u32 intel_hdmi_infoframe_enable(unsigned int type)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(infoframe_type_to_idx); i++) {
		if (infoframe_type_to_idx[i] == type)
			return BIT(i);
	}

	return 0;
}

u32 intel_hdmi_infoframes_enabled(struct intel_encoder *encoder,
				  const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);
	u32 val, ret = 0;
	int i;

	val = dig_port->infoframes_enabled(encoder, crtc_state);

	/* map from hardware bits to dip idx */
	for (i = 0; i < ARRAY_SIZE(infoframe_type_to_idx); i++) {
		unsigned int type = infoframe_type_to_idx[i];

		if (HAS_DDI(dev_priv)) {
			if (val & hsw_infoframe_enable(type))
				ret |= BIT(i);
		} else {
			if (val & g4x_infoframe_enable(type))
				ret |= BIT(i);
		}
	}

	return ret;
}

/*
 * The data we write to the DIP data buffer registers is 1 byte bigger than the
 * HDMI infoframe size because of an ECC/reserved byte at position 3 (starting
 * at 0). It's also a byte used by DisplayPort so the same DIP registers can be
 * used for both technologies.
 *
 * DW0: Reserved/ECC/DP | HB2 | HB1 | HB0
 * DW1:       DB3       | DB2 | DB1 | DB0
 * DW2:       DB7       | DB6 | DB5 | DB4
 * DW3: ...
 *
 * (HB is Header Byte, DB is Data Byte)
 *
 * The hdmi pack() functions don't know about that hardware specific hole so we
 * trick them by giving an offset into the buffer and moving back the header
 * bytes by one.
 */
static void intel_write_infoframe(struct intel_encoder *encoder,
				  const struct intel_crtc_state *crtc_state,
				  enum hdmi_infoframe_type type,
				  const union hdmi_infoframe *frame)
{
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);
	u8 buffer[VIDEO_DIP_DATA_SIZE];
	ssize_t len;

	if ((crtc_state->infoframes.enable &
	     intel_hdmi_infoframe_enable(type)) == 0)
		return;

	if (drm_WARN_ON(encoder->base.dev, frame->any.type != type))
		return;

	/* see comment above for the reason for this offset */
	len = hdmi_infoframe_pack_only(frame, buffer + 1, sizeof(buffer) - 1);
	if (drm_WARN_ON(encoder->base.dev, len < 0))
		return;

	/* Insert the 'hole' (see big comment above) at position 3 */
	memmove(&buffer[0], &buffer[1], 3);
	buffer[3] = 0;
	len++;

	dig_port->write_infoframe(encoder, crtc_state, type, buffer, len);
}

void intel_read_infoframe(struct intel_encoder *encoder,
			  const struct intel_crtc_state *crtc_state,
			  enum hdmi_infoframe_type type,
			  union hdmi_infoframe *frame)
{
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);
	u8 buffer[VIDEO_DIP_DATA_SIZE];
	int ret;

	if ((crtc_state->infoframes.enable &
	     intel_hdmi_infoframe_enable(type)) == 0)
		return;

	dig_port->read_infoframe(encoder, crtc_state,
				       type, buffer, sizeof(buffer));

	/* Fill the 'hole' (see big comment above) at position 3 */
	memmove(&buffer[1], &buffer[0], 3);

	/* see comment above for the reason for this offset */
	ret = hdmi_infoframe_unpack(frame, buffer + 1, sizeof(buffer) - 1);
	if (ret) {
		drm_dbg_kms(encoder->base.dev,
			    "Failed to unpack infoframe type 0x%02x\n", type);
		return;
	}

	if (frame->any.type != type)
		drm_dbg_kms(encoder->base.dev,
			    "Found the wrong infoframe type 0x%x (expected 0x%02x)\n",
			    frame->any.type, type);
}

static bool
intel_hdmi_compute_avi_infoframe(struct intel_encoder *encoder,
				 struct intel_crtc_state *crtc_state,
				 struct drm_connector_state *conn_state)
{
	struct hdmi_avi_infoframe *frame = &crtc_state->infoframes.avi.avi;
	const struct drm_display_mode *adjusted_mode =
		&crtc_state->hw.adjusted_mode;
	struct drm_connector *connector = conn_state->connector;
	int ret;

	if (!crtc_state->has_infoframe)
		return true;

	crtc_state->infoframes.enable |=
		intel_hdmi_infoframe_enable(HDMI_INFOFRAME_TYPE_AVI);

	ret = drm_hdmi_avi_infoframe_from_display_mode(frame, connector,
						       adjusted_mode);
	if (ret)
		return false;

	if (crtc_state->output_format == INTEL_OUTPUT_FORMAT_YCBCR420)
		frame->colorspace = HDMI_COLORSPACE_YUV420;
	else if (crtc_state->output_format == INTEL_OUTPUT_FORMAT_YCBCR444)
		frame->colorspace = HDMI_COLORSPACE_YUV444;
	else
		frame->colorspace = HDMI_COLORSPACE_RGB;

	drm_hdmi_avi_infoframe_colorspace(frame, conn_state);

	/* nonsense combination */
	drm_WARN_ON(encoder->base.dev, crtc_state->limited_color_range &&
		    crtc_state->output_format != INTEL_OUTPUT_FORMAT_RGB);

	if (crtc_state->output_format == INTEL_OUTPUT_FORMAT_RGB) {
		drm_hdmi_avi_infoframe_quant_range(frame, connector,
						   adjusted_mode,
						   crtc_state->limited_color_range ?
						   HDMI_QUANTIZATION_RANGE_LIMITED :
						   HDMI_QUANTIZATION_RANGE_FULL);
	} else {
		frame->quantization_range = HDMI_QUANTIZATION_RANGE_DEFAULT;
		frame->ycc_quantization_range = HDMI_YCC_QUANTIZATION_RANGE_LIMITED;
	}

	drm_hdmi_avi_infoframe_content_type(frame, conn_state);

	/* TODO: handle pixel repetition for YCBCR420 outputs */

	ret = hdmi_avi_infoframe_check(frame);
	if (drm_WARN_ON(encoder->base.dev, ret))
		return false;

	return true;
}

static bool
intel_hdmi_compute_spd_infoframe(struct intel_encoder *encoder,
				 struct intel_crtc_state *crtc_state,
				 struct drm_connector_state *conn_state)
{
	struct hdmi_spd_infoframe *frame = &crtc_state->infoframes.spd.spd;
	int ret;

	if (!crtc_state->has_infoframe)
		return true;

	crtc_state->infoframes.enable |=
		intel_hdmi_infoframe_enable(HDMI_INFOFRAME_TYPE_SPD);

	ret = hdmi_spd_infoframe_init(frame, "Intel", "Integrated gfx");
	if (drm_WARN_ON(encoder->base.dev, ret))
		return false;

	frame->sdi = HDMI_SPD_SDI_PC;

	ret = hdmi_spd_infoframe_check(frame);
	if (drm_WARN_ON(encoder->base.dev, ret))
		return false;

	return true;
}

static bool
intel_hdmi_compute_hdmi_infoframe(struct intel_encoder *encoder,
				  struct intel_crtc_state *crtc_state,
				  struct drm_connector_state *conn_state)
{
	struct hdmi_vendor_infoframe *frame =
		&crtc_state->infoframes.hdmi.vendor.hdmi;
	const struct drm_display_info *info =
		&conn_state->connector->display_info;
	int ret;

	if (!crtc_state->has_infoframe || !info->has_hdmi_infoframe)
		return true;

	crtc_state->infoframes.enable |=
		intel_hdmi_infoframe_enable(HDMI_INFOFRAME_TYPE_VENDOR);

	ret = drm_hdmi_vendor_infoframe_from_display_mode(frame,
							  conn_state->connector,
							  &crtc_state->hw.adjusted_mode);
	if (drm_WARN_ON(encoder->base.dev, ret))
		return false;

	ret = hdmi_vendor_infoframe_check(frame);
	if (drm_WARN_ON(encoder->base.dev, ret))
		return false;

	return true;
}

static bool
intel_hdmi_compute_drm_infoframe(struct intel_encoder *encoder,
				 struct intel_crtc_state *crtc_state,
				 struct drm_connector_state *conn_state)
{
	struct hdmi_drm_infoframe *frame = &crtc_state->infoframes.drm.drm;
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	int ret;

	if (DISPLAY_VER(dev_priv) < 10)
		return true;

	if (!crtc_state->has_infoframe)
		return true;

	if (!conn_state->hdr_output_metadata)
		return true;

	crtc_state->infoframes.enable |=
		intel_hdmi_infoframe_enable(HDMI_INFOFRAME_TYPE_DRM);

	ret = drm_hdmi_infoframe_set_hdr_metadata(frame, conn_state);
	if (ret < 0) {
		drm_dbg_kms(&dev_priv->drm,
			    "couldn't set HDR metadata in infoframe\n");
		return false;
	}

	ret = hdmi_drm_infoframe_check(frame);
	if (drm_WARN_ON(&dev_priv->drm, ret))
		return false;

	return true;
}

static void g4x_set_infoframes(struct intel_encoder *encoder,
			       bool enable,
			       const struct intel_crtc_state *crtc_state,
			       const struct drm_connector_state *conn_state)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);
	struct intel_hdmi *intel_hdmi = &dig_port->hdmi;
	i915_reg_t reg = VIDEO_DIP_CTL;
	u32 val = intel_de_read(dev_priv, reg);
	u32 port = VIDEO_DIP_PORT(encoder->port);

	assert_hdmi_port_disabled(intel_hdmi);

	/* If the registers were not initialized yet, they might be zeroes,
	 * which means we're selecting the AVI DIP and we're setting its
	 * frequency to once. This seems to really confuse the HW and make
	 * things stop working (the register spec says the AVI always needs to
	 * be sent every VSync). So here we avoid writing to the register more
	 * than we need and also explicitly select the AVI DIP and explicitly
	 * set its frequency to every VSync. Avoiding to write it twice seems to
	 * be enough to solve the problem, but being defensive shouldn't hurt us
	 * either. */
	val |= VIDEO_DIP_SELECT_AVI | VIDEO_DIP_FREQ_VSYNC;

	if (!enable) {
		if (!(val & VIDEO_DIP_ENABLE))
			return;
		if (port != (val & VIDEO_DIP_PORT_MASK)) {
			drm_dbg_kms(&dev_priv->drm,
				    "video DIP still enabled on port %c\n",
				    (val & VIDEO_DIP_PORT_MASK) >> 29);
			return;
		}
		val &= ~(VIDEO_DIP_ENABLE | VIDEO_DIP_ENABLE_AVI |
			 VIDEO_DIP_ENABLE_VENDOR | VIDEO_DIP_ENABLE_SPD);
		intel_de_write(dev_priv, reg, val);
		intel_de_posting_read(dev_priv, reg);
		return;
	}

	if (port != (val & VIDEO_DIP_PORT_MASK)) {
		if (val & VIDEO_DIP_ENABLE) {
			drm_dbg_kms(&dev_priv->drm,
				    "video DIP already enabled on port %c\n",
				    (val & VIDEO_DIP_PORT_MASK) >> 29);
			return;
		}
		val &= ~VIDEO_DIP_PORT_MASK;
		val |= port;
	}

	val |= VIDEO_DIP_ENABLE;
	val &= ~(VIDEO_DIP_ENABLE_AVI |
		 VIDEO_DIP_ENABLE_VENDOR | VIDEO_DIP_ENABLE_SPD);

	intel_de_write(dev_priv, reg, val);
	intel_de_posting_read(dev_priv, reg);

	intel_write_infoframe(encoder, crtc_state,
			      HDMI_INFOFRAME_TYPE_AVI,
			      &crtc_state->infoframes.avi);
	intel_write_infoframe(encoder, crtc_state,
			      HDMI_INFOFRAME_TYPE_SPD,
			      &crtc_state->infoframes.spd);
	intel_write_infoframe(encoder, crtc_state,
			      HDMI_INFOFRAME_TYPE_VENDOR,
			      &crtc_state->infoframes.hdmi);
}

/*
 * Determine if default_phase=1 can be indicated in the GCP infoframe.
 *
 * From HDMI specification 1.4a:
 * - The first pixel of each Video Data Period shall always have a pixel packing phase of 0
 * - The first pixel following each Video Data Period shall have a pixel packing phase of 0
 * - The PP bits shall be constant for all GCPs and will be equal to the last packing phase
 * - The first pixel following every transition of HSYNC or VSYNC shall have a pixel packing
 *   phase of 0
 */
static bool gcp_default_phase_possible(int pipe_bpp,
				       const struct drm_display_mode *mode)
{
	unsigned int pixels_per_group;

	switch (pipe_bpp) {
	case 30:
		/* 4 pixels in 5 clocks */
		pixels_per_group = 4;
		break;
	case 36:
		/* 2 pixels in 3 clocks */
		pixels_per_group = 2;
		break;
	case 48:
		/* 1 pixel in 2 clocks */
		pixels_per_group = 1;
		break;
	default:
		/* phase information not relevant for 8bpc */
		return false;
	}

	return mode->crtc_hdisplay % pixels_per_group == 0 &&
		mode->crtc_htotal % pixels_per_group == 0 &&
		mode->crtc_hblank_start % pixels_per_group == 0 &&
		mode->crtc_hblank_end % pixels_per_group == 0 &&
		mode->crtc_hsync_start % pixels_per_group == 0 &&
		mode->crtc_hsync_end % pixels_per_group == 0 &&
		((mode->flags & DRM_MODE_FLAG_INTERLACE) == 0 ||
		 mode->crtc_htotal/2 % pixels_per_group == 0);
}

static bool intel_hdmi_set_gcp_infoframe(struct intel_encoder *encoder,
					 const struct intel_crtc_state *crtc_state,
					 const struct drm_connector_state *conn_state)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	i915_reg_t reg;

	if ((crtc_state->infoframes.enable &
	     intel_hdmi_infoframe_enable(HDMI_PACKET_TYPE_GENERAL_CONTROL)) == 0)
		return false;

	if (HAS_DDI(dev_priv))
		reg = HSW_TVIDEO_DIP_GCP(crtc_state->cpu_transcoder);
	else if (IS_VALLEYVIEW(dev_priv) || IS_CHERRYVIEW(dev_priv))
		reg = VLV_TVIDEO_DIP_GCP(crtc->pipe);
	else if (HAS_PCH_SPLIT(dev_priv))
		reg = TVIDEO_DIP_GCP(crtc->pipe);
	else
		return false;

	intel_de_write(dev_priv, reg, crtc_state->infoframes.gcp);

	return true;
}

void intel_hdmi_read_gcp_infoframe(struct intel_encoder *encoder,
				   struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	i915_reg_t reg;

	if ((crtc_state->infoframes.enable &
	     intel_hdmi_infoframe_enable(HDMI_PACKET_TYPE_GENERAL_CONTROL)) == 0)
		return;

	if (HAS_DDI(dev_priv))
		reg = HSW_TVIDEO_DIP_GCP(crtc_state->cpu_transcoder);
	else if (IS_VALLEYVIEW(dev_priv) || IS_CHERRYVIEW(dev_priv))
		reg = VLV_TVIDEO_DIP_GCP(crtc->pipe);
	else if (HAS_PCH_SPLIT(dev_priv))
		reg = TVIDEO_DIP_GCP(crtc->pipe);
	else
		return;

	crtc_state->infoframes.gcp = intel_de_read(dev_priv, reg);
}

static void intel_hdmi_compute_gcp_infoframe(struct intel_encoder *encoder,
					     struct intel_crtc_state *crtc_state,
					     struct drm_connector_state *conn_state)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);

	if (IS_G4X(dev_priv) || !crtc_state->has_infoframe)
		return;

	crtc_state->infoframes.enable |=
		intel_hdmi_infoframe_enable(HDMI_PACKET_TYPE_GENERAL_CONTROL);

	/* Indicate color indication for deep color mode */
	if (crtc_state->pipe_bpp > 24)
		crtc_state->infoframes.gcp |= GCP_COLOR_INDICATION;

	/* Enable default_phase whenever the display mode is suitably aligned */
	if (gcp_default_phase_possible(crtc_state->pipe_bpp,
				       &crtc_state->hw.adjusted_mode))
		crtc_state->infoframes.gcp |= GCP_DEFAULT_PHASE_ENABLE;
}

static void ibx_set_infoframes(struct intel_encoder *encoder,
			       bool enable,
			       const struct intel_crtc_state *crtc_state,
			       const struct drm_connector_state *conn_state)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);
	struct intel_hdmi *intel_hdmi = &dig_port->hdmi;
	i915_reg_t reg = TVIDEO_DIP_CTL(crtc->pipe);
	u32 val = intel_de_read(dev_priv, reg);
	u32 port = VIDEO_DIP_PORT(encoder->port);

	assert_hdmi_port_disabled(intel_hdmi);

	/* See the big comment in g4x_set_infoframes() */
	val |= VIDEO_DIP_SELECT_AVI | VIDEO_DIP_FREQ_VSYNC;

	if (!enable) {
		if (!(val & VIDEO_DIP_ENABLE))
			return;
		val &= ~(VIDEO_DIP_ENABLE | VIDEO_DIP_ENABLE_AVI |
			 VIDEO_DIP_ENABLE_VENDOR | VIDEO_DIP_ENABLE_GAMUT |
			 VIDEO_DIP_ENABLE_SPD | VIDEO_DIP_ENABLE_GCP);
		intel_de_write(dev_priv, reg, val);
		intel_de_posting_read(dev_priv, reg);
		return;
	}

	if (port != (val & VIDEO_DIP_PORT_MASK)) {
		drm_WARN(&dev_priv->drm, val & VIDEO_DIP_ENABLE,
			 "DIP already enabled on port %c\n",
			 (val & VIDEO_DIP_PORT_MASK) >> 29);
		val &= ~VIDEO_DIP_PORT_MASK;
		val |= port;
	}

	val |= VIDEO_DIP_ENABLE;
	val &= ~(VIDEO_DIP_ENABLE_AVI |
		 VIDEO_DIP_ENABLE_VENDOR | VIDEO_DIP_ENABLE_GAMUT |
		 VIDEO_DIP_ENABLE_SPD | VIDEO_DIP_ENABLE_GCP);

	if (intel_hdmi_set_gcp_infoframe(encoder, crtc_state, conn_state))
		val |= VIDEO_DIP_ENABLE_GCP;

	intel_de_write(dev_priv, reg, val);
	intel_de_posting_read(dev_priv, reg);

	intel_write_infoframe(encoder, crtc_state,
			      HDMI_INFOFRAME_TYPE_AVI,
			      &crtc_state->infoframes.avi);
	intel_write_infoframe(encoder, crtc_state,
			      HDMI_INFOFRAME_TYPE_SPD,
			      &crtc_state->infoframes.spd);
	intel_write_infoframe(encoder, crtc_state,
			      HDMI_INFOFRAME_TYPE_VENDOR,
			      &crtc_state->infoframes.hdmi);
}

static void cpt_set_infoframes(struct intel_encoder *encoder,
			       bool enable,
			       const struct intel_crtc_state *crtc_state,
			       const struct drm_connector_state *conn_state)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct intel_hdmi *intel_hdmi = enc_to_intel_hdmi(encoder);
	i915_reg_t reg = TVIDEO_DIP_CTL(crtc->pipe);
	u32 val = intel_de_read(dev_priv, reg);

	assert_hdmi_port_disabled(intel_hdmi);

	/* See the big comment in g4x_set_infoframes() */
	val |= VIDEO_DIP_SELECT_AVI | VIDEO_DIP_FREQ_VSYNC;

	if (!enable) {
		if (!(val & VIDEO_DIP_ENABLE))
			return;
		val &= ~(VIDEO_DIP_ENABLE | VIDEO_DIP_ENABLE_AVI |
			 VIDEO_DIP_ENABLE_VENDOR | VIDEO_DIP_ENABLE_GAMUT |
			 VIDEO_DIP_ENABLE_SPD | VIDEO_DIP_ENABLE_GCP);
		intel_de_write(dev_priv, reg, val);
		intel_de_posting_read(dev_priv, reg);
		return;
	}

	/* Set both together, unset both together: see the spec. */
	val |= VIDEO_DIP_ENABLE | VIDEO_DIP_ENABLE_AVI;
	val &= ~(VIDEO_DIP_ENABLE_VENDOR | VIDEO_DIP_ENABLE_GAMUT |
		 VIDEO_DIP_ENABLE_SPD | VIDEO_DIP_ENABLE_GCP);

	if (intel_hdmi_set_gcp_infoframe(encoder, crtc_state, conn_state))
		val |= VIDEO_DIP_ENABLE_GCP;

	intel_de_write(dev_priv, reg, val);
	intel_de_posting_read(dev_priv, reg);

	intel_write_infoframe(encoder, crtc_state,
			      HDMI_INFOFRAME_TYPE_AVI,
			      &crtc_state->infoframes.avi);
	intel_write_infoframe(encoder, crtc_state,
			      HDMI_INFOFRAME_TYPE_SPD,
			      &crtc_state->infoframes.spd);
	intel_write_infoframe(encoder, crtc_state,
			      HDMI_INFOFRAME_TYPE_VENDOR,
			      &crtc_state->infoframes.hdmi);
}

static void vlv_set_infoframes(struct intel_encoder *encoder,
			       bool enable,
			       const struct intel_crtc_state *crtc_state,
			       const struct drm_connector_state *conn_state)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct intel_hdmi *intel_hdmi = enc_to_intel_hdmi(encoder);
	i915_reg_t reg = VLV_TVIDEO_DIP_CTL(crtc->pipe);
	u32 val = intel_de_read(dev_priv, reg);
	u32 port = VIDEO_DIP_PORT(encoder->port);

	assert_hdmi_port_disabled(intel_hdmi);

	/* See the big comment in g4x_set_infoframes() */
	val |= VIDEO_DIP_SELECT_AVI | VIDEO_DIP_FREQ_VSYNC;

	if (!enable) {
		if (!(val & VIDEO_DIP_ENABLE))
			return;
		val &= ~(VIDEO_DIP_ENABLE | VIDEO_DIP_ENABLE_AVI |
			 VIDEO_DIP_ENABLE_VENDOR | VIDEO_DIP_ENABLE_GAMUT |
			 VIDEO_DIP_ENABLE_SPD | VIDEO_DIP_ENABLE_GCP);
		intel_de_write(dev_priv, reg, val);
		intel_de_posting_read(dev_priv, reg);
		return;
	}

	if (port != (val & VIDEO_DIP_PORT_MASK)) {
		drm_WARN(&dev_priv->drm, val & VIDEO_DIP_ENABLE,
			 "DIP already enabled on port %c\n",
			 (val & VIDEO_DIP_PORT_MASK) >> 29);
		val &= ~VIDEO_DIP_PORT_MASK;
		val |= port;
	}

	val |= VIDEO_DIP_ENABLE;
	val &= ~(VIDEO_DIP_ENABLE_AVI |
		 VIDEO_DIP_ENABLE_VENDOR | VIDEO_DIP_ENABLE_GAMUT |
		 VIDEO_DIP_ENABLE_SPD | VIDEO_DIP_ENABLE_GCP);

	if (intel_hdmi_set_gcp_infoframe(encoder, crtc_state, conn_state))
		val |= VIDEO_DIP_ENABLE_GCP;

	intel_de_write(dev_priv, reg, val);
	intel_de_posting_read(dev_priv, reg);

	intel_write_infoframe(encoder, crtc_state,
			      HDMI_INFOFRAME_TYPE_AVI,
			      &crtc_state->infoframes.avi);
	intel_write_infoframe(encoder, crtc_state,
			      HDMI_INFOFRAME_TYPE_SPD,
			      &crtc_state->infoframes.spd);
	intel_write_infoframe(encoder, crtc_state,
			      HDMI_INFOFRAME_TYPE_VENDOR,
			      &crtc_state->infoframes.hdmi);
}

static void hsw_set_infoframes(struct intel_encoder *encoder,
			       bool enable,
			       const struct intel_crtc_state *crtc_state,
			       const struct drm_connector_state *conn_state)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	i915_reg_t reg = HSW_TVIDEO_DIP_CTL(crtc_state->cpu_transcoder);
	u32 val = intel_de_read(dev_priv, reg);

	assert_hdmi_transcoder_func_disabled(dev_priv,
					     crtc_state->cpu_transcoder);

	val &= ~(VIDEO_DIP_ENABLE_VSC_HSW | VIDEO_DIP_ENABLE_AVI_HSW |
		 VIDEO_DIP_ENABLE_GCP_HSW | VIDEO_DIP_ENABLE_VS_HSW |
		 VIDEO_DIP_ENABLE_GMP_HSW | VIDEO_DIP_ENABLE_SPD_HSW |
		 VIDEO_DIP_ENABLE_DRM_GLK);

	if (!enable) {
		intel_de_write(dev_priv, reg, val);
		intel_de_posting_read(dev_priv, reg);
		return;
	}

	if (intel_hdmi_set_gcp_infoframe(encoder, crtc_state, conn_state))
		val |= VIDEO_DIP_ENABLE_GCP_HSW;

	intel_de_write(dev_priv, reg, val);
	intel_de_posting_read(dev_priv, reg);

	intel_write_infoframe(encoder, crtc_state,
			      HDMI_INFOFRAME_TYPE_AVI,
			      &crtc_state->infoframes.avi);
	intel_write_infoframe(encoder, crtc_state,
			      HDMI_INFOFRAME_TYPE_SPD,
			      &crtc_state->infoframes.spd);
	intel_write_infoframe(encoder, crtc_state,
			      HDMI_INFOFRAME_TYPE_VENDOR,
			      &crtc_state->infoframes.hdmi);
	intel_write_infoframe(encoder, crtc_state,
			      HDMI_INFOFRAME_TYPE_DRM,
			      &crtc_state->infoframes.drm);
}

void intel_dp_dual_mode_set_tmds_output(struct intel_hdmi *hdmi, bool enable)
{
	struct drm_i915_private *dev_priv = intel_hdmi_to_i915(hdmi);
	struct i2c_adapter *adapter;

	if (hdmi->dp_dual_mode.type < DRM_DP_DUAL_MODE_TYPE2_DVI)
		return;

	adapter = intel_gmbus_get_adapter(dev_priv, hdmi->ddc_bus);

	drm_dbg_kms(&dev_priv->drm, "%s DP dual mode adaptor TMDS output\n",
		    enable ? "Enabling" : "Disabling");

	drm_dp_dual_mode_set_tmds_output(&dev_priv->drm, hdmi->dp_dual_mode.type, adapter, enable);
}

static int intel_hdmi_hdcp_read(struct intel_digital_port *dig_port,
				unsigned int offset, void *buffer, size_t size)
{
	struct drm_i915_private *i915 = to_i915(dig_port->base.base.dev);
	struct intel_hdmi *hdmi = &dig_port->hdmi;
	struct i2c_adapter *adapter = intel_gmbus_get_adapter(i915,
							      hdmi->ddc_bus);
	int ret;
	u8 start = offset & 0xff;
	struct i2c_msg msgs[] = {
		{
			.addr = DRM_HDCP_DDC_ADDR,
			.flags = 0,
			.len = 1,
			.buf = &start,
		},
		{
			.addr = DRM_HDCP_DDC_ADDR,
			.flags = I2C_M_RD,
			.len = size,
			.buf = buffer
		}
	};
	ret = i2c_transfer(adapter, msgs, ARRAY_SIZE(msgs));
	if (ret == ARRAY_SIZE(msgs))
		return 0;
	return ret >= 0 ? -EIO : ret;
}

static int intel_hdmi_hdcp_write(struct intel_digital_port *dig_port,
				 unsigned int offset, void *buffer, size_t size)
{
	struct drm_i915_private *i915 = to_i915(dig_port->base.base.dev);
	struct intel_hdmi *hdmi = &dig_port->hdmi;
	struct i2c_adapter *adapter = intel_gmbus_get_adapter(i915,
							      hdmi->ddc_bus);
	int ret;
	u8 *write_buf;
	struct i2c_msg msg;

	write_buf = kzalloc(size + 1, GFP_KERNEL);
	if (!write_buf)
		return -ENOMEM;

	write_buf[0] = offset & 0xff;
	memcpy(&write_buf[1], buffer, size);

	msg.addr = DRM_HDCP_DDC_ADDR;
	msg.flags = 0,
	msg.len = size + 1,
	msg.buf = write_buf;

	ret = i2c_transfer(adapter, &msg, 1);
	if (ret == 1)
		ret = 0;
	else if (ret >= 0)
		ret = -EIO;

	kfree(write_buf);
	return ret;
}

static
int intel_hdmi_hdcp_write_an_aksv(struct intel_digital_port *dig_port,
				  u8 *an)
{
	struct drm_i915_private *i915 = to_i915(dig_port->base.base.dev);
	struct intel_hdmi *hdmi = &dig_port->hdmi;
	struct i2c_adapter *adapter = intel_gmbus_get_adapter(i915,
							      hdmi->ddc_bus);
	int ret;

	ret = intel_hdmi_hdcp_write(dig_port, DRM_HDCP_DDC_AN, an,
				    DRM_HDCP_AN_LEN);
	if (ret) {
		drm_dbg_kms(&i915->drm, "Write An over DDC failed (%d)\n",
			    ret);
		return ret;
	}

	ret = intel_gmbus_output_aksv(adapter);
	if (ret < 0) {
		drm_dbg_kms(&i915->drm, "Failed to output aksv (%d)\n", ret);
		return ret;
	}
	return 0;
}

static int intel_hdmi_hdcp_read_bksv(struct intel_digital_port *dig_port,
				     u8 *bksv)
{
	struct drm_i915_private *i915 = to_i915(dig_port->base.base.dev);

	int ret;
#ifdef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
	ret = intel_hdmi_hdcp_read(dig_port, DRM_HDCP_DDC_BKSV, bksv,
				   DRM_HDCP_KSV_LEN);
#else
	/*
	 * According to HDMI 2.1 specs only HDCP 2.x can be enabled when
	 * frl is being used therefore HDCP 1.4 is not supported and 
	 * reading bksv is also not supported.
	 */
	if (dig_port->hdmi.frl.trained) {
		ret = -ENOTSUPP;
		drm_dbg_kms(&i915->drm, "Not reading Bksv as frl is enabled(%d)\n",
				ret);
	} else {
		ret = intel_hdmi_hdcp_read(dig_port, DRM_HDCP_DDC_BKSV, bksv,
				   DRM_HDCP_KSV_LEN);
	}
#endif
	if (ret)
		drm_dbg_kms(&i915->drm, "Read Bksv over DDC failed (%d)\n",
			    ret);
	return ret;
}

static
int intel_hdmi_hdcp_read_bstatus(struct intel_digital_port *dig_port,
				 u8 *bstatus)
{
	struct drm_i915_private *i915 = to_i915(dig_port->base.base.dev);

	int ret;
	ret = intel_hdmi_hdcp_read(dig_port, DRM_HDCP_DDC_BSTATUS,
				   bstatus, DRM_HDCP_BSTATUS_LEN);
	if (ret)
		drm_dbg_kms(&i915->drm, "Read bstatus over DDC failed (%d)\n",
			    ret);
	return ret;
}

static
int intel_hdmi_hdcp_repeater_present(struct intel_digital_port *dig_port,
				     bool *repeater_present)
{
	struct drm_i915_private *i915 = to_i915(dig_port->base.base.dev);
	int ret;
	u8 val;

	ret = intel_hdmi_hdcp_read(dig_port, DRM_HDCP_DDC_BCAPS, &val, 1);
	if (ret) {
		drm_dbg_kms(&i915->drm, "Read bcaps over DDC failed (%d)\n",
			    ret);
		return ret;
	}
	*repeater_present = val & DRM_HDCP_DDC_BCAPS_REPEATER_PRESENT;
	return 0;
}

static
int intel_hdmi_hdcp_read_ri_prime(struct intel_digital_port *dig_port,
				  u8 *ri_prime)
{
	struct drm_i915_private *i915 = to_i915(dig_port->base.base.dev);

	int ret;
	ret = intel_hdmi_hdcp_read(dig_port, DRM_HDCP_DDC_RI_PRIME,
				   ri_prime, DRM_HDCP_RI_LEN);
	if (ret)
		drm_dbg_kms(&i915->drm, "Read Ri' over DDC failed (%d)\n",
			    ret);
	return ret;
}

static
int intel_hdmi_hdcp_read_ksv_ready(struct intel_digital_port *dig_port,
				   bool *ksv_ready)
{
	struct drm_i915_private *i915 = to_i915(dig_port->base.base.dev);
	int ret;
	u8 val;

	ret = intel_hdmi_hdcp_read(dig_port, DRM_HDCP_DDC_BCAPS, &val, 1);
	if (ret) {
		drm_dbg_kms(&i915->drm, "Read bcaps over DDC failed (%d)\n",
			    ret);
		return ret;
	}
	*ksv_ready = val & DRM_HDCP_DDC_BCAPS_KSV_FIFO_READY;
	return 0;
}

static
int intel_hdmi_hdcp_read_ksv_fifo(struct intel_digital_port *dig_port,
				  int num_downstream, u8 *ksv_fifo)
{
	struct drm_i915_private *i915 = to_i915(dig_port->base.base.dev);
	int ret;
	ret = intel_hdmi_hdcp_read(dig_port, DRM_HDCP_DDC_KSV_FIFO,
				   ksv_fifo, num_downstream * DRM_HDCP_KSV_LEN);
	if (ret) {
		drm_dbg_kms(&i915->drm,
			    "Read ksv fifo over DDC failed (%d)\n", ret);
		return ret;
	}
	return 0;
}

static
int intel_hdmi_hdcp_read_v_prime_part(struct intel_digital_port *dig_port,
				      int i, u32 *part)
{
	struct drm_i915_private *i915 = to_i915(dig_port->base.base.dev);
	int ret;

	if (i >= DRM_HDCP_V_PRIME_NUM_PARTS)
		return -EINVAL;

	ret = intel_hdmi_hdcp_read(dig_port, DRM_HDCP_DDC_V_PRIME(i),
				   part, DRM_HDCP_V_PRIME_PART_LEN);
	if (ret)
		drm_dbg_kms(&i915->drm, "Read V'[%d] over DDC failed (%d)\n",
			    i, ret);
	return ret;
}

static int kbl_repositioning_enc_en_signal(struct intel_connector *connector,
					   enum transcoder cpu_transcoder)
{
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_digital_port *dig_port = intel_attached_dig_port(connector);
	struct intel_crtc *crtc = to_intel_crtc(connector->base.state->crtc);
	u32 scanline;
	int ret;

	for (;;) {
		scanline = intel_de_read(dev_priv, PIPEDSL(crtc->pipe));
		if (scanline > 100 && scanline < 200)
			break;
		usleep_range(25, 50);
	}

	ret = intel_ddi_toggle_hdcp_bits(&dig_port->base, cpu_transcoder,
					 false, TRANS_DDI_HDCP_SIGNALLING);
	if (ret) {
		drm_err(&dev_priv->drm,
			"Disable HDCP signalling failed (%d)\n", ret);
		return ret;
	}

	ret = intel_ddi_toggle_hdcp_bits(&dig_port->base, cpu_transcoder,
					 true, TRANS_DDI_HDCP_SIGNALLING);
	if (ret) {
		drm_err(&dev_priv->drm,
			"Enable HDCP signalling failed (%d)\n", ret);
		return ret;
	}

	return 0;
}

static
int intel_hdmi_hdcp_toggle_signalling(struct intel_digital_port *dig_port,
				      enum transcoder cpu_transcoder,
				      bool enable)
{
	struct intel_hdmi *hdmi = &dig_port->hdmi;
	struct intel_connector *connector = hdmi->attached_connector;
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	int ret;

	if (!enable)
		usleep_range(6, 60); /* Bspec says >= 6us */

	ret = intel_ddi_toggle_hdcp_bits(&dig_port->base,
					 cpu_transcoder, enable,
					 TRANS_DDI_HDCP_SIGNALLING);
	if (ret) {
		drm_err(&dev_priv->drm, "%s HDCP signalling failed (%d)\n",
			enable ? "Enable" : "Disable", ret);
		return ret;
	}

	/*
	 * WA: To fix incorrect positioning of the window of
	 * opportunity and enc_en signalling in KABYLAKE.
	 */
	if (IS_KABYLAKE(dev_priv) && enable)
		return kbl_repositioning_enc_en_signal(connector,
						       cpu_transcoder);

	return 0;
}

static
bool intel_hdmi_hdcp_check_link_once(struct intel_digital_port *dig_port,
				     struct intel_connector *connector)
{
	struct drm_i915_private *i915 = to_i915(dig_port->base.base.dev);
	enum port port = dig_port->base.port;
	enum transcoder cpu_transcoder = connector->hdcp.cpu_transcoder;
	int ret;
	union {
		u32 reg;
		u8 shim[DRM_HDCP_RI_LEN];
	} ri;

	ret = intel_hdmi_hdcp_read_ri_prime(dig_port, ri.shim);
	if (ret)
		return false;

	intel_de_write(i915, HDCP_RPRIME(i915, cpu_transcoder, port), ri.reg);

	/* Wait for Ri prime match */
	if (wait_for((intel_de_read(i915, HDCP_STATUS(i915, cpu_transcoder, port)) &
		      (HDCP_STATUS_RI_MATCH | HDCP_STATUS_ENC)) ==
		     (HDCP_STATUS_RI_MATCH | HDCP_STATUS_ENC), 1)) {
		drm_dbg_kms(&i915->drm, "Ri' mismatch detected (%x)\n",
			intel_de_read(i915, HDCP_STATUS(i915, cpu_transcoder,
							port)));
		return false;
	}
	return true;
}

static
bool intel_hdmi_hdcp_check_link(struct intel_digital_port *dig_port,
				struct intel_connector *connector)
{
	struct drm_i915_private *i915 = to_i915(dig_port->base.base.dev);
	int retry;

	for (retry = 0; retry < 3; retry++)
		if (intel_hdmi_hdcp_check_link_once(dig_port, connector))
			return true;

	drm_err(&i915->drm, "Link check failed\n");
	return false;
}

struct hdcp2_hdmi_msg_timeout {
	u8 msg_id;
	u16 timeout;
};

static const struct hdcp2_hdmi_msg_timeout hdcp2_msg_timeout[] = {
	{ HDCP_2_2_AKE_SEND_CERT, HDCP_2_2_CERT_TIMEOUT_MS, },
	{ HDCP_2_2_AKE_SEND_PAIRING_INFO, HDCP_2_2_PAIRING_TIMEOUT_MS, },
	{ HDCP_2_2_LC_SEND_LPRIME, HDCP_2_2_HDMI_LPRIME_TIMEOUT_MS, },
	{ HDCP_2_2_REP_SEND_RECVID_LIST, HDCP_2_2_RECVID_LIST_TIMEOUT_MS, },
	{ HDCP_2_2_REP_STREAM_READY, HDCP_2_2_STREAM_READY_TIMEOUT_MS, },
};

static
int intel_hdmi_hdcp2_read_rx_status(struct intel_digital_port *dig_port,
				    u8 *rx_status)
{
	return intel_hdmi_hdcp_read(dig_port,
				    HDCP_2_2_HDMI_REG_RXSTATUS_OFFSET,
				    rx_status,
				    HDCP_2_2_HDMI_RXSTATUS_LEN);
}

static int get_hdcp2_msg_timeout(u8 msg_id, bool is_paired)
{
	int i;

	if (msg_id == HDCP_2_2_AKE_SEND_HPRIME) {
		if (is_paired)
			return HDCP_2_2_HPRIME_PAIRED_TIMEOUT_MS;
		else
			return HDCP_2_2_HPRIME_NO_PAIRED_TIMEOUT_MS;
	}

	for (i = 0; i < ARRAY_SIZE(hdcp2_msg_timeout); i++) {
		if (hdcp2_msg_timeout[i].msg_id == msg_id)
			return hdcp2_msg_timeout[i].timeout;
	}

	return -EINVAL;
}

static int
hdcp2_detect_msg_availability(struct intel_digital_port *dig_port,
			      u8 msg_id, bool *msg_ready,
			      ssize_t *msg_sz)
{
	struct drm_i915_private *i915 = to_i915(dig_port->base.base.dev);
	u8 rx_status[HDCP_2_2_HDMI_RXSTATUS_LEN];
	int ret;

	ret = intel_hdmi_hdcp2_read_rx_status(dig_port, rx_status);
	if (ret < 0) {
		drm_dbg_kms(&i915->drm, "rx_status read failed. Err %d\n",
			    ret);
		return ret;
	}

	*msg_sz = ((HDCP_2_2_HDMI_RXSTATUS_MSG_SZ_HI(rx_status[1]) << 8) |
		  rx_status[0]);

	if (msg_id == HDCP_2_2_REP_SEND_RECVID_LIST)
		*msg_ready = (HDCP_2_2_HDMI_RXSTATUS_READY(rx_status[1]) &&
			     *msg_sz);
	else
		*msg_ready = *msg_sz;

	return 0;
}

static ssize_t
intel_hdmi_hdcp2_wait_for_msg(struct intel_digital_port *dig_port,
			      u8 msg_id, bool paired)
{
	struct drm_i915_private *i915 = to_i915(dig_port->base.base.dev);
	bool msg_ready = false;
	int timeout, ret;
	ssize_t msg_sz = 0;

	timeout = get_hdcp2_msg_timeout(msg_id, paired);
	if (timeout < 0)
		return timeout;

	ret = __wait_for(ret = hdcp2_detect_msg_availability(dig_port,
							     msg_id, &msg_ready,
							     &msg_sz),
			 !ret && msg_ready && msg_sz, timeout * 1000,
			 1000, 5 * 1000);
	if (ret)
		drm_dbg_kms(&i915->drm, "msg_id: %d, ret: %d, timeout: %d\n",
			    msg_id, ret, timeout);

	return ret ? ret : msg_sz;
}

static
int intel_hdmi_hdcp2_write_msg(struct intel_digital_port *dig_port,
			       void *buf, size_t size)
{
	unsigned int offset;

	offset = HDCP_2_2_HDMI_REG_WR_MSG_OFFSET;
	return intel_hdmi_hdcp_write(dig_port, offset, buf, size);
}

static
int intel_hdmi_hdcp2_read_msg(struct intel_digital_port *dig_port,
			      u8 msg_id, void *buf, size_t size)
{
	struct drm_i915_private *i915 = to_i915(dig_port->base.base.dev);
	struct intel_hdmi *hdmi = &dig_port->hdmi;
	struct intel_hdcp *hdcp = &hdmi->attached_connector->hdcp;
	unsigned int offset;
	ssize_t ret;

	ret = intel_hdmi_hdcp2_wait_for_msg(dig_port, msg_id,
					    hdcp->is_paired);
	if (ret < 0)
		return ret;

	/*
	 * Available msg size should be equal to or lesser than the
	 * available buffer.
	 */
	if (ret > size) {
		drm_dbg_kms(&i915->drm,
			    "msg_sz(%zd) is more than exp size(%zu)\n",
			    ret, size);
		return -EINVAL;
	}

	offset = HDCP_2_2_HDMI_REG_RD_MSG_OFFSET;
	ret = intel_hdmi_hdcp_read(dig_port, offset, buf, ret);
	if (ret)
		drm_dbg_kms(&i915->drm, "Failed to read msg_id: %d(%zd)\n",
			    msg_id, ret);

	return ret;
}

static
int intel_hdmi_hdcp2_check_link(struct intel_digital_port *dig_port,
				struct intel_connector *connector)
{
	u8 rx_status[HDCP_2_2_HDMI_RXSTATUS_LEN];
	int ret;

	ret = intel_hdmi_hdcp2_read_rx_status(dig_port, rx_status);
	if (ret)
		return ret;

	/*
	 * Re-auth request and Link Integrity Failures are represented by
	 * same bit. i.e reauth_req.
	 */
	if (HDCP_2_2_HDMI_RXSTATUS_REAUTH_REQ(rx_status[1]))
		ret = HDCP_REAUTH_REQUEST;
	else if (HDCP_2_2_HDMI_RXSTATUS_READY(rx_status[1]))
		ret = HDCP_TOPOLOGY_CHANGE;

	return ret;
}

static
int intel_hdmi_hdcp2_capable(struct intel_digital_port *dig_port,
			     bool *capable)
{
	u8 hdcp2_version;
	int ret;

	*capable = false;
	ret = intel_hdmi_hdcp_read(dig_port, HDCP_2_2_HDMI_REG_VER_OFFSET,
				   &hdcp2_version, sizeof(hdcp2_version));
	if (!ret && hdcp2_version & HDCP_2_2_HDMI_SUPPORT_MASK)
		*capable = true;

	return ret;
}

static const struct intel_hdcp_shim intel_hdmi_hdcp_shim = {
	.write_an_aksv = intel_hdmi_hdcp_write_an_aksv,
	.read_bksv = intel_hdmi_hdcp_read_bksv,
	.read_bstatus = intel_hdmi_hdcp_read_bstatus,
	.repeater_present = intel_hdmi_hdcp_repeater_present,
	.read_ri_prime = intel_hdmi_hdcp_read_ri_prime,
	.read_ksv_ready = intel_hdmi_hdcp_read_ksv_ready,
	.read_ksv_fifo = intel_hdmi_hdcp_read_ksv_fifo,
	.read_v_prime_part = intel_hdmi_hdcp_read_v_prime_part,
	.toggle_signalling = intel_hdmi_hdcp_toggle_signalling,
	.check_link = intel_hdmi_hdcp_check_link,
	.write_2_2_msg = intel_hdmi_hdcp2_write_msg,
	.read_2_2_msg = intel_hdmi_hdcp2_read_msg,
	.check_2_2_link	= intel_hdmi_hdcp2_check_link,
	.hdcp_2_2_capable = intel_hdmi_hdcp2_capable,
	.protocol = HDCP_PROTOCOL_HDMI,
};

static int intel_hdmi_source_max_tmds_clock(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	int max_tmds_clock, vbt_max_tmds_clock;

	if (DISPLAY_VER(dev_priv) >= 10)
		max_tmds_clock = 594000;
	else if (DISPLAY_VER(dev_priv) >= 8 || IS_HASWELL(dev_priv))
		max_tmds_clock = 300000;
	else if (DISPLAY_VER(dev_priv) >= 5)
		max_tmds_clock = 225000;
	else
		max_tmds_clock = 165000;

	vbt_max_tmds_clock = intel_bios_max_tmds_clock(encoder);
	if (vbt_max_tmds_clock)
		max_tmds_clock = min(max_tmds_clock, vbt_max_tmds_clock);

	return max_tmds_clock;
}

static bool intel_has_hdmi_sink(struct intel_hdmi *hdmi,
				const struct drm_connector_state *conn_state)
{
	return hdmi->has_hdmi_sink &&
		READ_ONCE(to_intel_digital_connector_state(conn_state)->force_audio) != HDMI_AUDIO_OFF_DVI;
}

static bool intel_hdmi_is_ycbcr420(const struct intel_crtc_state *crtc_state)
{
	return crtc_state->output_format == INTEL_OUTPUT_FORMAT_YCBCR420;
}

#ifdef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
static int hdmi_port_clock_limit(struct intel_hdmi *hdmi,
			 	 bool respect_downstream_limits,
				 bool has_hdmi_sink)
#else
static int hdmi_port_tmds_clock_limit(struct intel_hdmi *hdmi,
				      bool respect_downstream_limits,
				      bool has_hdmi_sink)
#endif
{
	struct intel_encoder *encoder = &hdmi_to_dig_port(hdmi)->base;
	int max_tmds_clock = intel_hdmi_source_max_tmds_clock(encoder);

	if (respect_downstream_limits) {
		struct intel_connector *connector = hdmi->attached_connector;
		const struct drm_display_info *info = &connector->base.display_info;

		if (hdmi->dp_dual_mode.max_tmds_clock)
			max_tmds_clock = min(max_tmds_clock,
					     hdmi->dp_dual_mode.max_tmds_clock);

		if (info->max_tmds_clock)
			max_tmds_clock = min(max_tmds_clock,
					     info->max_tmds_clock);
		else if (!has_hdmi_sink)
			max_tmds_clock = min(max_tmds_clock, 165000);
	}

	return max_tmds_clock;
}

#ifdef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
static enum drm_mode_status
hdmi_port_clock_valid(struct intel_hdmi *hdmi,
		      int clock, bool respect_downstream_limits,
		      bool has_hdmi_sink)
#else
static enum drm_mode_status
hdmi_port_tmds_clock_valid(struct intel_hdmi *hdmi,
			   int clock, bool respect_downstream_limits,
			   bool has_hdmi_sink)
#endif
{
	struct drm_i915_private *dev_priv = intel_hdmi_to_i915(hdmi);
	enum phy phy = intel_port_to_phy(dev_priv, hdmi_to_dig_port(hdmi)->base.port);

	if (clock < 25000)
		return MODE_CLOCK_LOW;
#ifdef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
	if (clock > hdmi_port_clock_limit(hdmi, respect_downstream_limits,
					  has_hdmi_sink))
#else
	if (clock > hdmi_port_tmds_clock_limit(hdmi, respect_downstream_limits,
					       has_hdmi_sink))
#endif
		return MODE_CLOCK_HIGH;

	/* GLK DPLL can't generate 446-480 MHz */
	if (IS_GEMINILAKE(dev_priv) && clock > 446666 && clock < 480000)
		return MODE_CLOCK_RANGE;

	/* BXT/GLK DPLL can't generate 223-240 MHz */
	if ((IS_GEMINILAKE(dev_priv) || IS_BROXTON(dev_priv)) &&
	    clock > 223333 && clock < 240000)
		return MODE_CLOCK_RANGE;

	/* CHV DPLL can't generate 216-240 MHz */
	if (IS_CHERRYVIEW(dev_priv) && clock > 216000 && clock < 240000)
		return MODE_CLOCK_RANGE;

	/* ICL+ combo PHY PLL can't generate 500-533.2 MHz */
	if (intel_phy_is_combo(dev_priv, phy) && clock > 500000 && clock < 533200)
		return MODE_CLOCK_RANGE;

	/* ICL+ TC PHY PLL can't generate 500-532.8 MHz */
	if (intel_phy_is_tc(dev_priv, phy) && clock > 500000 && clock < 532800)
		return MODE_CLOCK_RANGE;

	/*
	 * SNPS PHYs' MPLLB table-based programming can only handle a fixed
	 * set of link rates.
	 *
	 * FIXME: We will hopefully get an algorithmic way of programming
	 * the MPLLB for HDMI in the future.
	 */
	if (DISPLAY_VER(dev_priv) >= 14)
		return intel_cx0_phy_check_hdmi_link_rate(hdmi, clock);
	else if (IS_DG2(dev_priv))
		return intel_snps_phy_check_hdmi_link_rate(clock);

	return MODE_OK;
}

#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
/*
 * Returns the Fixed rate per lane required to support given pixel rate.
 * Returns 0 for pixel rate demanding > 12 Gbps
 */
static int intel_hdmi_frl_required_bitrate(int pixel_rate_kbps)
{
	/*
	 * 3 lane configurations:
	 * 3 Gbps * 3 = 9 Gbps; 6 Gbps * 3 = 18 Gbps.
	 *
	 * 4 lane configurations:
	 * 6 Gbps * 4 = 24 Gbps; 8 Gbps * 4 = 32 Gbp;
	 * 10 Gbps * 4 = 40 Gbps; 12 Gbps * 4 = 48 Gbps.
	 */
#define frl_gbps_to_kbps(rate) ((rate) * 1000000)
	if (pixel_rate_kbps <= frl_gbps_to_kbps(9))
		return frl_gbps_to_kbps(3);

	if (pixel_rate_kbps > frl_gbps_to_kbps(9) &&
	    pixel_rate_kbps <= frl_gbps_to_kbps(18))
		return frl_gbps_to_kbps(6);

	if (pixel_rate_kbps > frl_gbps_to_kbps(18) &&
	    pixel_rate_kbps <= frl_gbps_to_kbps(24))
		return frl_gbps_to_kbps(6);

	if (pixel_rate_kbps > frl_gbps_to_kbps(24) &&
	    pixel_rate_kbps <= frl_gbps_to_kbps(32))
		return frl_gbps_to_kbps(8);

	if (pixel_rate_kbps > frl_gbps_to_kbps(32) &&
	    pixel_rate_kbps <= frl_gbps_to_kbps(40))
		return frl_gbps_to_kbps(10);

	if (pixel_rate_kbps > frl_gbps_to_kbps(40) &&
	    pixel_rate_kbps <= frl_gbps_to_kbps(48))
		return frl_gbps_to_kbps(12);

	/*
	 * pixel rate more than 48 Gbps rate, means more than
	 * 12 Gbps x 4 lanes. Such a rate not possible with FRL.
	 */
	return 0;
}

static int
hdmi21_port_clock_limit(struct intel_hdmi *hdmi)
{
	int max_lane_rate_gbps, max_symbol_clock_khz;

	max_lane_rate_gbps = intel_hdmi_frl_required_bitrate(hdmi->max_frl_rate);

	max_symbol_clock_khz = (max_lane_rate_gbps * 1000000) / 18;

	/*
	 * FIXME: Currently the resolution of C20 clocks is in 10KHz.
	 * Check if we need to have finer granularity.
	 */
	return roundup(max_symbol_clock_khz, 10);
}

static enum drm_mode_status
hdmi_port_frl_clock_valid(struct intel_hdmi *hdmi, int clock)
{
	struct intel_encoder *encoder = &hdmi_to_dig_port(hdmi)->base;
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);

	if (!clock || clock > hdmi21_port_clock_limit(hdmi))
		return MODE_CLOCK_HIGH;

	if (DISPLAY_VER(i915) >= 14)
		return intel_c20_phy_check_hdmi_link_rate(clock);

	return MODE_OK;
}
#endif

#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
static enum drm_mode_status
hdmi_port_clock_valid(struct intel_hdmi *hdmi,
		      int clock, bool respect_downstream_limits,
		      bool has_hdmi_sink, bool frl_mode)
{
	if (frl_mode)
		return hdmi_port_frl_clock_valid(hdmi, clock);

	return hdmi_port_tmds_clock_valid(hdmi, clock,
					  respect_downstream_limits,
					  has_hdmi_sink);
}
#endif

int intel_hdmi_tmds_clock(int clock, int bpc, bool ycbcr420_output)
{
	/* YCBCR420 TMDS rate requirement is half the pixel clock */
	if (ycbcr420_output)
		clock /= 2;

	/*
	 * Need to adjust the port link by:
	 *  1.5x for 12bpc
	 *  1.25x for 10bpc
	 */
	return clock * bpc / 8;
}

static bool intel_hdmi_source_bpc_possible(struct drm_i915_private *i915, int bpc)
{
	switch (bpc) {
	case 12:
		return !HAS_GMCH(i915);
	case 10:
		return DISPLAY_VER(i915) >= 11;
	case 8:
		return true;
	default:
		MISSING_CASE(bpc);
		return false;
	}
}

static bool intel_hdmi_sink_bpc_possible(struct drm_connector *connector,
					 int bpc, bool has_hdmi_sink, bool ycbcr420_output)
{
	const struct drm_display_info *info = &connector->display_info;
	const struct drm_hdmi_info *hdmi = &info->hdmi;

	switch (bpc) {
	case 12:
		if (!has_hdmi_sink)
			return false;

		if (ycbcr420_output)
			return hdmi->y420_dc_modes & DRM_EDID_YCBCR420_DC_36;
		else
#ifdef EDID_HDMI_RGB444_DC_MODES_PRESENT
			return info->edid_hdmi_rgb444_dc_modes & DRM_EDID_HDMI_DC_36;
#else
			return info->edid_hdmi_dc_modes & DRM_EDID_HDMI_DC_36;
#endif
	case 10:
		if (!has_hdmi_sink)
			return false;

		if (ycbcr420_output)
			return hdmi->y420_dc_modes & DRM_EDID_YCBCR420_DC_30;
		else
#ifdef EDID_HDMI_RGB444_DC_MODES_PRESENT
			return info->edid_hdmi_rgb444_dc_modes & DRM_EDID_HDMI_DC_30;
#else
			return info->edid_hdmi_dc_modes & DRM_EDID_HDMI_DC_30;
#endif
	case 8:
		return true;
	default:
		MISSING_CASE(bpc);
		return false;
	}
}

#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
static
int intel_hdmi_frl_clock(int clock, int bpc, bool ycbcr420_output)
{
	int pixel_rate_kbps, frl_bit_rate_required;
	int frl_symbol_clock;

	if (ycbcr420_output)
		clock /= 2;

	pixel_rate_kbps = clock * bpc * 3;

	/* find the closest frl bit rate */
	frl_bit_rate_required = intel_hdmi_frl_required_bitrate(pixel_rate_kbps);

	/* frl_symbol_clock */
	frl_symbol_clock = frl_bit_rate_required / 18;

	/*
	 * FIXME: Currently the resolution of C20 clocks is in 10KHz.
	 * Check if we need to have finer granularity.
	 */
	return roundup(frl_symbol_clock, 10);
}

static
int intel_hdmi_clock(int clock, int bpc, bool ycbcr420_output, bool frl_mode)
{
	if (frl_mode)
		return intel_hdmi_frl_clock(clock, bpc, ycbcr420_output);

	return intel_hdmi_tmds_clock(clock, bpc, ycbcr420_output);
}
#endif

static enum drm_mode_status
intel_hdmi_mode_clock_valid(struct drm_connector *connector, int clock,
			    bool has_hdmi_sink, bool ycbcr420_output)
{
	struct drm_i915_private *i915 = to_i915(connector->dev);
	struct intel_hdmi *hdmi = intel_attached_hdmi(to_intel_connector(connector));
	enum drm_mode_status status = MODE_OK;
	int bpc;
#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
	bool frl_mode;

	frl_mode = hdmi->has_sink_hdmi_21 && hdmi->max_frl_rate;
#endif

	/*
	 * Try all color depths since valid port clock range
	 * can have holes. Any mode that can be used with at
	 * least one color depth is accepted.
	 */
	for (bpc = 12; bpc >= 8; bpc -= 2) {
#ifdef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
		int tmds_clock = intel_hdmi_tmds_clock(clock, bpc, ycbcr420_output);
#else
		int hdmi_clock = intel_hdmi_clock(clock, bpc, ycbcr420_output, frl_mode);
#endif

		if (!intel_hdmi_source_bpc_possible(i915, bpc))
			continue;

		if (!intel_hdmi_sink_bpc_possible(connector, bpc, has_hdmi_sink, ycbcr420_output))
			continue;
#ifdef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
		status = hdmi_port_clock_valid(hdmi, tmds_clock, true, has_hdmi_sink);
#else
		status = hdmi_port_clock_valid(hdmi, hdmi_clock, true, has_hdmi_sink, frl_mode);
#endif
		if (status == MODE_OK)
			return MODE_OK;
	}

	/* can never happen */
	drm_WARN_ON(&i915->drm, status == MODE_OK);

	return status;
}

#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
static enum drm_mode_status
intel_hdmi_dsc_mode_valid(struct drm_connector *connector,
			  struct drm_display_mode *mode, int clock,
			  bool ycbcr420_only)
{
	struct intel_hdmi *hdmi = intel_attached_hdmi(to_intel_connector(connector));
	int slice_count, slice_width, src_frc_bpp;
	int pixel_rate_kbps, dsc_frl_rate_kbps, hdmi_max_chunk_bytes;
	int compressed_bpp_x16, frl_symbol_clock, frl_bit_rate_required;
	bool hdmi_all_bpp, bigjoiner;
	u8 min_bpc;

	/* TBD: get the lowest dsc bpc from the common pool of src and sink */
	min_bpc = 8;

	/* TBD: bigjoiner support */
	bigjoiner = false;
	 /* TBD: need to add dsc support for other formats*/
	if (ycbcr420_only)
		return MODE_CLOCK_HIGH;

	slice_count = get_dsc_slice_count(hdmi, mode, INTEL_OUTPUT_FORMAT_RGB,
					  bigjoiner);
	if (slice_count == 0)
		return MODE_CLOCK_HIGH;

	slice_width = mode->hdisplay / slice_count;
	src_frc_bpp = 0;

	hdmi_max_chunk_bytes =
			connector->display_info.hdmi.dsc_cap.total_chunk_kbytes * 1024;
	hdmi_all_bpp = connector->display_info.hdmi.dsc_cap.all_bpp;

	/*
	 * Check if we get a valid compressed bpp with the min bpc for the given
	 * mode, and src/sink capabilities. If we do not get a valid compressed
	 * bpp with the min bpc, then the mode cannot be supported.
	 */
	compressed_bpp_x16 = intel_hdmi_dsc_get_bpp(src_frc_bpp, slice_width, slice_count,
						    INTEL_OUTPUT_FORMAT_RGB, min_bpc,
						    hdmi_all_bpp, hdmi_max_chunk_bytes);
	if (compressed_bpp_x16 == 0)
		return MODE_CLOCK_HIGH;

	pixel_rate_kbps = clock * DIV_ROUND_UP(compressed_bpp_x16, 16);

	dsc_frl_rate_kbps = hdmi->max_dsc_frl_rate * 1000000;

	/* Check if mode can be supported with max available dsc rate */
	if (pixel_rate_kbps > dsc_frl_rate_kbps)
		return MODE_CLOCK_HIGH;

	/*
	 * Check if mode can be supported by the port clock.
	 * First get the required Fixed rate that will support the given b/w
	 * with compression.
	 * Next, get the frl symbol clock and see it its supported by our port
	 * clock.
	 */
	frl_bit_rate_required = intel_hdmi_frl_required_bitrate(pixel_rate_kbps);

	frl_symbol_clock = DIV_ROUND_UP(frl_bit_rate_required, 18);

	return hdmi_port_frl_clock_valid(hdmi, frl_symbol_clock);
}

static bool
intel_hdmi_src_dsc_supported(struct drm_i915_private *dev_priv)
{
	return DISPLAY_VER(dev_priv) >= 14;
}
#endif

static enum drm_mode_status
intel_hdmi_mode_valid(struct drm_connector *connector,
		      struct drm_display_mode *mode)
{
	struct intel_hdmi *hdmi = intel_attached_hdmi(to_intel_connector(connector));
	struct drm_i915_private *dev_priv = intel_hdmi_to_i915(hdmi);
	struct intel_encoder *encoder = &hdmi_to_dig_port(hdmi)->base;
	enum drm_mode_status status;
	int clock = mode->clock;
	int max_dotclk = to_i915(connector->dev)->max_dotclk_freq;
	bool has_hdmi_sink = intel_has_hdmi_sink(hdmi, connector->state);
	bool bigjoiner = false;
#ifdef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
	bool ycbcr_420_only;
#else
	bool ycbcr_420_only, dsc;

	dsc = intel_hdmi_src_dsc_supported(dev_priv) &&
		connector->display_info.hdmi.dsc_cap.v_1p2;
#endif
	
	if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
		return MODE_NO_DBLESCAN;

	if ((mode->flags & DRM_MODE_FLAG_3D_MASK) == DRM_MODE_FLAG_3D_FRAME_PACKING)
		clock *= 2;

	if (intel_need_bigjoiner(encoder, mode->hdisplay,
				 mode->crtc_clock)) {
		bigjoiner = true;
		max_dotclk *= 2;
	}

#ifdef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
	if (clock > max_dotclk && !bigjoiner)
#else
	if (DISPLAY_VER(dev_priv) < 13 && bigjoiner && !dsc)
#endif
		return MODE_CLOCK_HIGH;

	if (mode->flags & DRM_MODE_FLAG_DBLCLK) {
		if (!has_hdmi_sink)
			return MODE_CLOCK_LOW;
		clock *= 2;
	}

	/*
	 * HDMI2.1 requires higher resolution modes like 8k60, 4K120 to be
	 * enumerated only if FRL is supported. Platforms < MTL do not support
	 * FRL so prune the higher resolution modes that require doctclock more
	 * than 600MHz.
	 */
	if (DISPLAY_VER(dev_priv) < 14 && clock > 600000)
		return MODE_CLOCK_HIGH;

	ycbcr_420_only = drm_mode_is_420_only(&connector->display_info, mode);

	status = intel_hdmi_mode_clock_valid(connector, clock, has_hdmi_sink, ycbcr_420_only);
	if (status != MODE_OK) {
		if (ycbcr_420_only ||
		    !connector->ycbcr_420_allowed ||
#ifdef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
		    !drm_mode_is_420_also(&connector->display_info, mode))
#else
		    !drm_mode_is_420_also(&connector->display_info, mode) ||
		    !dsc)
#endif
			return status;

		status = intel_hdmi_mode_clock_valid(connector, clock, has_hdmi_sink, true);
#ifdef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
		if (status != MODE_OK)
#else
		if (status != MODE_OK || !dsc)
#endif
			return status;
	}

#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
	if (status != MODE_OK && dsc)
		status = intel_hdmi_dsc_mode_valid(connector, mode, clock, ycbcr_420_only);

	if (status != MODE_OK)
		return status;
#endif

	return intel_mode_valid_max_plane_size(dev_priv, mode, bigjoiner);
}

bool intel_hdmi_bpc_possible(const struct intel_crtc_state *crtc_state,
			     int bpc, bool has_hdmi_sink, bool ycbcr420_output)
{
	struct drm_atomic_state *state = crtc_state->uapi.state;
	struct drm_connector_state *connector_state;
	struct drm_connector *connector;
	int i;

	for_each_new_connector_in_state(state, connector, connector_state, i) {
		if (connector_state->crtc != crtc_state->uapi.crtc)
			continue;

		if (!intel_hdmi_sink_bpc_possible(connector, bpc, has_hdmi_sink, ycbcr420_output))
			return false;
	}

	return true;
}

static bool hdmi_bpc_possible(const struct intel_crtc_state *crtc_state, int bpc)
{
	struct drm_i915_private *dev_priv =
		to_i915(crtc_state->uapi.crtc->dev);
	const struct drm_display_mode *adjusted_mode =
		&crtc_state->hw.adjusted_mode;

	if (!intel_hdmi_source_bpc_possible(dev_priv, bpc))
		return false;

	/*
	 * HDMI deep color affects the clocks, so it's only possible
	 * when not cloning with other encoder types.
	 */
	if (bpc > 8 && crtc_state->output_types != BIT(INTEL_OUTPUT_HDMI))
		return false;

	/* Display Wa_1405510057:icl,ehl */
	if (intel_hdmi_is_ycbcr420(crtc_state) &&
	    bpc == 10 && DISPLAY_VER(dev_priv) == 11 &&
	    (adjusted_mode->crtc_hblank_end -
	     adjusted_mode->crtc_hblank_start) % 8 == 2)
		return false;

	return intel_hdmi_bpc_possible(crtc_state, bpc, crtc_state->has_hdmi_sink,
				       intel_hdmi_is_ycbcr420(crtc_state));
}

static int intel_hdmi_compute_bpc(struct intel_encoder *encoder,
				  struct intel_crtc_state *crtc_state,
				  int clock, bool respect_downstream_limits)
{
	struct intel_hdmi *intel_hdmi = enc_to_intel_hdmi(encoder);
	bool ycbcr420_output = intel_hdmi_is_ycbcr420(crtc_state);
	int bpc;

	/*
	 * pipe_bpp could already be below 8bpc due to FDI
	 * bandwidth constraints. HDMI minimum is 8bpc however.
	 */
	bpc = max(crtc_state->pipe_bpp / 3, 8);

	/*
	 * We will never exceed downstream TMDS clock limits while
	 * attempting deep color. If the user insists on forcing an
	 * out of spec mode they will have to be satisfied with 8bpc.
	 */
	if (!respect_downstream_limits)
		bpc = 8;

	for (; bpc >= 8; bpc -= 2) {
		int tmds_clock = intel_hdmi_tmds_clock(clock, bpc, ycbcr420_output);

		if (hdmi_bpc_possible(crtc_state, bpc) &&
#ifdef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
		    hdmi_port_clock_valid(intel_hdmi, tmds_clock,
		    			  respect_downstream_limits,
					  crtc_state->has_hdmi_sink) == MODE_OK)
#else
		    hdmi_port_tmds_clock_valid(intel_hdmi, tmds_clock,
					       respect_downstream_limits,
					       crtc_state->has_hdmi_sink) == MODE_OK)
#endif
			return bpc;
	}

	return -EINVAL;
}

static int intel_hdmi_compute_clock(struct intel_encoder *encoder,
				    struct intel_crtc_state *crtc_state,
				    bool respect_downstream_limits)
{
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);
	const struct drm_display_mode *adjusted_mode =
		&crtc_state->hw.adjusted_mode;
	int bpc, clock = adjusted_mode->crtc_clock;

	if (adjusted_mode->flags & DRM_MODE_FLAG_DBLCLK)
		clock *= 2;

	bpc = intel_hdmi_compute_bpc(encoder, crtc_state, clock,
				     respect_downstream_limits);
	if (bpc < 0)
		return bpc;

#ifdef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
	crtc_state->port_clock =
		intel_hdmi_tmds_clock(clock, bpc, intel_hdmi_is_ycbcr420(crtc_state));
#else
	/*
	 * In case of frl mode div18 symbol clock is computed
	 * during frl capacity computation
	 */
	if (crtc_state->frl.enable)
		crtc_state->port_clock = crtc_state->frl.div18;
	else
		crtc_state->port_clock =
			    intel_hdmi_tmds_clock(clock, bpc,
						  intel_hdmi_is_ycbcr420(crtc_state));
#endif
	/*
	 * pipe_bpp could already be below 8bpc due to
	 * FDI bandwidth constraints. We shouldn't bump it
	 * back up to the HDMI minimum 8bpc in that case.
	 */
	crtc_state->pipe_bpp = min(crtc_state->pipe_bpp, bpc * 3);

	drm_dbg_kms(&i915->drm,
		    "picking %d bpc for HDMI output (pipe bpp: %d)\n",
		    bpc, crtc_state->pipe_bpp);

	return 0;
}

bool intel_hdmi_limited_color_range(const struct intel_crtc_state *crtc_state,
				    const struct drm_connector_state *conn_state)
{
	const struct intel_digital_connector_state *intel_conn_state =
		to_intel_digital_connector_state(conn_state);
	const struct drm_display_mode *adjusted_mode =
		&crtc_state->hw.adjusted_mode;

	/*
	 * Our YCbCr output is always limited range.
	 * crtc_state->limited_color_range only applies to RGB,
	 * and it must never be set for YCbCr or we risk setting
	 * some conflicting bits in PIPECONF which will mess up
	 * the colors on the monitor.
	 */
	if (crtc_state->output_format != INTEL_OUTPUT_FORMAT_RGB)
		return false;

	if (intel_conn_state->broadcast_rgb == INTEL_BROADCAST_RGB_AUTO) {
		/* See CEA-861-E - 5.1 Default Encoding Parameters */
		return crtc_state->has_hdmi_sink &&
			drm_default_rgb_quant_range(adjusted_mode) ==
			HDMI_QUANTIZATION_RANGE_LIMITED;
	} else {
		return intel_conn_state->broadcast_rgb == INTEL_BROADCAST_RGB_LIMITED;
	}
}

static bool intel_hdmi_has_audio(struct intel_encoder *encoder,
				 const struct intel_crtc_state *crtc_state,
				 const struct drm_connector_state *conn_state)
{
	struct intel_hdmi *intel_hdmi = enc_to_intel_hdmi(encoder);
	const struct intel_digital_connector_state *intel_conn_state =
		to_intel_digital_connector_state(conn_state);

	if (!crtc_state->has_hdmi_sink)
		return false;

	if (intel_conn_state->force_audio == HDMI_AUDIO_AUTO)
		return intel_hdmi->has_audio;
	else
		return intel_conn_state->force_audio == HDMI_AUDIO_ON;
}

static enum intel_output_format
intel_hdmi_output_format(struct intel_connector *connector,
			 bool ycbcr_420_output)
{
	if (connector->base.ycbcr_420_allowed && ycbcr_420_output)
		return INTEL_OUTPUT_FORMAT_YCBCR420;
	else
		return INTEL_OUTPUT_FORMAT_RGB;
}

static int intel_hdmi_compute_output_format(struct intel_encoder *encoder,
					    struct intel_crtc_state *crtc_state,
					    const struct drm_connector_state *conn_state,
					    bool respect_downstream_limits)
{
	struct intel_connector *connector = to_intel_connector(conn_state->connector);
	const struct drm_display_mode *adjusted_mode = &crtc_state->hw.adjusted_mode;
	const struct drm_display_info *info = &connector->base.display_info;
	struct drm_i915_private *i915 = to_i915(connector->base.dev);
	bool ycbcr_420_only = drm_mode_is_420_only(info, adjusted_mode);
	int ret;

	crtc_state->output_format = intel_hdmi_output_format(connector, ycbcr_420_only);

	if (ycbcr_420_only && !intel_hdmi_is_ycbcr420(crtc_state)) {
		drm_dbg_kms(&i915->drm,
			    "YCbCr 4:2:0 mode but YCbCr 4:2:0 output not possible. Falling back to RGB.\n");
		crtc_state->output_format = INTEL_OUTPUT_FORMAT_RGB;
	}

	ret = intel_hdmi_compute_clock(encoder, crtc_state, respect_downstream_limits);
	if (ret) {
		if (intel_hdmi_is_ycbcr420(crtc_state) ||
		    !connector->base.ycbcr_420_allowed ||
		    !drm_mode_is_420_also(info, adjusted_mode))
			return ret;

		crtc_state->output_format = intel_hdmi_output_format(connector, true);
		ret = intel_hdmi_compute_clock(encoder, crtc_state, respect_downstream_limits);
	}

	return ret;
}

#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
static void intel_hdmi_compute_cvtemp_header(struct intel_crtc_state *pipe_config)
{
	struct hdmi_extended_metadata_packet *cvt_emp = &pipe_config->cvt_emp;

	cvt_emp->type = HDMI_EMP_TYPE_CVTEM;
	cvt_emp->header.hb0 = TRANS_HDMI_EMP_HB0;
	cvt_emp->first_data_set.pb0_new = true;
	cvt_emp->first_data_set.pb0_end = false;
	cvt_emp->first_data_set.pb0_afr = false;
	cvt_emp->first_data_set.pb0_vfr = true;
	cvt_emp->first_data_set.pb0_sync = true;
	cvt_emp->first_data_set.ds_type = HDMI_EMP_DS_TYPE_PSTATIC;
	cvt_emp->first_data_set.org_id = 1;
	cvt_emp->first_data_set.data_set_tag = 2;
	/*
	 * HDMI2.1 defined EMP CVTEM packets:
	 * 128 DSC packets + 2 HFront + 2 HSync + 2 Hback + 2 HCactive
	 * = 136 Bytes.
	 */
	cvt_emp->first_data_set.data_set_length = 136;
	cvt_emp->enabled = true;
}

static bool intel_dsc_supports_ycbcr420(struct drm_i915_private *i915)
{
	if (DISPLAY_VER(i915) >= 14)
		return true;

	return false;
}

static void intel_hdmi_dsc_compute_config(struct intel_encoder *encoder,
					  struct intel_crtc_state *pipe_config,
					  struct drm_hdmi_frl_dfm *frl_dfm)
{
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);
	struct intel_hdmi *intel_hdmi = enc_to_intel_hdmi(encoder);
	struct intel_connector *intel_connector = intel_hdmi->attached_connector;
	struct drm_connector *connector = &intel_connector->base;
	const struct drm_display_mode *adjusted_mode =
		&pipe_config->hw.adjusted_mode;
	struct drm_dsc_config *vdsc_cfg = &pipe_config->dsc.config;
	int ret = 0;

	/* HDMI2.1 supports VDSC 1.2 onwards */
	if (!connector->display_info.hdmi.dsc_cap.v_1p2)
		return;

	if (!frl_dfm->config.target_bpp_16 || !frl_dfm->config.slice_width)
		return;

	pipe_config->dsc.compressed_bpp = frl_dfm->config.target_bpp_16;

	pipe_config->dsc.slice_count = adjusted_mode->hdisplay / frl_dfm->config.slice_width;

	/*
	 * TODO : Common code for DP and HDMI Move out
	 * VDSC engine operates at 1 Pixel per clock, so if peak pixel rate
	 * is greater than the maximum Cdclock and if slice count is even
	 * then we need to use 2 VDSC instances.
	 */
	if (adjusted_mode->crtc_clock > i915->max_cdclk_freq ||
	    pipe_config->bigjoiner_pipes) {
		if (pipe_config->dsc.slice_count < 2) {
			drm_dbg_kms(&i915->drm,
				    "Cannot split stream to use 2 VDSC instances\n");
			return;
		}

		pipe_config->dsc.dsc_split = true;
	}

	if (intel_dsc_supports_ycbcr420(i915) &&
	    connector->display_info.hdmi.dsc_cap.native_420 &&
	    pipe_config->output_format == INTEL_OUTPUT_FORMAT_YCBCR420) {
		vdsc_cfg->convert_rgb = 0;
		vdsc_cfg->native_420 = 1;
	} else {
		vdsc_cfg->convert_rgb = 1;
		vdsc_cfg->native_420 = 0;
	}

	vdsc_cfg->slice_height = intel_hdmi_dsc_get_slice_height(adjusted_mode->vdisplay);
	/*
	 * Following PPS parameters are hard coded as per HDMI2.1 spec Table 7-25, 28-30
	 */
	vdsc_cfg->dsc_version_major = 1;
	vdsc_cfg->dsc_version_minor = 2;
	vdsc_cfg->line_buf_depth = 13;
	vdsc_cfg->block_pred_enable = 1;
	vdsc_cfg->rc_model_size = DSC_RC_MODEL_SIZE_CONST; //As per C-Model-AN

	/* Fill remaining common DSC parameters */
	ret = intel_dsc_compute_params(pipe_config);
	if (ret < 0) {
		drm_dbg_kms(&i915->drm,
			    "Cannot compute valid DSC parameters for Input Bpp = %d Compressed BPP = %d\n",
			    pipe_config->pipe_bpp,
			    pipe_config->dsc.compressed_bpp);
		return;
	}

	ret = drm_dsc_compute_rc_parameters(vdsc_cfg);
	if (ret < 0)
		return;

	pipe_config->dsc.compression_enable = true;
	drm_dbg_kms(&i915->drm,
		    "HDMI DSC computed with Input Bpp = %d Compressed Bpp = %d Slice Count = %d\n",
		    pipe_config->pipe_bpp,
		    pipe_config->dsc.compressed_bpp,
		    pipe_config->dsc.slice_count);
}

static u32
get_drm_color_format(enum intel_output_format output_format)
{
	switch (output_format) {
	case INTEL_OUTPUT_FORMAT_RGB:
		return DRM_COLOR_FORMAT_RGB444;
	case INTEL_OUTPUT_FORMAT_YCBCR420:
		return DRM_COLOR_FORMAT_YCRCB420;
	case INTEL_OUTPUT_FORMAT_YCBCR444:
		return DRM_COLOR_FORMAT_YCRCB444;
	default:
		return DRM_COLOR_FORMAT_RGB444;
	}
}

static void
compute_frl_mn(struct intel_crtc_state *crtc_state, u32 ftb_avg_k)
{
	u64 ftb_avg, div_18_clk, gcd_val;

	ftb_avg = ftb_avg_k * 1000;
	div_18_clk = mult_frac(1000000000, crtc_state->frl.required_rate, 18);
	gcd_val = gcd(ftb_avg, div_18_clk);

	crtc_state->frl.link_m_ext = DIV_ROUND_UP_ULL(ftb_avg, gcd_val);
	crtc_state->frl.link_n_ext = DIV_ROUND_UP_ULL(div_18_clk, gcd_val);

	/* Frl div 18 stored in Khz */
	crtc_state->frl.div18 = DIV_ROUND_UP_ULL(div_18_clk, 1000);
}

static int get_dsc_slice_count(struct intel_hdmi *intel_hdmi,
		               const struct drm_display_mode *mode,
			       enum intel_output_format output_format,
			       bool use_bigjoiner)
{
	/*
	 * Bspec: 31627
	 * max_slices per line 4, without big joiner, 8 with big joiner
	 * max slice width in pixels 5120 without pipe joiner, 8192 with pipe joiner
	 */
#define SRC_MAX_SLICES                 4
#define SRC_MAX_SLICES_BIG_JOINER      8
#define SRC_MAX_SLICES_WIDTH            5120
#define SRC_MAX_SLICES_WIDTH_BIG_JOINER        8192

	int src_max_slices = SRC_MAX_SLICES;
	int src_max_width = SRC_MAX_SLICES_WIDTH;
	struct intel_connector *intel_connector = intel_hdmi->attached_connector;
	struct drm_connector *connector = &intel_connector->base;
	int hdmi_throughput = connector->display_info.hdmi.dsc_cap.clk_per_slice;
	int hdmi_max_slices = connector->display_info.hdmi.dsc_cap.max_slices;

	if (use_bigjoiner) {
		src_max_slices = SRC_MAX_SLICES_BIG_JOINER;
		src_max_width = SRC_MAX_SLICES_WIDTH_BIG_JOINER;
	}

	return intel_hdmi_dsc_get_num_slices(mode, output_format,
			src_max_slices, src_max_width,
			hdmi_max_slices, hdmi_throughput);
}

static bool
intel_hdmi_can_support_frl_mode_with_dsc(struct intel_hdmi *intel_hdmi,
					 struct intel_crtc_state *pipe_config,
					 struct drm_hdmi_frl_dfm *frl_dfm)
{
	const struct drm_display_mode *adjusted_mode = &pipe_config->hw.adjusted_mode;
	struct intel_connector *intel_connector = intel_hdmi->attached_connector;
	struct drm_connector *connector = &intel_connector->base;
	int hdmi_max_chunk_bytes = connector->display_info.hdmi.dsc_cap.total_chunk_kbytes * 1024;
	bool hdmi_all_bpp = connector->display_info.hdmi.dsc_cap.all_bpp;
	int slice_count, slice_width;
	int src_frc_bpp, bpp, bpp_x16, max_dsc_bpp, min_dsc_bpp;
	u8 bpc;

	slice_count = get_dsc_slice_count(intel_hdmi, adjusted_mode,
			pipe_config->output_format,
			pipe_config->bigjoiner_pipes);
	if (!slice_count)
		return false;

	slice_width = adjusted_mode->hdisplay / slice_count;

	/*TODO Check for fractional bpp support from source */
	src_frc_bpp = 0;
	bpc = pipe_config->pipe_bpp / 3;

	get_dsc_min_max_bpp(pipe_config->output_format, bpc, hdmi_all_bpp,
			&min_dsc_bpp, &max_dsc_bpp);

	for (bpp = max_dsc_bpp; bpp > min_dsc_bpp; bpp--) {
		bpp_x16 = get_dsc_compressed_bpp(slice_count, slice_width,
						 hdmi_max_chunk_bytes,
						 src_frc_bpp, min_dsc_bpp, bpp);
		if (!bpp_x16)
			return false;

		bpp = DIV_ROUND_UP(bpp_x16, 16);

		/* Fill DSC related DFM input parameters */
		frl_dfm->config.target_bpp_16 = bpp_x16;
		frl_dfm->config.slice_width = slice_width;

		if (drm_frl_dfm_dsc_requirement_met(frl_dfm))
			return true;
	}

	return false;
}

static bool
intel_hdmi_can_support_frl_mode(struct intel_encoder *encoder,
				struct intel_crtc_state *pipe_config)
{
	int rate[] =  {48, 40, 32, 24, 18, 9};
	int audio_freq_hz[] = {192000, 176400, 96000, 88200, 48000};
	struct drm_hdmi_frl_dfm frl_dfm = {0};
	struct intel_hdmi *intel_hdmi = enc_to_intel_hdmi(encoder);
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	int max_rate = intel_hdmi->max_frl_rate;
	int max_dsc_rate = intel_hdmi->max_dsc_frl_rate;
	struct drm_display_mode *adjusted_mode = &pipe_config->hw.adjusted_mode;
	bool can_support_frl_mode = false;
	bool can_support_frl_mode_with_dsc = false;
	int i, j;

	/* Fill mode related input params */
	frl_dfm.config.pixel_clock_nominal_khz = adjusted_mode->clock;
	frl_dfm.config.hactive = adjusted_mode->hdisplay;
	frl_dfm.config.hblank = adjusted_mode->htotal - adjusted_mode->hdisplay;

	/*
	 * #FIXME Currently the bpc and color_format are set to default values
	 * of 8bpc and RGB format. Need to compute the format and check with different
	 * bpc, that satisfies the DFM calculation.
	 */

	/* Fill color related input params */
	frl_dfm.config.bpc = 8;
	frl_dfm.config.color_format = get_drm_color_format(INTEL_OUTPUT_FORMAT_RGB);

	pipe_config->pipe_bpp = frl_dfm.config.bpc * 3;
	pipe_config->output_format = INTEL_OUTPUT_FORMAT_RGB;
	/*
	 * Check if the resolution can be supported in FRL mode
	 * We try with maximum FRL rate and check if Data flow metring
	 * requirements are met, otherwise a lower rate is tried.
	 */
	for (i = 0; i < ARRAY_SIZE(rate); i++) {
		if (rate[i] > max_rate)
			continue;
		/* Fill the bw related input parameters */
		frl_dfm.config.lanes = rate[i] < 24 ? 3 : 4;
		frl_dfm.config.bit_rate_kbps = (rate[i] * 1000000) / frl_dfm.config.lanes;
		for (j = 0; j < ARRAY_SIZE(audio_freq_hz); j++) {
			/* TODO: Check if pipe_config->has_audio is set */
			/* Fill the audio related input params */
			frl_dfm.config.audio_hz = audio_freq_hz[j];
			frl_dfm.config.audio_channels = 8; /*Support 8 channel audio */
			if (drm_frl_dfm_nondsc_requirement_met(&frl_dfm)) {
				can_support_frl_mode = true;
				break;
			}

			if (!max_dsc_rate || max_dsc_rate < rate[i])
				continue;
			/* Try with DSC */
			if (intel_hdmi_can_support_frl_mode_with_dsc(intel_hdmi,
								     pipe_config,
								     &frl_dfm)) {
				can_support_frl_mode_with_dsc = true;
				break;
			}
		}

		if (can_support_frl_mode || can_support_frl_mode_with_dsc)
			break;
	}

	if (!can_support_frl_mode && !can_support_frl_mode_with_dsc) {
		drm_dbg_kms(&dev_priv->drm, "Cannot support FRL mode\n");

		return false;
	}
	
	/* Fill frl capacity output params */
	pipe_config->frl.required_lanes = frl_dfm.config.lanes;
	pipe_config->frl.required_rate = frl_dfm.config.bit_rate_kbps / 1000000;
	pipe_config->frl.tb_borrowed = frl_dfm.params.tb_borrowed;
	pipe_config->frl.tb_actual = frl_dfm.params.tb_borrowed / 2;
	drm_dbg_kms(&dev_priv->drm, "FRL DFM config: tb_borrowed = %d, tb_actual = %d\n",
		    pipe_config->frl.tb_borrowed, pipe_config->frl.tb_actual);

	/*
	 * If no time borrowing required to transmit the active region,
	 * min tb threshold is set to default of 492 tribytes.
	 * Otherwise min tb threshold is 492 - (tb Borrowed / 2)
	 */
	if (frl_dfm.params.tb_borrowed && (frl_dfm.params.tb_borrowed / 2) <= 492)
		pipe_config->frl.tb_threshold_min = 492 - (frl_dfm.params.tb_borrowed / 2);
	else
		pipe_config->frl.tb_threshold_min = 492;

	compute_frl_mn(pipe_config, frl_dfm.params.ftb_avg_k);
	drm_dbg_kms(&dev_priv->drm, "FRL Clock: link_m = %dHz, link_n = %dHz, div18 = %dKHz\n",
		    pipe_config->frl.link_m_ext, pipe_config->frl.link_n_ext,
		    pipe_config->frl.div18);

	/*
	 * TODO
	 * 1. Calculate condition for Reseource based scheduling enable.
	 * Disabling resource based scheduling for now.
	 * 2. Active Character buffer threshold depends on cd clock bw.
	 * Setting default value of 0.
	 */
	pipe_config->frl.rsrc_sched_en = false;
	pipe_config->frl.active_char_buf_threshold = 0;

	if (can_support_frl_mode_with_dsc) {
		pipe_config->frl.hcactive_tb = frl_dfm.params.hcactive_target;
		pipe_config->frl.hctotal_tb = frl_dfm.params.hcactive_target + frl_dfm.params.hcblank_target;
		drm_dbg_kms(&dev_priv->drm, "FRL DFM DSC config: hcactive_tb = %d, hctotal_tb = %d\n",
			    pipe_config->frl.hcactive_tb, pipe_config->frl.hctotal_tb);

		/* Compute all DSC parameters */
		intel_hdmi_dsc_compute_config(encoder, pipe_config, &frl_dfm);
	}

	return true;
}
#endif

int intel_hdmi_compute_config(struct intel_encoder *encoder,
			      struct intel_crtc_state *pipe_config,
			      struct drm_connector_state *conn_state)
{
	struct intel_hdmi *intel_hdmi = enc_to_intel_hdmi(encoder);
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct drm_display_mode *adjusted_mode = &pipe_config->hw.adjusted_mode;
	struct drm_connector *connector = conn_state->connector;
	struct drm_scdc *scdc = &connector->display_info.hdmi.scdc;
#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
	struct intel_crtc *crtc = to_intel_crtc(pipe_config->uapi.crtc);
#endif
	int ret;

	if (adjusted_mode->flags & DRM_MODE_FLAG_DBLSCAN)
		return -EINVAL;

	pipe_config->output_format = INTEL_OUTPUT_FORMAT_RGB;
	pipe_config->has_hdmi_sink = intel_has_hdmi_sink(intel_hdmi,
							 conn_state);

	if (pipe_config->has_hdmi_sink)
		pipe_config->has_infoframe = true;

	if (adjusted_mode->flags & DRM_MODE_FLAG_DBLCLK)
		pipe_config->pixel_multiplier = 2;

	if (HAS_PCH_SPLIT(dev_priv) && !HAS_DDI(dev_priv))
		pipe_config->has_pch_encoder = true;

	pipe_config->has_audio =
		intel_hdmi_has_audio(encoder, pipe_config, conn_state);

#ifdef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
	/*
	 * Try to respect downstream TMDS clock limits first, if
	 * that fails assume the user might know something we don't.
	 */
	 ret = intel_hdmi_compute_output_format(encoder, pipe_config, conn_state, true);
	 if (ret)
		ret = intel_hdmi_compute_output_format(encoder, pipe_config, conn_state, false);
	 if (ret) {
		 drm_dbg_kms(&dev_priv->drm,
		 	     "unsupported HDMI clock (%d kHz), rejecting mode\n",
			     pipe_config->hw.adjusted_mode.crtc_clock);
		 return ret;
	 }
	 pipe_config->lane_count = 4;			     
#else
	if (intel_need_bigjoiner(encoder, adjusted_mode->hdisplay,
				 adjusted_mode->crtc_clock))
		pipe_config->bigjoiner_pipes = GENMASK(crtc->pipe + 1, crtc->pipe);

	if (intel_bios_hdmi_max_frl_rate(encoder) &&
	    intel_hdmi->has_sink_hdmi_21 &&
	    intel_hdmi_can_support_frl_mode(encoder, pipe_config)) {
		drm_dbg_kms(&dev_priv->drm,
			    "Enabling FRL mode with lanes = %d rate = %d\n",
			    pipe_config->frl.required_lanes,
			    pipe_config->frl.required_rate);

		pipe_config->frl.enable = true;
		pipe_config->lane_count = pipe_config->frl.required_lanes;
		/* Port clock is div18 clock rounded to 10 Khz */
		pipe_config->port_clock = roundup(pipe_config->frl.div18, 10);
	} else {

		/* Modes that need Bigjoiner cannot work without FRL */
		if (pipe_config->bigjoiner_pipes)
			return -EINVAL;
		/*
		 * Try to respect downstream TMDS clock limits first, if
		 * that fails assume the user might know something we don't.
		 */
		ret = intel_hdmi_compute_output_format(encoder, pipe_config, conn_state, true);
		if (ret)
			ret = intel_hdmi_compute_output_format(encoder, pipe_config, conn_state,
							       false);
		if (ret) {
			drm_dbg_kms(&dev_priv->drm,
				    "unsupported HDMI clock (%d kHz), rejecting mode\n",
				    pipe_config->hw.adjusted_mode.crtc_clock);
			return ret;
		}
		pipe_config->frl.enable = false;
		pipe_config->lane_count = 4;
	}
#endif

	if (intel_hdmi_is_ycbcr420(pipe_config)) {
		ret = intel_panel_fitting(pipe_config, conn_state);
		if (ret)
			return ret;
	}

	pipe_config->limited_color_range =
		intel_hdmi_limited_color_range(pipe_config, conn_state);

	if (conn_state->picture_aspect_ratio)
		adjusted_mode->picture_aspect_ratio =
			conn_state->picture_aspect_ratio;

#ifdef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
	if (scdc->scrambling.supported && DISPLAY_VER(dev_priv) >= 10) {
#else
	/*
	 * Enable scrambing for only for TMDS mode.
	 * For FRL mode, scrambling is always enabled by HW, and
	 * scrambling enable and high tmds clock bits are not used.
	 */
	if (scdc->scrambling.supported && DISPLAY_VER(dev_priv) >= 10 &&
	    !pipe_config->frl.enable) {
#endif
		if (scdc->scrambling.low_rates)
			pipe_config->hdmi_scrambling = true;

		if (pipe_config->port_clock > 340000) {
			pipe_config->hdmi_scrambling = true;
			pipe_config->hdmi_high_tmds_clock_ratio = true;
		}
	}

	intel_hdmi_compute_gcp_infoframe(encoder, pipe_config,
					 conn_state);

	if (!intel_hdmi_compute_avi_infoframe(encoder, pipe_config, conn_state)) {
		drm_dbg_kms(&dev_priv->drm, "bad AVI infoframe\n");
		return -EINVAL;
	}

	if (!intel_hdmi_compute_spd_infoframe(encoder, pipe_config, conn_state)) {
		drm_dbg_kms(&dev_priv->drm, "bad SPD infoframe\n");
		return -EINVAL;
	}

	if (!intel_hdmi_compute_hdmi_infoframe(encoder, pipe_config, conn_state)) {
		drm_dbg_kms(&dev_priv->drm, "bad HDMI infoframe\n");
		return -EINVAL;
	}

	if (!intel_hdmi_compute_drm_infoframe(encoder, pipe_config, conn_state)) {
		drm_dbg_kms(&dev_priv->drm, "bad DRM infoframe\n");
		return -EINVAL;
	}
#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
	if (pipe_config->dsc.compression_enable)
		intel_hdmi_compute_cvtemp_header(pipe_config);
#endif
	return 0;
}

void intel_hdmi_encoder_shutdown(struct intel_encoder *encoder)
{
	struct intel_hdmi *intel_hdmi = enc_to_intel_hdmi(encoder);

	/*
	 * Give a hand to buggy BIOSen which forget to turn
	 * the TMDS output buffers back on after a reboot.
	 */
	intel_dp_dual_mode_set_tmds_output(intel_hdmi, true);
}

static void
intel_hdmi_unset_edid(struct drm_connector *connector)
{
	struct intel_hdmi *intel_hdmi = intel_attached_hdmi(to_intel_connector(connector));

	intel_hdmi->has_hdmi_sink = false;
	intel_hdmi->has_audio = false;
#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
	intel_hdmi->has_sink_hdmi_21 = false;
#endif

	intel_hdmi->dp_dual_mode.type = DRM_DP_DUAL_MODE_NONE;
	intel_hdmi->dp_dual_mode.max_tmds_clock = 0;

	kfree(to_intel_connector(connector)->detect_edid);
	to_intel_connector(connector)->detect_edid = NULL;
}

static void
intel_hdmi_dp_dual_mode_detect(struct drm_connector *connector, bool has_edid)
{
	struct drm_i915_private *dev_priv = to_i915(connector->dev);
	struct intel_hdmi *hdmi = intel_attached_hdmi(to_intel_connector(connector));
	enum port port = hdmi_to_dig_port(hdmi)->base.port;
	struct i2c_adapter *adapter =
		intel_gmbus_get_adapter(dev_priv, hdmi->ddc_bus);
	enum drm_dp_dual_mode_type type = drm_dp_dual_mode_detect(&dev_priv->drm, adapter);

	/*
	 * Type 1 DVI adaptors are not required to implement any
	 * registers, so we can't always detect their presence.
	 * Ideally we should be able to check the state of the
	 * CONFIG1 pin, but no such luck on our hardware.
	 *
	 * The only method left to us is to check the VBT to see
	 * if the port is a dual mode capable DP port. But let's
	 * only do that when we sucesfully read the EDID, to avoid
	 * confusing log messages about DP dual mode adaptors when
	 * there's nothing connected to the port.
	 */
	if (type == DRM_DP_DUAL_MODE_UNKNOWN) {
		/* An overridden EDID imply that we want this port for testing.
		 * Make sure not to set limits for that port.
		 */
		if (has_edid && !connector->override_edid &&
		    intel_bios_is_port_dp_dual_mode(dev_priv, port)) {
			drm_dbg_kms(&dev_priv->drm,
				    "Assuming DP dual mode adaptor presence based on VBT\n");
			type = DRM_DP_DUAL_MODE_TYPE1_DVI;
		} else {
			type = DRM_DP_DUAL_MODE_NONE;
		}
	}

	if (type == DRM_DP_DUAL_MODE_NONE)
		return;

	hdmi->dp_dual_mode.type = type;
	hdmi->dp_dual_mode.max_tmds_clock =
		drm_dp_dual_mode_max_tmds_clock(&dev_priv->drm, type, adapter);

	drm_dbg_kms(&dev_priv->drm,
		    "DP dual mode adaptor (%s) detected (max TMDS clock: %d kHz)\n",
		    drm_dp_get_dual_mode_type_name(type),
		    hdmi->dp_dual_mode.max_tmds_clock);

	/* Older VBTs are often buggy and can't be trusted :( Play it safe. */
	if ((DISPLAY_VER(dev_priv) >= 8 || IS_HASWELL(dev_priv)) &&
	    !intel_bios_is_port_dp_dual_mode(dev_priv, port)) {
		drm_dbg_kms(&dev_priv->drm,
			    "Ignoring DP dual mode adaptor max TMDS clock for native HDMI port\n");
		hdmi->dp_dual_mode.max_tmds_clock = 0;
	}
}

#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
static void
intel_hdmi_reset_frl_config(struct intel_hdmi *intel_hdmi)
{
	intel_hdmi->frl.trained = false;
	intel_hdmi->frl.lanes = 0;
	intel_hdmi->frl.rate_gbps = 0;
	intel_hdmi->frl.ffe_level = 0;
}
#endif

static bool
intel_hdmi_set_edid(struct drm_connector *connector)
{
	struct drm_i915_private *dev_priv = to_i915(connector->dev);
	struct intel_hdmi *intel_hdmi = intel_attached_hdmi(to_intel_connector(connector));
#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
	struct intel_encoder *encoder = &hdmi_to_dig_port(intel_hdmi)->base;
#endif
	intel_wakeref_t wakeref;
	struct edid *edid;
	bool connected = false;
	struct i2c_adapter *i2c;

	wakeref = intel_display_power_get(dev_priv, POWER_DOMAIN_GMBUS);

	i2c = intel_gmbus_get_adapter(dev_priv, intel_hdmi->ddc_bus);

	edid = drm_get_edid(connector, i2c);

	if (!edid && !intel_gmbus_is_forced_bit(i2c)) {
		drm_dbg_kms(&dev_priv->drm,
			    "HDMI GMBUS EDID read failed, retry using GPIO bit-banging\n");
		intel_gmbus_force_bit(i2c, true);
		edid = drm_get_edid(connector, i2c);
		intel_gmbus_force_bit(i2c, false);
	}

	intel_hdmi_dp_dual_mode_detect(connector, edid != NULL);

	intel_display_power_put(dev_priv, POWER_DOMAIN_GMBUS, wakeref);

	to_intel_connector(connector)->detect_edid = edid;
	if (edid && edid->input & DRM_EDID_INPUT_DIGITAL) {
#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
		int src_rate_lane_gbps = DIV_ROUND_UP(intel_bios_hdmi_max_frl_rate(encoder),
						      1000000);
		int max_src_rate = src_rate_lane_gbps * 4;
#endif

		intel_hdmi->has_audio = drm_detect_monitor_audio(edid);
		intel_hdmi->has_hdmi_sink = drm_detect_hdmi_monitor(edid);
#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
		intel_hdmi->has_sink_hdmi_21 =
			drm_hdmi_sink_max_frl_rate(connector) > 0 ? true : false;
		intel_hdmi->max_frl_rate = min(drm_hdmi_sink_max_frl_rate(connector),
					       max_src_rate);
		intel_hdmi->max_dsc_frl_rate = min(drm_hdmi_sink_dsc_max_frl_rate(connector),
						   max_src_rate);
		intel_hdmi_reset_frl_config(intel_hdmi);
#endif

		connected = true;
	}

	cec_notifier_set_phys_addr_from_edid(intel_hdmi->cec_notifier, edid);

	return connected;
}

static enum drm_connector_status
intel_hdmi_detect(struct drm_connector *connector, bool force)
{
	enum drm_connector_status status = connector_status_disconnected;
	struct drm_i915_private *dev_priv = to_i915(connector->dev);
	struct intel_hdmi *intel_hdmi = intel_attached_hdmi(to_intel_connector(connector));
	struct intel_encoder *encoder = &hdmi_to_dig_port(intel_hdmi)->base;
	intel_wakeref_t wakeref;

	drm_dbg_kms(&dev_priv->drm, "[CONNECTOR:%d:%s]\n",
		    connector->base.id, connector->name);

	if (!INTEL_DISPLAY_ENABLED(dev_priv))
		return connector_status_disconnected;

	wakeref = intel_display_power_get(dev_priv, POWER_DOMAIN_GMBUS);

	if (DISPLAY_VER(dev_priv) >= 11 &&
	    !intel_digital_port_connected(encoder))
		goto out;

	intel_hdmi_unset_edid(connector);

	if (intel_hdmi_set_edid(connector))
		status = connector_status_connected;

out:
	intel_display_power_put(dev_priv, POWER_DOMAIN_GMBUS, wakeref);

	if (status != connector_status_connected)
		cec_notifier_phys_addr_invalidate(intel_hdmi->cec_notifier);

	/*
	 * Make sure the refs for power wells enabled during detect are
	 * dropped to avoid a new detect cycle triggered by HPD polling.
	 */
	intel_display_power_flush_work(dev_priv);

	return status;
}

static void
intel_hdmi_force(struct drm_connector *connector)
{
	struct drm_i915_private *i915 = to_i915(connector->dev);

	drm_dbg_kms(&i915->drm, "[CONNECTOR:%d:%s]\n",
		    connector->base.id, connector->name);

	intel_hdmi_unset_edid(connector);

	if (connector->status != connector_status_connected)
		return;

	intel_hdmi_set_edid(connector);
}

static int intel_hdmi_get_modes(struct drm_connector *connector)
{
	struct edid *edid;

	edid = to_intel_connector(connector)->detect_edid;
	if (edid == NULL)
		return 0;

	return intel_connector_update_modes(connector, edid);
}

static struct i2c_adapter *
intel_hdmi_get_i2c_adapter(struct drm_connector *connector)
{
	struct drm_i915_private *dev_priv = to_i915(connector->dev);
	struct intel_hdmi *intel_hdmi = intel_attached_hdmi(to_intel_connector(connector));

	return intel_gmbus_get_adapter(dev_priv, intel_hdmi->ddc_bus);
}

static void intel_hdmi_create_i2c_symlink(struct drm_connector *connector)
{
	struct drm_i915_private *i915 = to_i915(connector->dev);
	struct i2c_adapter *adapter = intel_hdmi_get_i2c_adapter(connector);
	struct kobject *i2c_kobj = &adapter->dev.kobj;
	struct kobject *connector_kobj = &connector->kdev->kobj;
	int ret;

	ret = sysfs_create_link(connector_kobj, i2c_kobj, i2c_kobj->name);
	if (ret)
		drm_err(&i915->drm, "Failed to create i2c symlink (%d)\n", ret);
}

static void intel_hdmi_remove_i2c_symlink(struct drm_connector *connector)
{
	struct i2c_adapter *adapter = intel_hdmi_get_i2c_adapter(connector);
	struct kobject *i2c_kobj = &adapter->dev.kobj;
	struct kobject *connector_kobj = &connector->kdev->kobj;

	sysfs_remove_link(connector_kobj, i2c_kobj->name);
}

static int
intel_hdmi_connector_register(struct drm_connector *connector)
{
	int ret;

	ret = intel_connector_register(connector);
	if (ret)
		return ret;

	intel_hdmi_create_i2c_symlink(connector);

	return ret;
}

static void intel_hdmi_connector_unregister(struct drm_connector *connector)
{
	struct cec_notifier *n = intel_attached_hdmi(to_intel_connector(connector))->cec_notifier;

	cec_notifier_conn_unregister(n);

	intel_hdmi_remove_i2c_symlink(connector);
	intel_connector_unregister(connector);
}

static const struct drm_connector_funcs intel_hdmi_connector_funcs = {
	.detect = intel_hdmi_detect,
	.force = intel_hdmi_force,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.atomic_get_property = intel_digital_connector_atomic_get_property,
	.atomic_set_property = intel_digital_connector_atomic_set_property,
	.late_register = intel_hdmi_connector_register,
	.early_unregister = intel_hdmi_connector_unregister,
	.destroy = intel_connector_destroy,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
	.atomic_duplicate_state = intel_digital_connector_duplicate_state,
};

static const struct drm_connector_helper_funcs intel_hdmi_connector_helper_funcs = {
	.get_modes = intel_hdmi_get_modes,
	.mode_valid = intel_hdmi_mode_valid,
	.atomic_check = intel_digital_connector_atomic_check,
};

static void
intel_hdmi_add_properties(struct intel_hdmi *intel_hdmi, struct drm_connector *connector)
{
	struct drm_i915_private *dev_priv = to_i915(connector->dev);

	intel_attach_force_audio_property(connector);
	intel_attach_broadcast_rgb_property(connector);
	intel_attach_aspect_ratio_property(connector);

	intel_attach_hdmi_colorspace_property(connector);
	drm_connector_attach_content_type_property(connector);

	if (DISPLAY_VER(dev_priv) >= 10)
		drm_connector_attach_hdr_output_metadata_property(connector);

	if (!HAS_GMCH(dev_priv))
		drm_connector_attach_max_bpc_property(connector, 8, 12);
}

/*
 * intel_hdmi_handle_sink_scrambling: handle sink scrambling/clock ratio setup
 * @encoder: intel_encoder
 * @connector: drm_connector
 * @high_tmds_clock_ratio = bool to indicate if the function needs to set
 *  or reset the high tmds clock ratio for scrambling
 * @scrambling: bool to Indicate if the function needs to set or reset
 *  sink scrambling
 *
 * This function handles scrambling on HDMI 2.0 capable sinks.
 * If required clock rate is > 340 Mhz && scrambling is supported by sink
 * it enables scrambling. This should be called before enabling the HDMI
 * 2.0 port, as the sink can choose to disable the scrambling if it doesn't
 * detect a scrambled clock within 100 ms.
 *
 * Returns:
 * True on success, false on failure.
 */
bool intel_hdmi_handle_sink_scrambling(struct intel_encoder *encoder,
				       struct drm_connector *connector,
				       bool high_tmds_clock_ratio,
				       bool scrambling)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_hdmi *intel_hdmi = enc_to_intel_hdmi(encoder);
	struct drm_scrambling *sink_scrambling =
		&connector->display_info.hdmi.scdc.scrambling;
	struct i2c_adapter *adapter =
		intel_gmbus_get_adapter(dev_priv, intel_hdmi->ddc_bus);

	if (!sink_scrambling->supported)
		return true;

	drm_dbg_kms(&dev_priv->drm,
		    "[CONNECTOR:%d:%s] scrambling=%s, TMDS bit clock ratio=1/%d\n",
		    connector->base.id, connector->name,
		    str_yes_no(scrambling), high_tmds_clock_ratio ? 40 : 10);

	/* Set TMDS bit clock ratio to 1/40 or 1/10, and enable/disable scrambling */
	return drm_scdc_set_high_tmds_clock_ratio(adapter,
						  high_tmds_clock_ratio) &&
		drm_scdc_set_scrambling(adapter, scrambling);
}

static u8 chv_port_to_ddc_pin(struct drm_i915_private *dev_priv, enum port port)
{
	u8 ddc_pin;

	switch (port) {
	case PORT_B:
		ddc_pin = GMBUS_PIN_DPB;
		break;
	case PORT_C:
		ddc_pin = GMBUS_PIN_DPC;
		break;
	case PORT_D:
		ddc_pin = GMBUS_PIN_DPD_CHV;
		break;
	default:
		MISSING_CASE(port);
		ddc_pin = GMBUS_PIN_DPB;
		break;
	}
	return ddc_pin;
}

static u8 bxt_port_to_ddc_pin(struct drm_i915_private *dev_priv, enum port port)
{
	u8 ddc_pin;

	switch (port) {
	case PORT_B:
		ddc_pin = GMBUS_PIN_1_BXT;
		break;
	case PORT_C:
		ddc_pin = GMBUS_PIN_2_BXT;
		break;
	default:
		MISSING_CASE(port);
		ddc_pin = GMBUS_PIN_1_BXT;
		break;
	}
	return ddc_pin;
}

static u8 cnp_port_to_ddc_pin(struct drm_i915_private *dev_priv,
			      enum port port)
{
	u8 ddc_pin;

	switch (port) {
	case PORT_B:
		ddc_pin = GMBUS_PIN_1_BXT;
		break;
	case PORT_C:
		ddc_pin = GMBUS_PIN_2_BXT;
		break;
	case PORT_D:
		ddc_pin = GMBUS_PIN_4_CNP;
		break;
	case PORT_F:
		ddc_pin = GMBUS_PIN_3_BXT;
		break;
	default:
		MISSING_CASE(port);
		ddc_pin = GMBUS_PIN_1_BXT;
		break;
	}
	return ddc_pin;
}

static u8 icl_port_to_ddc_pin(struct drm_i915_private *dev_priv, enum port port)
{
	enum phy phy = intel_port_to_phy(dev_priv, port);

	if (intel_phy_is_combo(dev_priv, phy))
		return GMBUS_PIN_1_BXT + port;
	else if (intel_phy_is_tc(dev_priv, phy))
		return GMBUS_PIN_9_TC1_ICP + intel_port_to_tc(dev_priv, port);

	drm_WARN(&dev_priv->drm, 1, "Unknown port:%c\n", port_name(port));
	return GMBUS_PIN_2_BXT;
}

static u8 mcc_port_to_ddc_pin(struct drm_i915_private *dev_priv, enum port port)
{
	enum phy phy = intel_port_to_phy(dev_priv, port);
	u8 ddc_pin;

	switch (phy) {
	case PHY_A:
		ddc_pin = GMBUS_PIN_1_BXT;
		break;
	case PHY_B:
		ddc_pin = GMBUS_PIN_2_BXT;
		break;
	case PHY_C:
		ddc_pin = GMBUS_PIN_9_TC1_ICP;
		break;
	default:
		MISSING_CASE(phy);
		ddc_pin = GMBUS_PIN_1_BXT;
		break;
	}
	return ddc_pin;
}

static u8 rkl_port_to_ddc_pin(struct drm_i915_private *dev_priv, enum port port)
{
	enum phy phy = intel_port_to_phy(dev_priv, port);

	WARN_ON(port == PORT_C);

	/*
	 * Pin mapping for RKL depends on which PCH is present.  With TGP, the
	 * final two outputs use type-c pins, even though they're actually
	 * combo outputs.  With CMP, the traditional DDI A-D pins are used for
	 * all outputs.
	 */
	if (INTEL_PCH_TYPE(dev_priv) >= PCH_TGP && phy >= PHY_C)
		return GMBUS_PIN_9_TC1_ICP + phy - PHY_C;

	return GMBUS_PIN_1_BXT + phy;
}

static u8 gen9bc_tgp_port_to_ddc_pin(struct drm_i915_private *i915, enum port port)
{
	enum phy phy = intel_port_to_phy(i915, port);

	drm_WARN_ON(&i915->drm, port == PORT_A);

	/*
	 * Pin mapping for GEN9 BC depends on which PCH is present.  With TGP,
	 * final two outputs use type-c pins, even though they're actually
	 * combo outputs.  With CMP, the traditional DDI A-D pins are used for
	 * all outputs.
	 */
	if (INTEL_PCH_TYPE(i915) >= PCH_TGP && phy >= PHY_C)
		return GMBUS_PIN_9_TC1_ICP + phy - PHY_C;

	return GMBUS_PIN_1_BXT + phy;
}

static u8 dg1_port_to_ddc_pin(struct drm_i915_private *dev_priv, enum port port)
{
	return intel_port_to_phy(dev_priv, port) + 1;
}

static u8 adls_port_to_ddc_pin(struct drm_i915_private *dev_priv, enum port port)
{
	enum phy phy = intel_port_to_phy(dev_priv, port);

	WARN_ON(port == PORT_B || port == PORT_C);

	/*
	 * Pin mapping for ADL-S requires TC pins for all combo phy outputs
	 * except first combo output.
	 */
	if (phy == PHY_A)
		return GMBUS_PIN_1_BXT;

	return GMBUS_PIN_9_TC1_ICP + phy - PHY_B;
}

static u8 g4x_port_to_ddc_pin(struct drm_i915_private *dev_priv,
			      enum port port)
{
	u8 ddc_pin;

	switch (port) {
	case PORT_B:
		ddc_pin = GMBUS_PIN_DPB;
		break;
	case PORT_C:
		ddc_pin = GMBUS_PIN_DPC;
		break;
	case PORT_D:
		ddc_pin = GMBUS_PIN_DPD;
		break;
	default:
		MISSING_CASE(port);
		ddc_pin = GMBUS_PIN_DPB;
		break;
	}
	return ddc_pin;
}

static u8 intel_hdmi_ddc_pin(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	enum port port = encoder->port;
	u8 ddc_pin;

	ddc_pin = intel_bios_alternate_ddc_pin(encoder);
	if (ddc_pin) {
		drm_dbg_kms(&dev_priv->drm,
			    "Using DDC pin 0x%x for port %c (VBT)\n",
			    ddc_pin, port_name(port));
		return ddc_pin;
	}

	if (IS_ALDERLAKE_S(dev_priv))
		ddc_pin = adls_port_to_ddc_pin(dev_priv, port);
	else if (INTEL_PCH_TYPE(dev_priv) >= PCH_DG1)
		ddc_pin = dg1_port_to_ddc_pin(dev_priv, port);
	else if (IS_ROCKETLAKE(dev_priv))
		ddc_pin = rkl_port_to_ddc_pin(dev_priv, port);
	else if (DISPLAY_VER(dev_priv) == 9 && HAS_PCH_TGP(dev_priv))
		ddc_pin = gen9bc_tgp_port_to_ddc_pin(dev_priv, port);
	else if (IS_JSL_EHL(dev_priv) && HAS_PCH_TGP(dev_priv))
		ddc_pin = mcc_port_to_ddc_pin(dev_priv, port);
	else if (INTEL_PCH_TYPE(dev_priv) >= PCH_ICP)
		ddc_pin = icl_port_to_ddc_pin(dev_priv, port);
	else if (HAS_PCH_CNP(dev_priv))
		ddc_pin = cnp_port_to_ddc_pin(dev_priv, port);
	else if (IS_GEMINILAKE(dev_priv) || IS_BROXTON(dev_priv))
		ddc_pin = bxt_port_to_ddc_pin(dev_priv, port);
	else if (IS_CHERRYVIEW(dev_priv))
		ddc_pin = chv_port_to_ddc_pin(dev_priv, port);
	else
		ddc_pin = g4x_port_to_ddc_pin(dev_priv, port);

	drm_dbg_kms(&dev_priv->drm,
		    "Using DDC pin 0x%x for port %c (platform default)\n",
		    ddc_pin, port_name(port));

	return ddc_pin;
}

void intel_infoframe_init(struct intel_digital_port *dig_port)
{
	struct drm_i915_private *dev_priv =
		to_i915(dig_port->base.base.dev);

	if (IS_VALLEYVIEW(dev_priv) || IS_CHERRYVIEW(dev_priv)) {
		dig_port->write_infoframe = vlv_write_infoframe;
		dig_port->read_infoframe = vlv_read_infoframe;
		dig_port->set_infoframes = vlv_set_infoframes;
		dig_port->infoframes_enabled = vlv_infoframes_enabled;
	} else if (IS_G4X(dev_priv)) {
		dig_port->write_infoframe = g4x_write_infoframe;
		dig_port->read_infoframe = g4x_read_infoframe;
		dig_port->set_infoframes = g4x_set_infoframes;
		dig_port->infoframes_enabled = g4x_infoframes_enabled;
	} else if (HAS_DDI(dev_priv)) {
		if (intel_bios_is_lspcon_present(dev_priv, dig_port->base.port)) {
			dig_port->write_infoframe = lspcon_write_infoframe;
			dig_port->read_infoframe = lspcon_read_infoframe;
			dig_port->set_infoframes = lspcon_set_infoframes;
			dig_port->infoframes_enabled = lspcon_infoframes_enabled;
		} else {
			dig_port->write_infoframe = hsw_write_infoframe;
			dig_port->read_infoframe = hsw_read_infoframe;
			dig_port->set_infoframes = hsw_set_infoframes;
			dig_port->infoframes_enabled = hsw_infoframes_enabled;
		}
	} else if (HAS_PCH_IBX(dev_priv)) {
		dig_port->write_infoframe = ibx_write_infoframe;
		dig_port->read_infoframe = ibx_read_infoframe;
		dig_port->set_infoframes = ibx_set_infoframes;
		dig_port->infoframes_enabled = ibx_infoframes_enabled;
	} else {
		dig_port->write_infoframe = cpt_write_infoframe;
		dig_port->read_infoframe = cpt_read_infoframe;
		dig_port->set_infoframes = cpt_set_infoframes;
		dig_port->infoframes_enabled = cpt_infoframes_enabled;
	}
}

#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
/* Common code with DP, need to put in a common place */
static void intel_hdmi_modeset_retry_work_fn(struct work_struct *work)
{
	struct intel_connector *intel_connector;
	struct drm_connector *connector;

	intel_connector = container_of(work, typeof(*intel_connector),
				       modeset_retry_work);
	connector = &intel_connector->base;
	DRM_DEBUG_KMS("[CONNECTOR:%d:%s]\n", connector->base.id,
		      connector->name);

	/* Grab the locks before changing connector property*/
	mutex_lock(&connector->dev->mode_config.mutex);
	/*
	 * Set connector link status to BAD and send a Uevent to notify
	 * userspace to do a modeset.
	 */
	drm_connector_set_link_status_property(connector,
					       DRM_MODE_LINK_STATUS_BAD);
	mutex_unlock(&connector->dev->mode_config.mutex);
	/* Send Hotplug uevent so userspace can reprobe */
	drm_kms_helper_hotplug_event(connector->dev);
}
#endif

void intel_hdmi_init_connector(struct intel_digital_port *dig_port,
			       struct intel_connector *intel_connector)
{
	struct drm_connector *connector = &intel_connector->base;
	struct intel_hdmi *intel_hdmi = &dig_port->hdmi;
	struct intel_encoder *intel_encoder = &dig_port->base;
	struct drm_device *dev = intel_encoder->base.dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct i2c_adapter *ddc;
	enum port port = intel_encoder->port;
	struct cec_connector_info conn_info;

	drm_dbg_kms(&dev_priv->drm,
		    "Adding HDMI connector on [ENCODER:%d:%s]\n",
		    intel_encoder->base.base.id, intel_encoder->base.name);

	if (DISPLAY_VER(dev_priv) < 12 && drm_WARN_ON(dev, port == PORT_A))
		return;

	if (drm_WARN(dev, dig_port->max_lanes < 4,
		     "Not enough lanes (%d) for HDMI on [ENCODER:%d:%s]\n",
		     dig_port->max_lanes, intel_encoder->base.base.id,
		     intel_encoder->base.name))
		return;

	intel_hdmi->ddc_bus = intel_hdmi_ddc_pin(intel_encoder);
	ddc = intel_gmbus_get_adapter(dev_priv, intel_hdmi->ddc_bus);

	drm_connector_init_with_ddc(dev, connector,
				    &intel_hdmi_connector_funcs,
				    DRM_MODE_CONNECTOR_HDMIA,
				    ddc);
	drm_connector_helper_add(connector, &intel_hdmi_connector_helper_funcs);

	connector->interlace_allowed = true;
	connector->stereo_allowed = true;

	if (DISPLAY_VER(dev_priv) >= 10)
		connector->ycbcr_420_allowed = true;

	intel_connector->polled = DRM_CONNECTOR_POLL_HPD;

	if (HAS_DDI(dev_priv))
		intel_connector->get_hw_state = intel_ddi_connector_get_hw_state;
	else
		intel_connector->get_hw_state = intel_connector_get_hw_state;

	intel_hdmi_add_properties(intel_hdmi, connector);

	intel_connector_attach_encoder(intel_connector, intel_encoder);
	intel_hdmi->attached_connector = intel_connector;

	if (is_hdcp_supported(dev_priv, port)) {
		int ret = intel_hdcp_init(intel_connector, dig_port,
					  &intel_hdmi_hdcp_shim);
		if (ret)
			drm_dbg_kms(&dev_priv->drm,
				    "HDCP init failed, skipping.\n");
	}

	/* For G4X desktop chip, PEG_BAND_GAP_DATA 3:0 must first be written
	 * 0xd.  Failure to do so will result in spurious interrupts being
	 * generated on the port when a cable is not attached.
	 */
	if (IS_G45(dev_priv)) {
		u32 temp = intel_de_read(dev_priv, PEG_BAND_GAP_DATA);
		intel_de_write(dev_priv, PEG_BAND_GAP_DATA,
		               (temp & ~0xf) | 0xd);
	}

	cec_fill_conn_info_from_drm(&conn_info, connector);

	intel_hdmi->cec_notifier =
		cec_notifier_conn_register(dev->dev, port_identifier(port),
					   &conn_info);
	if (!intel_hdmi->cec_notifier)
		drm_dbg_kms(&dev_priv->drm, "CEC notifier get failed\n");

#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
	/* Initialize the work for modeset in case of link train failure */
	if (DISPLAY_VER(dev_priv) >= 14)
		INIT_WORK(&intel_connector->modeset_retry_work,
			  intel_hdmi_modeset_retry_work_fn);
#endif
}

/*
 * intel_hdmi_dsc_get_slice_height - get the dsc slice_height
 * @vactive: Vactive of a display mode
 *
 * @return: appropriate dsc slice height for a given mode.
 */
int intel_hdmi_dsc_get_slice_height(int vactive)
{
	int slice_height;

	/*
	 * Slice Height determination : HDMI2.1 Section 7.7.5.2
	 * Select smallest slice height >=96, that results in a valid PPS and
	 * requires minimum padding lines required for final slice.
	 *
	 * Assumption : Vactive is even.
	 */
	for (slice_height = 96; slice_height <= vactive; slice_height += 2)
		if (vactive % slice_height == 0)
			return slice_height;

	return 0;
}

/*
 * intel_hdmi_dsc_get_num_slices - get no. of dsc slices based on dsc encoder
 * and dsc decoder capabilities
 *
 * @mode: drm_display_mode for which num of slices are needed
 * @output_format : pipe output format
 * @src_max_slices: maximum slices supported by the DSC encoder
 * @src_max_slice_width: maximum slice width supported by DSC encoder
 * @hdmi_max_slices: maximum slices supported by sink DSC decoder
 * @hdmi_throughput: maximum clock per slice (MHz) supported by HDMI sink
 *
 * @return: num of dsc slices that can be supported by the dsc encoder
 * and decoder.
 */
int
intel_hdmi_dsc_get_num_slices(const struct drm_display_mode *mode,
			      enum intel_output_format output_format,
			      int src_max_slices, int src_max_slice_width,
			      int hdmi_max_slices, int hdmi_throughput)
{
/* Pixel rates in KPixels/sec */
#define HDMI_DSC_PEAK_PIXEL_RATE		2720000
/*
 * Rates at which the source and sink are required to process pixels in each
 * slice, can be two levels: either atleast 340000KHz or atleast 40000KHz.
 */
#define HDMI_DSC_MAX_ENC_THROUGHPUT_0		340000
#define HDMI_DSC_MAX_ENC_THROUGHPUT_1		400000

/* Spec limits the slice width to 2720 pixels */
#define MAX_HDMI_SLICE_WIDTH			2720
	int kslice_adjust;
	int adjusted_clk_khz;
	int min_slices;
	int target_slices;
	int max_throughput; /* max clock freq. in khz per slice */
	int max_slice_width;
	int slice_width;
	int pixel_clock = mode->crtc_clock;

	if (!hdmi_throughput)
		return 0;

	/*
	 * Slice Width determination : HDMI2.1 Section 7.7.5.1
	 * kslice_adjust factor for 4:2:0, and 4:2:2 formats is 0.5, where as
	 * for 4:4:4 is 1.0. Multiplying these factors by 10 and later
	 * dividing adjusted clock value by 10.
	 */
	if (output_format == INTEL_OUTPUT_FORMAT_YCBCR444 ||
	    output_format == INTEL_OUTPUT_FORMAT_RGB)
		kslice_adjust = 10;
	else
		kslice_adjust = 5;

	/*
	 * As per spec, the rate at which the source and the sink process
	 * the pixels per slice are at two levels: atleast 340Mhz or 400Mhz.
	 * This depends upon the pixel clock rate and output formats
	 * (kslice adjust).
	 * If pixel clock * kslice adjust >= 2720MHz slices can be processed
	 * at max 340MHz, otherwise they can be processed at max 400MHz.
	 */

	adjusted_clk_khz = DIV_ROUND_UP(kslice_adjust * pixel_clock, 10);

	if (adjusted_clk_khz <= HDMI_DSC_PEAK_PIXEL_RATE)
		max_throughput = HDMI_DSC_MAX_ENC_THROUGHPUT_0;
	else
		max_throughput = HDMI_DSC_MAX_ENC_THROUGHPUT_1;

	/*
	 * Taking into account the sink's capability for maximum
	 * clock per slice (in MHz) as read from HF-VSDB.
	 */
	max_throughput = min(max_throughput, hdmi_throughput * 1000);

	min_slices = DIV_ROUND_UP(adjusted_clk_khz, max_throughput);
	max_slice_width = min(MAX_HDMI_SLICE_WIDTH, src_max_slice_width);

	/*
	 * Keep on increasing the num of slices/line, starting from min_slices
	 * per line till we get such a number, for which the slice_width is
	 * just less than max_slice_width. The slices/line selected should be
	 * less than or equal to the max horizontal slices that the combination
	 * of PCON encoder and HDMI decoder can support.
	 */
	slice_width = max_slice_width;

	do {
		if (min_slices <= 1 && src_max_slices >= 1 && hdmi_max_slices >= 1)
			target_slices = 1;
		else if (min_slices <= 2 && src_max_slices >= 2 && hdmi_max_slices >= 2)
			target_slices = 2;
		else if (min_slices <= 4 && src_max_slices >= 4 && hdmi_max_slices >= 4)
			target_slices = 4;
		else if (min_slices <= 8 && src_max_slices >= 8 && hdmi_max_slices >= 8)
			target_slices = 8;
		else if (min_slices <= 12 && src_max_slices >= 12 && hdmi_max_slices >= 12)
			target_slices = 12;
		else if (min_slices <= 16 && src_max_slices >= 16 && hdmi_max_slices >= 16)
			target_slices = 16;
		else
			return 0;

		slice_width = DIV_ROUND_UP(mode->hdisplay, target_slices);
		if (slice_width >= max_slice_width)
			min_slices = target_slices + 1;
	} while (slice_width >= max_slice_width);

	return target_slices;
}

#ifdef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
/*
 * intel_hdmi_dsc_get_bpp - get the appropriate compressed bits_per_pixel based on
 * source and sink capabilities.
 *
 * @src_fraction_bpp: fractional bpp supported by the source
 * @slice_width: dsc slice width supported by the source and sink
 * @num_slices: num of slices supported by the source and sink
 * @output_format: video output format
 * @bpc: bits per color
 * @hdmi_all_bpp: sink supports decoding of 1/16th bpp setting
 * @hdmi_max_chunk_bytes: max bytes in a line of chunks supported by sink
 *
 * @return: compressed bits_per_pixel in step of 1/16 of bits_per_pixel
 */
int
intel_hdmi_dsc_get_bpp(int src_fractional_bpp, int slice_width, int num_slices,
                      enum intel_output_format output_format, u8 bpc,
                      bool hdmi_all_bpp, int hdmi_max_chunk_bytes)
#else
static int
get_dsc_compressed_bpp(int num_slices, int slice_width, int hdmi_max_chunk_bytes,
		       int src_fractional_bpp, int min_dsc_bpp, int max_dsc_bpp)
#endif
{
#ifdef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
	int max_dsc_bpp, min_dsc_bpp;
#endif	
	int target_bytes;
	bool bpp_found = false;
	int bpp_decrement_x16;
	int bpp_target;
	int bpp_target_x16;

#ifdef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
	/*
 	 * Get min bpp and max bpp as per Table 7.23, in HDMI2.1 spec
 	 * Start with the max bpp and keep on decrementing with
 	 * fractional bpp, if supported by PCON DSC encoder
 	 *
 	 * for each bpp we check if no of bytes can be supported by HDMI sink
 	 */

	if (output_format == INTEL_OUTPUT_FORMAT_YCBCR420) {
        	min_dsc_bpp = 6;
        	max_dsc_bpp = 3 * bpc / 2;
	} else if (output_format == INTEL_OUTPUT_FORMAT_YCBCR444 ||
           	output_format == INTEL_OUTPUT_FORMAT_RGB) {
        	min_dsc_bpp = 8;
       		max_dsc_bpp = 3 * bpc;
	} else {
        	/* Assuming 4:2:2 encoding */
        	min_dsc_bpp = 7;
        	max_dsc_bpp = 2 * bpc;
	}

	/*
 	 * Taking into account if all dsc_all_bpp supported by HDMI2.1 sink
	 * Section 7.7.34 : Source shall not enable compressed Video
	 * Transport with bpp_target settings above 12 bpp unless
	 * DSC_all_bpp is set to 1.
	 */
	if (!hdmi_all_bpp)
        	max_dsc_bpp = min(max_dsc_bpp, 12);
#endif

	/*
	 * The Sink has a limit of compressed data in bytes for a scanline,
	 * as described in max_chunk_bytes field in HFVSDB block of edid.
	 * The no. of bytes depend on the target bits per pixel that the
	 * source configures. So we start with the max_bpp and calculate
	 * the target_chunk_bytes. We keep on decrementing the target_bpp,
	 * till we get the target_chunk_bytes just less than what the sink's
	 * max_chunk_bytes, or else till we reach the min_dsc_bpp.
	 *
	 * The decrement is according to the fractional support from PCON DSC
	 * encoder. For fractional BPP we use bpp_target as a multiple of 16.
	 *
	 * bpp_target_x16 = bpp_target * 16
	 * So we need to decrement by {1, 2, 4, 8, 16} for fractional bpps
	 * {1/16, 1/8, 1/4, 1/2, 1} respectively.
	 */

	bpp_target = max_dsc_bpp;

	/* src does not support fractional bpp implies decrement by 16 for bppx16 */
	if (!src_fractional_bpp)
		src_fractional_bpp = 1;
	bpp_decrement_x16 = DIV_ROUND_UP(16, src_fractional_bpp);
	bpp_target_x16 = (bpp_target * 16) - bpp_decrement_x16;

	while (bpp_target_x16 > (min_dsc_bpp * 16)) {
		int bpp;

		bpp = DIV_ROUND_UP(bpp_target_x16, 16);
		target_bytes = DIV_ROUND_UP((num_slices * slice_width * bpp), 8);
		if (target_bytes <= hdmi_max_chunk_bytes) {
			bpp_found = true;
			break;
		}
		bpp_target_x16 -= bpp_decrement_x16;
	}
	if (bpp_found)
		return bpp_target_x16;

	return 0;
}

#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
static void
get_dsc_min_max_bpp(enum intel_output_format output_format, u8 bpc,
		    bool hdmi_all_bpp, int *min_dsc_bpp, int *max_dsc_bpp)
{
	/*
	 * Get min bpp and max bpp as per Table 7.23, in HDMI2.1 spec
	 * Start with the max bpp and keep on decrementing with
	 * fractional bpp, if supported by the DSC encoder
	 *
	 * for each bpp we check if no of bytes can be supported by HDMI sink
	 */

	if (output_format == INTEL_OUTPUT_FORMAT_YCBCR420) {
		*min_dsc_bpp = 6;
		*max_dsc_bpp = 3 * bpc / 2;
	} else if (output_format == INTEL_OUTPUT_FORMAT_YCBCR444 ||
		   output_format == INTEL_OUTPUT_FORMAT_RGB) {
		*min_dsc_bpp = 8;
		*max_dsc_bpp = 3 * bpc;
	} else {
		/* Assuming 4:2:2 encoding */
		*min_dsc_bpp = 7;
		*max_dsc_bpp = 2 * bpc;
	}

	/*
	 * Taking into account if all dsc_all_bpp supported by HDMI2.1 sink
	 * Section 7.7.34 : Source shall not enable compressed Video
	 * Transport with bpp_target settings above 12 bpp unless
	 * DSC_all_bpp is set to 1.
	 */
	if (!hdmi_all_bpp)
		*max_dsc_bpp = min(*max_dsc_bpp, 12);
}

/*
 * intel_hdmi_dsc_get_bpp - get the appropriate compressed bits_per_pixel based on
 * source and sink capabilities.
 *
 * @src_fraction_bpp: fractional bpp supported by the source
 * @slice_width: dsc slice width supported by the source and sink
 * @num_slices: num of slices supported by the source and sink
 * @output_format: video output format
 * @bpc: bits per color
 * @hdmi_all_bpp: sink supports decoding of 1/16th bpp setting
 * @hdmi_max_chunk_bytes: max bytes in a line of chunks supported by sink
 *
 * @return: compressed bits_per_pixel in step of 1/16 of bits_per_pixel
 */
int
intel_hdmi_dsc_get_bpp(int src_fractional_bpp, int slice_width, int num_slices,
		       enum intel_output_format output_format, u8 bpc,
		       bool hdmi_all_bpp, int hdmi_max_chunk_bytes)
{
	int max_dsc_bpp, min_dsc_bpp;
	int dsc_bpp_x16;

	get_dsc_min_max_bpp(output_format, bpc, hdmi_all_bpp,
			    &min_dsc_bpp, &max_dsc_bpp);

	dsc_bpp_x16 = get_dsc_compressed_bpp(num_slices, slice_width,
					     hdmi_max_chunk_bytes,
					     src_fractional_bpp,
					     min_dsc_bpp, max_dsc_bpp);

	return dsc_bpp_x16;
}
#endif

#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
static
bool is_flt_ready(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_hdmi *intel_hdmi = enc_to_intel_hdmi(encoder);
	struct i2c_adapter *adapter =
		intel_gmbus_get_adapter(dev_priv, intel_hdmi->ddc_bus);

	return drm_scdc_read_status_flags(adapter) & SCDC_FLT_READY;
}

static
bool intel_hdmi_frl_prepare_lts2(struct intel_encoder *encoder,
				 const struct intel_crtc_state *crtc_state,
				 int ffe_level)
{
#define TIMEOUT_FLT_READY_MS  250
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_hdmi *intel_hdmi = enc_to_intel_hdmi(encoder);
	struct i2c_adapter *adapter =
		intel_gmbus_get_adapter(dev_priv, intel_hdmi->ddc_bus);
	bool flt_ready = false;
	int frl_rate;
	int frl_lanes;

	frl_rate = crtc_state->frl.required_rate;
	frl_lanes = crtc_state->frl.required_lanes;

	if (!frl_rate || !frl_lanes)
		return false;

	/*
	 * POLL for FRL ready : READ SCDC 0x40 Bit 6 FLT ready
	 * #TODO Check if 250 msec is required
	 */
	wait_for(flt_ready = is_flt_ready(encoder) == true,
		 TIMEOUT_FLT_READY_MS);

	if (!flt_ready) {
		drm_dbg_kms(&dev_priv->drm,
			    "HDMI sink not ready for FRL in %d\n",
			    TIMEOUT_FLT_READY_MS);

		return false;
	}

	/*
	 * #TODO As per spec, during prepare phase LTS2, the TXFFE to be
	 * programmed to be 0 for each lane in the PHY registers.
	 */

	if (drm_scdc_config_frl(adapter, frl_rate, frl_lanes, ffe_level) < 0) {
		drm_dbg_kms(&dev_priv->drm,
			    "Failed to write SCDC config regs for FRL\n");

		return false;
	}

	return flt_ready;
}

enum frl_lt_status {
	FRL_TRAINING_PASSED,
	FRL_CHANGE_RATE,
	FRL_TRAIN_CONTINUE,
	FRL_TRAIN_RETRAIN,
	FRL_TRAIN_STOP,
};

static
u8 get_frl_update_flag(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_hdmi *intel_hdmi = enc_to_intel_hdmi(encoder);
	struct i2c_adapter *adapter =
		intel_gmbus_get_adapter(dev_priv, intel_hdmi->ddc_bus);

	return drm_scdc_read_update_flags(adapter);
}

static
int get_link_training_patterns(struct intel_encoder *encoder,
			       enum drm_scdc_frl_ltp ltp[4])
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_hdmi *intel_hdmi = enc_to_intel_hdmi(encoder);
	struct i2c_adapter *adapter =
		intel_gmbus_get_adapter(dev_priv, intel_hdmi->ddc_bus);

	return drm_scdc_get_ltp(adapter, ltp);
}

static enum frl_lt_status
intel_hdmi_train_lanes(struct intel_encoder *encoder,
		       const struct intel_crtc_state *crtc_state,
		       int ffe_level)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	enum transcoder trans = crtc_state->cpu_transcoder;
	enum drm_scdc_frl_ltp ltp[4];
	int num_lanes = crtc_state->frl.required_lanes;
	int lane;

	/*
	 * LTS3 Link Training in Progress.
	 * Section 6.4.2.3 Table 6-34.
	 *
	 * Transmit link training pattern as requested by the sink
	 * for a specific rate.
	 * Source keep on Polling on FLT update flag and keep
	 * repeating patterns till timeout or request for new rate,
	 * or training is successful.
	 */
	if (!(get_frl_update_flag(encoder) & SCDC_FLT_UPDATE))
		return FRL_TRAIN_CONTINUE;

	if (get_link_training_patterns(encoder, ltp) < 0)
		return FRL_TRAIN_STOP;

	if (ltp[0] == ltp[1] && ltp[1] == ltp[2]) {
		if (num_lanes == 3 || (num_lanes == 4 && ltp[2] == ltp[3])) {
			if (ltp[0] == SCDC_FRL_NO_LTP)
				return FRL_TRAINING_PASSED;
			if (ltp[0] == SCDC_FRL_CHNG_RATE)
				return FRL_CHANGE_RATE;
		}
	}

	for (lane = 0; lane < num_lanes; lane++) {
		if (ltp[lane] >= SCDC_FRL_LTP1 && ltp[lane] <= SCDC_FRL_LTP8)
			/* write the LTP for the lane*/
			intel_de_write(dev_priv, TRANS_HDMI_FRL_TRAIN(trans),
				       TRANS_HDMI_FRL_LTP(ltp[lane], lane));
		else if (ltp[lane] == SCDC_FRL_CHNG_FFE) {
			/*
			 * #TODO Update TxFFE for the lane
			 *
			 * Read the existing TxFFE for the lane, from PHY regs.
			 * If TxFFE is already at FFE_level (i.e. max level)
			 * then Set TXFFE0 for the lane.
			 * Otherwise increment TxFFE for the lane.
			 */
		}
	}

	return FRL_TRAIN_CONTINUE;
}

static int
clear_scdc_update_flags(struct intel_encoder *encoder, u8 flags)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_hdmi *intel_hdmi = enc_to_intel_hdmi(encoder);
	struct i2c_adapter *adapter =
		intel_gmbus_get_adapter(dev_priv, intel_hdmi->ddc_bus);

	return drm_scdc_clear_update_flags(adapter, flags);
}

static enum frl_lt_status
frl_train_complete_ltsp(struct intel_encoder *encoder,
			const struct intel_crtc_state *crtc_state)
{
#define FLT_UPDATE_TIMEOUT_MS 200
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	enum transcoder trans = crtc_state->cpu_transcoder;
	u32 buf;
	u8 update_flag = 0;

	/*
	 * Start FRL transmission with only Gap Characters, with Scrambing,
	 * Reed Solomon FEC, and Super block structure.
	 */
	buf = intel_de_read(dev_priv, TRANS_HDMI_FRL_CFG(trans));
	intel_de_write(dev_priv, TRANS_HDMI_FRL_CFG(trans),
		       buf | TRANS_HDMI_FRL_TRAINING_COMPLETE);

	/* Clear SCDC FLT_UPDATE by writing 1 */
	if (clear_scdc_update_flags(encoder, SCDC_FLT_UPDATE) < 0)
		return FRL_TRAIN_STOP;

	wait_for((update_flag = get_frl_update_flag(encoder)) &
		 (SCDC_FRL_START | SCDC_FLT_UPDATE), FLT_UPDATE_TIMEOUT_MS);

	if (update_flag & SCDC_FRL_START)
		return FRL_TRAINING_PASSED;

	if (update_flag & SCDC_FLT_UPDATE) {
		drm_dbg_kms(&dev_priv->drm,
			    "FRL update received for retraining the lanes\n");
		clear_scdc_update_flags(encoder, SCDC_FLT_UPDATE);

		return FRL_TRAIN_RETRAIN;
	}

	drm_err(&dev_priv->drm, "FRL TRAINING: FRL update timedout\n");

	return FRL_TRAIN_STOP;
}

static enum frl_lt_status
intel_hdmi_frl_train_lts3(struct intel_encoder *encoder,
			  const struct intel_crtc_state *crtc_state,
			  int ffe_level)
{
/*
 * Time interval specified for link training HDMI2.1 Spec:
 * Sec 6.4.2.1 Table 6-31
 */
#define FLT_TIMEOUT_MS 200
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	enum frl_lt_status status;
	enum transcoder trans = crtc_state->cpu_transcoder;
	u32 buf;

	buf = intel_de_read(dev_priv, TRANS_HDMI_FRL_CFG(trans));
	intel_de_write(dev_priv, TRANS_HDMI_FRL_CFG(trans),
		       buf | TRANS_HDMI_FRL_ENABLE);

#define done ((status = intel_hdmi_train_lanes(encoder, crtc_state, ffe_level)) != FRL_TRAIN_CONTINUE)
	wait_for(done, FLT_TIMEOUT_MS);

	/* TIMEDOUT */
	if (status == FRL_TRAIN_CONTINUE) {
		drm_err(&dev_priv->drm, "FRL TRAINING: FLT TIMEDOUT\n");

		return FRL_TRAIN_STOP;
	}

	if (status != FRL_TRAINING_PASSED)
		return status;

	return frl_train_complete_ltsp(encoder, crtc_state);
}

static void intel_hdmi_frl_ltsl(struct intel_encoder *encoder,
				const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_hdmi *intel_hdmi = enc_to_intel_hdmi(encoder);
	struct i2c_adapter *adapter =
		intel_gmbus_get_adapter(dev_priv, intel_hdmi->ddc_bus);
	int lanes = crtc_state->frl.required_lanes;

	/* Clear flags */
	drm_scdc_config_frl(adapter, 0, lanes, 0);
	drm_scdc_clear_update_flags(adapter, SCDC_FLT_UPDATE);
}

static bool get_next_frl_rate(int *curr_rate_gbps, int max_sink_rate)
{
	int valid_rate[] =  {48, 40, 32, 24, 18, 9};
	int i;

	for (i = 0; i < ARRAY_SIZE(valid_rate); i++) {
		if (max_sink_rate < valid_rate[i])
			continue;

		if (*curr_rate_gbps < valid_rate[i]) {
			*curr_rate_gbps = valid_rate[i];
			return true;
		}
	}

	return false;
}

static int get_ffe_level(int rate_gbps)
{
	/*
	 * #TODO check for FFE_LEVEL to be programmed
	 *
	 * Should start with max ffe_levels supported by source. MAX can be 3.
	 * Currently setting ffe_level = 0.
	 */
	return 0;
}

/*
 * intel_hdmi_start_frl - Start FRL training for HDMI2.1 sink
 *
 */
void intel_hdmi_start_frl(struct intel_encoder *encoder,
			  const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);
	struct intel_hdmi *intel_hdmi = &dig_port->hdmi;
	struct intel_connector *intel_connector = intel_hdmi->attached_connector;
	struct drm_connector *connector = &intel_connector->base;
	int *rate;
	int max_rate = crtc_state->dsc.compression_enable ? intel_hdmi->max_dsc_frl_rate :
							intel_hdmi->max_frl_rate;
	int req_rate = crtc_state->frl.required_lanes * crtc_state->frl.required_rate;
	int ffe_level = get_ffe_level(req_rate);
	enum transcoder trans = crtc_state->cpu_transcoder;
	enum frl_lt_status status;
	u32 buf = 0;

	if (DISPLAY_VER(dev_priv) < 14)
		return;

	if (!crtc_state->frl.enable)
		goto ltsl_tmds_mode;

	if (intel_hdmi->frl.trained &&
	    intel_hdmi->frl.rate_gbps >= req_rate &&
	    intel_hdmi->frl.ffe_level == ffe_level) {
		drm_dbg_kms(&dev_priv->drm,
			    "[CONNECTOR:%d:%s] FRL Already trained with rate=%d, ffe_level=%d\n",
			    connector->base.id, connector->name,
			    req_rate, ffe_level);

		return;
	}

	intel_hdmi_reset_frl_config(intel_hdmi);

	if (!intel_hdmi_frl_prepare_lts2(encoder, crtc_state, ffe_level))
		status = FRL_TRAIN_STOP;
	else
		status = intel_hdmi_frl_train_lts3(encoder, crtc_state, ffe_level);

	switch (status) {
	case FRL_TRAINING_PASSED:
		intel_hdmi->frl.trained = true;
		intel_hdmi->frl.rate_gbps = req_rate;
		intel_hdmi->frl.ffe_level = ffe_level;
		drm_dbg_kms(&dev_priv->drm,
			    "[CONNECTOR:%d:%s] FRL Training Passed with rate=%d, ffe_level=%d\n",
			    connector->base.id, connector->name,
			    req_rate, ffe_level);

		return;
	case FRL_TRAIN_STOP:
		/*
		 * Cannot go with FRL transmission.
		 * Reset FRL rates so during next modeset TMDS mode will be
		 * selected.
		 */
		if (crtc_state->dsc.compression_enable)
			intel_hdmi->max_dsc_frl_rate = 0;
		else
			intel_hdmi->max_frl_rate = 0;
		break;
	case FRL_CHANGE_RATE:
		/*
		 * Sink request for change of FRL rate.
		 * Set FRL rates for the connector with lower rate.
		 */
		if (crtc_state->dsc.compression_enable)
			rate = &intel_hdmi->max_dsc_frl_rate;
		else
			rate = &intel_hdmi->max_frl_rate;
		if (!get_next_frl_rate(rate, max_rate))
			*rate = 0;
		break;
	case FRL_TRAIN_RETRAIN:
		/*
		 * For Retraining with same rate, we send a uevent to userspace.
		 * TODO Need to check how many times we can retry.
		 */
		fallthrough;
	default:
		break;
	}

ltsl_tmds_mode:
	intel_hdmi_frl_ltsl(encoder, crtc_state);
	buf = intel_de_read(dev_priv, TRANS_HDMI_FRL_CFG(trans));
	intel_de_write(dev_priv, TRANS_HDMI_FRL_CFG(trans),
		       buf & ~(TRANS_HDMI_FRL_ENABLE | TRANS_HDMI_FRL_TRAINING_COMPLETE));

	if (crtc_state->frl.enable && !intel_hdmi->frl.trained) {
		drm_err(&dev_priv->drm,
			"[CONNECTOR:%d:%s] FRL Training Failed with rate=%d, ffe_level=%d\n",
			connector->base.id, connector->name,
			req_rate, ffe_level);
		/* Send event to user space, to try with next rate or fall back to TMDS */
		schedule_work(&intel_connector->modeset_retry_work);
	}
}

void intel_hdmi_fill_emp_header_byte(const struct hdmi_extended_metadata_packet *emp,
				     u32 *emp_header)
{
	if (!emp->enabled)
		return;

	*emp_header = 0;
	*emp_header |= TRANS_HDMI_EMP_HB0;
	*emp_header |= TRANS_HDMI_EMP_NUM_PACKETS(emp->first_data_set.data_set_length);

	switch (emp->first_data_set.ds_type) {
	case HDMI_EMP_DS_TYPE_PSTATIC:
		*emp_header |= TRANS_HDMI_EMP_DS_TYPE_PSTATIC;
		break;
	case HDMI_EMP_DS_TYPE_DYNAMIC:
		*emp_header |= TRANS_HDMI_EMP_DS_TYPE_DYNAMIC;
		break;
	case HDMI_EMP_DS_TYPE_UNIQUE:
		*emp_header |= TRANS_HDMI_EMP_DS_TYPE_UNIQUE;
		break;
	default:
		break;
	}

	if (emp->first_data_set.pb0_end)
		*emp_header |= TRANS_HDMI_EMP_END;
}

void intel_hdmi_set_hcactive(struct drm_i915_private *dev_priv,
			     const struct intel_crtc_state *crtc_state)
{
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;
	u32 val = 0;

	if (!crtc_state->dsc.compression_enable)
		return;

	val |= TRANS_HDMI_HCACTIVE_TB(crtc_state->frl.hcactive_tb);
	val |= TRANS_HDMI_HCTOTAL_TB(crtc_state->frl.hctotal_tb);

	intel_de_write(dev_priv, TRANS_HDMI_HCTOTAL(cpu_transcoder), val);
}
#endif
