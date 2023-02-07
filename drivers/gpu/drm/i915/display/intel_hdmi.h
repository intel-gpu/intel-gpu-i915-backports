/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_HDMI_H__
#define __INTEL_HDMI_H__

#include <linux/hdmi.h>
#include <linux/types.h>

struct drm_connector;
struct drm_display_mode;
struct drm_encoder;
struct drm_i915_private;
struct intel_connector;
struct intel_digital_port;
struct intel_encoder;
struct intel_crtc_state;
struct intel_hdmi;
struct drm_connector_state;
union hdmi_infoframe;
enum port;
enum intel_output_format;

#ifndef VRR_FEATURE_NOT_SUPPORTED
/* Total Payload Bytes in an EMP(PB0-PB27) is 28 Bytes*/
#define EMP_PAYLOAD_SIZE 28

/*
 * Total VTEM Payload Packets to be written in 32bit EMP DATA REG
 * DW1: PB3|PB2|PB1|PB0
 * DW2: MD0|PB6|PB5|PB4
 * DW3: MD4|MD3|MD2|MD1
 * DW4-7: Padding
 */
#define VTEM_NUM_DWORDS (EMP_PAYLOAD_SIZE / 4)
#endif

void intel_hdmi_init_connector(struct intel_digital_port *dig_port,
			       struct intel_connector *intel_connector);
int intel_hdmi_compute_config(struct intel_encoder *encoder,
			      struct intel_crtc_state *pipe_config,
			      struct drm_connector_state *conn_state);
void intel_hdmi_encoder_shutdown(struct intel_encoder *encoder);
bool intel_hdmi_handle_sink_scrambling(struct intel_encoder *encoder,
				       struct drm_connector *connector,
				       bool high_tmds_clock_ratio,
				       bool scrambling);
void intel_dp_dual_mode_set_tmds_output(struct intel_hdmi *hdmi, bool enable);
void intel_infoframe_init(struct intel_digital_port *dig_port);
u32 intel_hdmi_infoframes_enabled(struct intel_encoder *encoder,
				  const struct intel_crtc_state *crtc_state);
u32 intel_hdmi_infoframe_enable(unsigned int type);
void intel_hdmi_read_gcp_infoframe(struct intel_encoder *encoder,
				   struct intel_crtc_state *crtc_state);
void intel_read_infoframe(struct intel_encoder *encoder,
			  const struct intel_crtc_state *crtc_state,
			  enum hdmi_infoframe_type type,
			  union hdmi_infoframe *frame);
bool intel_hdmi_limited_color_range(const struct intel_crtc_state *crtc_state,
				    const struct drm_connector_state *conn_state);
bool intel_hdmi_bpc_possible(const struct intel_crtc_state *crtc_state,
			     int bpc, bool has_hdmi_sink, bool ycbcr420_output);
int intel_hdmi_tmds_clock(int clock, int bpc, bool ycbcr420_output);
int intel_hdmi_dsc_get_bpp(int src_fractional_bpp, int slice_width,
			   int num_slices, enum intel_output_format output_format,
			   u8 bpc, bool hdmi_all_bpp, int hdmi_max_chunk_bytes);
int intel_hdmi_dsc_get_num_slices(const struct drm_display_mode *mode,
				  enum intel_output_format output_format,
				  int src_max_slices, int src_max_slice_width,
				  int hdmi_max_slices, int hdmi_throughput);
int intel_hdmi_dsc_get_slice_height(int vactive);
struct drm_i915_private *intel_hdmi_to_i915(struct intel_hdmi *intel_hdmi);
#ifndef NATIVE_HDMI21_FEATURES_NOT_SUPPORTED
void intel_hdmi_start_frl(struct intel_encoder *encoder,
			  const struct intel_crtc_state *crtc_state);
void intel_hdmi_fill_emp_header_byte(const struct hdmi_extended_metadata_packet *emp,
				     u32 *emp_header);
void intel_hdmi_set_hcactive(struct drm_i915_private *dev_priv,
			     const struct intel_crtc_state *crtc_state);
#endif
#ifndef VRR_FEATURE_NOT_SUPPORTED
void intel_mtl_write_emp(struct intel_encoder *encoder,
			 const struct intel_crtc_state *crtc_state);
void intel_mtl_read_emp(struct intel_encoder *encoder,
			struct intel_crtc_state *crtc_state);
#endif

#endif /* __INTEL_HDMI_H__ */
