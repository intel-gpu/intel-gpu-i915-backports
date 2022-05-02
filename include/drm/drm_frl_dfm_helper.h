/* SPDX-License-Identifier: MIT
 * Copyright © 2022 Intel Corp
 */

#ifndef DRM_FRL_DFM_H_
#define DRM_FRL_DFM_H_

/* DFM constraints and tolerance values from HDMI2.1 spec */
#define TB_BORROWED_MAX			400
#define FRL_CHAR_PER_CHAR_BLK		510
/* Tolerance pixel clock unit is in  mHz */
#define TOLERANCE_PIXEL_CLOCK		5
#define TOLERANCE_FRL_BIT_RATE		300
#define TOLERANCE_AUDIO_CLOCK		1000
#define ACR_RATE_MAX			1500
#define EFFICIENCY_MULTIPLIER		1000
#define OVERHEAD_M			(3 * EFFICIENCY_MULTIPLIER / 1000)
#define BPP_MULTIPLIER			16
#define FRL_TIMING_NS_MULTIPLIER	1000000000

/* All the input config needed to compute DFM requirements */
struct drm_frl_dfm_input_config {
	/*
	 * Pixel clock rate kHz, when FVA is
	 * enabled this rate is the rate after adjustment
	 */
	unsigned int pixel_clock_nominal_khz;

	/* active pixels per line */
	unsigned int hactive;

	/* Blanking pixels per line */
	unsigned int hblank;

	/* Bits per component */
	unsigned int bpc;

	/* Pixel encoding */
	unsigned int color_format;

	/* FRL bit rate in kbps */
	unsigned int bit_rate_kbps;

	/* FRL lanes */
	unsigned int lanes;

	/* Number of audio channels */
	unsigned int audio_channels;

	/* Audio rate in Hz */
	unsigned int audio_hz;

	/* Selected bpp target value */
	unsigned int target_bpp_16;

	/*
	 * Number of horizontal pixels in a slice.
	 * Equivalent to PPS parameter slice_width
	 */
	unsigned int slice_width;
};

/* Computed dfm parameters as per the HDMI2.1 spec */
struct drm_frl_dfm_params {
	/*
	 * Link overhead in percentage
	 * multiplied by 1000 (efficiency multiplier)
	 */
	unsigned int overhead_max;

	/* Maximum pixel rate in kHz */
	unsigned int pixel_clock_max_khz;

	/* Minimum video line period in nano sec */
	unsigned int line_time_ns;

	/* worst case slow frl character rate in kbps */
	unsigned int char_rate_min_kbps;

	/* minimum total frl charecters per line period */
	unsigned int cfrl_line;

	/* Average tribyte rate in khz */
	unsigned int ftb_avg_k;

	/* Audio characteristics */

	/*  number of audio packets needed during hblank */
	unsigned int num_audio_pkts_line;

	/*
	 *  Minimum required hblank assuming no control period
	 *  RC compression
	 */
	unsigned int hblank_audio_min;

	/* Number of tribytes required to carry active video */
	unsigned int tb_active;

	/* Total available tribytes during the blanking period */
	unsigned int tb_blank;

	/*
	 * Number of tribytes required to be transmitted during
	 * the hblank period
	 */
	unsigned int tb_borrowed;

	/* DSC frl characteristics */

	/* Tribytes required to carry the target bpp */
	unsigned int hcactive_target;

	/* tribytes available during blanking with target bpp */
	unsigned int hcblank_target;
};

/* FRL DFM structure to hold data involved in DFM computation */
struct drm_hdmi_frl_dfm {
	struct drm_frl_dfm_input_config config;
	struct drm_frl_dfm_params params;
};

bool drm_frl_dfm_nondsc_requirement_met(struct drm_hdmi_frl_dfm *frl_dfm);

bool
drm_frl_dfm_dsc_requirement_met(struct drm_hdmi_frl_dfm *frl_dfm);

#endif
