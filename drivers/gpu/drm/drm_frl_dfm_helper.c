// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corp
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <drm/drm_frl_dfm_helper.h>
#include <drm/drm_connector.h>

/* Total frl characters per super block */
static unsigned int drm_get_frl_char_per_super_blk(unsigned int lanes)
{
	return (4 * FRL_CHAR_PER_CHAR_BLK) + lanes;
}

/* Total minimum overhead multiplied by EFFICIENCY_MULIPLIER */
static unsigned int drm_get_total_minimum_overhead(unsigned int lanes)
{
	unsigned int overhead_sb;
	unsigned int overhead_rs;
	unsigned int overhead_map;

	/*
	 * Determine the overhead due to the inclusion of
	 * the SR and SSB FRL characters used for
	 * super block framing
	 */
	overhead_sb =
		(lanes * EFFICIENCY_MULTIPLIER) / drm_get_frl_char_per_super_blk(lanes);

	/*
	 * Determine the overhead due to the inclusion of RS FEC pairity
	 * symbols. Each character block uses 8 FRL characters for RS Pairity
	 * and there are 4 character blocks per super block
	 */
	overhead_rs =
		(8 * 4 * EFFICIENCY_MULTIPLIER) /  drm_get_frl_char_per_super_blk(lanes);

	/*
	 * Determine the overhead due to FRL Map characters.
	 * In a bandwidth constrained application, the FRL packets will be long,
	 * there will typically be two FRL Map Characters per Super Block most of the time.
	 * When a tracnsition occurs between Hactive and Hblank (uncomperssed video) or
	 * HCactive and HCblank (compressed video transport), there may be a
	 * third FRL Map Charecter. Therefore this spec assumes 2.5 FRL Map Characters
	 * per Super Block.
	 */
	overhead_map =
		(25  * EFFICIENCY_MULTIPLIER) / (10 * drm_get_frl_char_per_super_blk(lanes));

	return overhead_sb + overhead_rs + overhead_map;
}

/* Audio Support Verification Computations */

/*
 * During the Hblank period, Audio packets (32 frl characters each),
 * ACR packets (32 frl characters each), Island guard band (4 total frl characters)
 * and Video guard band (3 frl characters) do not benefit from RC compression
 * Therefore start by determining the number of Control Characters that maybe
 * RC compressible
 */
static unsigned int
drm_get_num_char_rc_compressible(unsigned int color_format, unsigned int bpc,
				 unsigned int audio_packets_line, unsigned int hblank)
{
	unsigned int cfrl_free;
	unsigned int kcdx100, k420;

	if (color_format == DRM_COLOR_FORMAT_YCRCB420)
		k420 = 2;
	else
		k420 = 1;

	if (color_format == DRM_COLOR_FORMAT_YCRCB422)
		kcdx100 = 100;
	else
		kcdx100 = (100 * bpc) / 8;

	cfrl_free = max(((hblank * kcdx100) / (100 * k420) - 32 * audio_packets_line - 7),
			U32_MIN);

	return cfrl_free;
}

/*
 * Determine the actual number of characters made available by
 * RC compression
 */
static unsigned int
drm_get_num_char_compression_savings(unsigned int cfrl_free)
{
	/*
	 * In order to be conservative, situations are considered where
	 * maximum RC compression may not be possible.
	 * Add one character each for RC break caused by:
	 * • Island Preamble not aligned to the RC Compression
	 * • Video Preamble not aligned to the RC Compression
	 * • HSYNC lead edge not aligned to the RC Compression
	 * • HSYNC trail edge not aligned to the RC Compression
	 */
	const unsigned int cfrl_margin = 4;
	unsigned int cfrl_savings = max(((7 * cfrl_free) / 8) - cfrl_margin, U32_MIN);

	return cfrl_savings;
}

static unsigned int
drm_get_frl_bits_per_pixel(unsigned int color_format, unsigned int bpc)
{
	unsigned int kcdx100, k420;

	if (color_format == DRM_COLOR_FORMAT_YCRCB420)
		k420 = 2;
	else
		k420 = 1;

	if (color_format == DRM_COLOR_FORMAT_YCRCB422)
		kcdx100 = 100;
	else
		kcdx100 = (100 * bpc) / 8;

	return (24 * kcdx100) / (100 * k420);
}

/* Determine the total available tribytes during the blanking period */
static unsigned int
drm_get_blanking_tribytes_avail(unsigned int color_format,
				unsigned int hblank, unsigned int bpc)
{
	unsigned int kcdx100, k420;

	if (color_format == DRM_COLOR_FORMAT_YCRCB420)
		k420 = 2;
	else
		k420 = 1;

	if (color_format == DRM_COLOR_FORMAT_YCRCB422)
		kcdx100 = 100;
	else
		kcdx100 = (100 * bpc) / 8;

	return DIV_ROUND_UP((hblank * kcdx100), (100 * k420));
}

/*
 * Determine the minimum time necessary to transmit the active tribytes
 * considering frl bandwidth limitation.
 * Given the available bandwidth (i.e after overhead is considered),
 * tactive_min represents the amount of time needed to transmit all the
 * active data
 */
static unsigned int
drm_get_tactive_min(unsigned int num_lanes, unsigned int tribyte_active,
		    unsigned int overhead_max_k, unsigned int frl_char_min_rate_k)
{
	unsigned int active_bytes, rate_kbps, efficiency_k, effective_rate_kbps;

	active_bytes = (3 * tribyte_active) / 2;
	rate_kbps = num_lanes * frl_char_min_rate_k;
	efficiency_k = EFFICIENCY_MULTIPLIER - overhead_max_k;
	effective_rate_kbps = mult_frac(rate_kbps, efficiency_k, EFFICIENCY_MULTIPLIER);

	return mult_frac(FRL_TIMING_NS_MULTIPLIER, active_bytes, effective_rate_kbps) / 1000;
}

/*
 * Determine the minimum time necessary to transmit the video blanking
 * tribytes considering frl bandwidth limitations
 */
static unsigned int
drm_get_tblank_min(unsigned int num_lanes, unsigned int tribyte_blank,
		   unsigned int overhead_max_k, unsigned int frl_char_min_rate_k)
{
	unsigned int blank_bytes, rate_kbps, efficiency_k, effective_rate_kbps;

	blank_bytes = (3 * tribyte_blank) / 2;
	rate_kbps = num_lanes * frl_char_min_rate_k;
	efficiency_k = EFFICIENCY_MULTIPLIER - overhead_max_k;
	effective_rate_kbps = mult_frac(rate_kbps, efficiency_k, EFFICIENCY_MULTIPLIER);

	return mult_frac(FRL_TIMING_NS_MULTIPLIER, blank_bytes, effective_rate_kbps) / 1000;

}

/* Collect link characteristics */
static void
drm_frl_dfm_compute_link_characteristics(struct drm_hdmi_frl_dfm *frl_dfm)
{
	unsigned int frl_bit_rate_min_kbps, line_width, rate_m;

	/* Determine the maximum legal pixel rate */
	frl_dfm->params.pixel_clock_max_khz =
		(frl_dfm->config.pixel_clock_nominal_khz * (1000 + TOLERANCE_PIXEL_CLOCK)) / 1000;

	/* Determine the minimum Video Line period */
	line_width = frl_dfm->config.hactive + frl_dfm->config.hblank;

	frl_dfm->params.line_time_ns = mult_frac(FRL_TIMING_NS_MULTIPLIER,
						  line_width,
						  frl_dfm->params.pixel_clock_max_khz) / 1000;

	/* Determine the worst-case slow FRL Bit Rate in kbps*/
	frl_bit_rate_min_kbps =
		(frl_dfm->config.bit_rate_kbps / 1000000) * (1000000 - TOLERANCE_FRL_BIT_RATE);

	/* Determine the worst-case slow FRL Character Rate */
	frl_dfm->params.char_rate_min_kbps = frl_bit_rate_min_kbps / 18;

	/* Character rate in mega chars/sec */
	rate_m = DIV_ROUND_UP(frl_dfm->params.char_rate_min_kbps * frl_dfm->config.lanes, 1000);

	/* Determine the Minimum Total FRL characters per line period */
	frl_dfm->params.cfrl_line = DIV_ROUND_UP(frl_dfm->params.line_time_ns * rate_m,
						 FRL_TIMING_NS_MULTIPLIER / 1000000);
}

/* Determine FRL link overhead */
static void drm_frl_dfm_compute_max_frl_link_overhead(struct drm_hdmi_frl_dfm *frl_dfm)
{
	unsigned int overhead_min =
		drm_get_total_minimum_overhead(frl_dfm->config.lanes);

	/*
	 * Additional margin to the overhead is provided to account for the possibility
	 * of more Map Characters, zero padding at the end of HCactive, and other minor
	 * items
	 */
	frl_dfm->params.overhead_max = overhead_min + OVERHEAD_M;
}

/* Audio support Verification computations */
static void
drm_frl_dfm_compute_audio_hblank_min(struct drm_hdmi_frl_dfm *frl_dfm)
{
	unsigned int num_audio_pkt, audio_pkt_rate;

	/*
	 * TBD: get the actual audio pkt type as described in
	 * table 6.44 of HDMI2.1 spec to find the num_audio_pkt,
	 * for now assume audio sample packet and audio packet
	 * layout as 1, resulting in number of audio packets
	 * required to carry each audio sample or audio frame
	 * as 1
	 */
	num_audio_pkt = 1;

	/*
	 * Determine Audio Related Packet Rate considering the audio clock
	 * increased to maximim rate permitted by Tolerance Audio clock
	 */
	audio_pkt_rate =
		((frl_dfm->config.audio_hz *  num_audio_pkt + (2 * ACR_RATE_MAX)) *
		 (1000000 + TOLERANCE_AUDIO_CLOCK)) / 1000000;

	/*
	 * Average required packets per line is
	 * number of audio packets needed during Hblank
	 */
	frl_dfm->params.num_audio_pkts_line =
		DIV_ROUND_UP(audio_pkt_rate * frl_dfm->params.line_time_ns,
			     FRL_TIMING_NS_MULTIPLIER);

	/*
	 * Minimum required Hblank assuming no Control Period RC Compression
	 * This includes Video Guard band, Two Island Guard bands, two 12 character
	 * Control Periods and 32 * AudioPackets_Line.
	 * In addition, 32 character periods are allocated for the transmission of an
	 * ACR packet
	 */
	frl_dfm->params.hblank_audio_min = 32 + 32 * frl_dfm->params.num_audio_pkts_line;
}

/*
 * Determine the number of tribytes required for active video , blanking period
 * with the pixel configuration
 */
static void
drm_frl_dfm_compute_tbactive_tbblank(struct drm_hdmi_frl_dfm *frl_dfm)
{
	unsigned int bpp, bytes_per_line;

	bpp = drm_get_frl_bits_per_pixel(frl_dfm->config.color_format, frl_dfm->config.bpc);
	bytes_per_line = (bpp * frl_dfm->config.hactive) / 8;

	frl_dfm->params.tb_active = DIV_ROUND_UP(bytes_per_line, 3);

	frl_dfm->params.tb_blank =
		drm_get_blanking_tribytes_avail(frl_dfm->config.color_format,
						frl_dfm->config.hblank,
						frl_dfm->config.bpc);
}

/* Verify the configuration meets the capacity requirements for the FRL configuration*/
static bool
drm_frl_dfm_verify_frl_capacity_requirement(struct drm_hdmi_frl_dfm *frl_dfm)
{
	unsigned int tactive_ref_ns, tblank_ref_ns, tactive_min_ns, tblank_min_ns;
	unsigned int tborrowed_ns;
	unsigned int line_time_ns = frl_dfm->params.line_time_ns;
	unsigned int hactive = frl_dfm->config.hactive;
	unsigned int hblank = frl_dfm->config.hblank;

	/* Determine the average tribyte rate in kilo tribytes per sec */
	frl_dfm->params.ftb_avg_k =
		(frl_dfm->params.pixel_clock_max_khz * (frl_dfm->params.tb_active + frl_dfm->params.tb_blank)) /
		(frl_dfm->config.hactive + frl_dfm->config.hblank);

	/*
	 * Determine the time required to transmit the active portion of the
	 * minimum possible active line period in the base timing
	 */
	tactive_ref_ns = (line_time_ns * hactive) / (hblank + hactive);

	/*
	 * Determine the time required to transmit the Video blanking portion
	 * of the minimum possible active line period in the base timing
	 */
	tblank_ref_ns = (line_time_ns * hblank) / (hblank + hactive);

	tactive_min_ns = drm_get_tactive_min(frl_dfm->config.lanes,
					     frl_dfm->params.tb_active,
					     frl_dfm->params.overhead_max,
					     frl_dfm->params.char_rate_min_kbps);
	tblank_min_ns = drm_get_tblank_min(frl_dfm->config.lanes,
					   frl_dfm->params.tb_blank,
					   frl_dfm->params.overhead_max,
					   frl_dfm->params.char_rate_min_kbps);

	if (tactive_ref_ns >= tactive_min_ns &&
	    tblank_ref_ns >= tblank_min_ns) {
		tborrowed_ns = 0;
		frl_dfm->params.tb_borrowed = 0;

		return true;
	}

	if (tactive_ref_ns < tactive_min_ns &&
	    tblank_ref_ns >= tblank_min_ns) {
		tborrowed_ns = tactive_min_ns - tactive_ref_ns;
		/* Determine the disparity in tribytes */
		frl_dfm->params.tb_borrowed =
			DIV_ROUND_UP((tborrowed_ns * frl_dfm->params.ftb_avg_k * 1000),
				     FRL_TIMING_NS_MULTIPLIER);

		if (frl_dfm->params.tb_borrowed <= TB_BORROWED_MAX)
			return true;
	}

	return false;
}

/* Verify utilization does not exceed capacity */
static bool
drm_frl_dfm_verify_utilization_possible(struct drm_hdmi_frl_dfm *frl_dfm)
{
	unsigned int cfrl_free, cfrl_savings, frl_char_payload_actual;
	unsigned int utilization, margin;

	cfrl_free = drm_get_num_char_rc_compressible(frl_dfm->config.color_format,
						     frl_dfm->config.bpc,
						     frl_dfm->params.num_audio_pkts_line,
						     frl_dfm->config.hblank);
	cfrl_savings = drm_get_num_char_compression_savings(cfrl_free);

	/*
	 * Determine the actual number of payload FRL characters required to
	 * carry each video line
	 */
	frl_char_payload_actual =
		DIV_ROUND_UP(3 * frl_dfm->params.tb_active, 2) +
		frl_dfm->params.tb_blank - cfrl_savings;

	/*
	 * Determine the payload utilization of the total number of
	 * FRL characters
	 */
	utilization = (frl_char_payload_actual * EFFICIENCY_MULTIPLIER) / frl_dfm->params.cfrl_line;

	margin = 1000 - (utilization + frl_dfm->params.overhead_max);

	if (margin > 0)
		return true;

	return false;
}

/* Check if DFM requirement is met */
bool
drm_frl_dfm_nondsc_requirement_met(struct drm_hdmi_frl_dfm *frl_dfm)
{
	bool frl_capacity_req_met;

	drm_frl_dfm_compute_max_frl_link_overhead(frl_dfm);
	drm_frl_dfm_compute_link_characteristics(frl_dfm);
	drm_frl_dfm_compute_audio_hblank_min(frl_dfm);
	drm_frl_dfm_compute_tbactive_tbblank(frl_dfm);

	frl_capacity_req_met = drm_frl_dfm_verify_frl_capacity_requirement(frl_dfm);

	if (frl_capacity_req_met)
		return drm_frl_dfm_verify_utilization_possible(frl_dfm);

	return false;
}
EXPORT_SYMBOL(drm_frl_dfm_nondsc_requirement_met);

/* DSC DFM functions */

/* Get required no. of tribytes (estimate1) during HCBlank */
static unsigned int
drm_get_frl_hcblank_tb_est1_target(unsigned int hcactive_target_tb,
				   unsigned int hactive, unsigned int hblank)
{
	return DIV_ROUND_UP(hcactive_target_tb * hblank, hactive);
}

/* Get required no. of tribytes during HCBlank */
static unsigned int
drm_get_frl_hcblank_tb_target(unsigned int hcactive_target_tb, unsigned int hactive,
			      unsigned int hblank, unsigned int hcblank_audio_min,
			      unsigned int cfrl_available)
{
	unsigned int hcblank_target_tb1 = drm_get_frl_hcblank_tb_est1_target(hcactive_target_tb,
								    hactive, hblank);
	unsigned int hcblank_target_tb2 = max(hcblank_target_tb1, hcblank_audio_min);

	return 4 * (min(hcblank_target_tb2,
			(2 * cfrl_available - 3 * hcactive_target_tb) / 2) / 4);
}

/* Get time to send all tribytes in hcactive region in nsec*/
static unsigned int
drm_frl_dsc_tactive_target_ns(unsigned int frl_lanes, unsigned int hcactive_target_tb,
			      unsigned int ftb_avg_k, unsigned int min_frl_char_rate_k,
			      unsigned int overhead_max)
{
	unsigned int avg_tribyte_time_ns, tribyte_time_ns;
	unsigned int num_chars_hcactive;
	unsigned int frl_char_rate_k;

	/* Avg time to transmit all active region tribytes */
	avg_tribyte_time_ns = mult_frac(FRL_TIMING_NS_MULTIPLIER,
					hcactive_target_tb, ftb_avg_k * 1000);

	/*
	 * 2 bytes in active region = 1 FRL characters
	 * 1 Tribyte in active region = 3/2 FRL characters
	 */
	num_chars_hcactive = (hcactive_target_tb * 3) / 2;

	/*
	 * FRL rate = lanes * frl character rate
	 * But actual bandwidth wil be less, due to FRL limitations so account
	 * for the overhead involved.
	 * FRL rate with overhead = FRL rate * (100 - overhead %) / 100
	 */
	frl_char_rate_k = frl_lanes * min_frl_char_rate_k;
	frl_char_rate_k = (frl_char_rate_k * (EFFICIENCY_MULTIPLIER - overhead_max)) /
			  EFFICIENCY_MULTIPLIER;

	/* Time to transmit all characters with FRL limitations */
	tribyte_time_ns = mult_frac(FRL_TIMING_NS_MULTIPLIER,
				    num_chars_hcactive, frl_char_rate_k * 1000);

	return max(avg_tribyte_time_ns, tribyte_time_ns);
}

/* Get TBdelta : borrowing in tribytes relative to avg tribyte rate */
static unsigned int
drm_frl_get_dsc_tri_bytes_delta(unsigned int tactive_target_ns, unsigned int tblank_target_ns,
				unsigned int tactive_ref_ns, unsigned int tblank_ref_ns,
				unsigned int hcactive_target_tb, unsigned int ftb_avg_k,
				unsigned int hactive, unsigned int hblank,
				unsigned int line_time_ns)
{
	unsigned int tb_delta_limit;
	unsigned int hcblank_target_tb1 = drm_get_frl_hcblank_tb_est1_target(hcactive_target_tb,
								    hactive, hblank);
	unsigned int tribytes = (hcactive_target_tb + hcblank_target_tb1);
	unsigned int tactive_avg_ns;

	if (tblank_ref_ns < tblank_target_ns) {
		tactive_avg_ns = mult_frac(FRL_TIMING_NS_MULTIPLIER,
					   hcactive_target_tb, ftb_avg_k * 1000);
		tb_delta_limit = ((tactive_ref_ns - tactive_avg_ns) * tribytes) / line_time_ns;
	} else {
		unsigned int t_delta_ns;

		if (tactive_target_ns > tactive_ref_ns)
			t_delta_ns = tactive_target_ns - tactive_ref_ns;
		else
			t_delta_ns = tactive_ref_ns - tactive_target_ns;
		tb_delta_limit = (t_delta_ns * tribytes) / line_time_ns;
	}

	return tb_delta_limit;
}

/* Compute hcactive and hcblank tribytes for given dsc bpp setting */
static void
drm_frl_dfm_dsc_compute_tribytes(struct drm_hdmi_frl_dfm *frl_dfm)
{
	unsigned int hcactive_target_tb;
	unsigned int hcblank_target_tb;
	unsigned int cfrl_available;
	unsigned int num_slices;
	unsigned int bytes_target;

	/* Assert for slice width ?*/
	if (!frl_dfm->config.slice_width)
		return;

	num_slices = DIV_ROUND_UP(frl_dfm->config.hactive, frl_dfm->config.slice_width);

	/* Get required no. of tribytes during HCActive */
	bytes_target = num_slices * DIV_ROUND_UP(frl_dfm->config.target_bpp_16 * frl_dfm->config.slice_width,
						 8 * BPP_MULTIPLIER);
	hcactive_target_tb = DIV_ROUND_UP(bytes_target, 3);

	/* Get FRL Available characters */
	cfrl_available =
		((EFFICIENCY_MULTIPLIER - frl_dfm->params.overhead_max) * frl_dfm->params.cfrl_line) / EFFICIENCY_MULTIPLIER;

	hcblank_target_tb =
		drm_get_frl_hcblank_tb_target(hcactive_target_tb,
					      frl_dfm->config.hactive,
					      frl_dfm->config.hblank,
					      frl_dfm->params.hblank_audio_min,
					      cfrl_available);

	frl_dfm->params.hcactive_target = hcactive_target_tb;
	frl_dfm->params.hcblank_target = hcblank_target_tb;
}

/* Check if audio supported with given dsc bpp and frl bandwidth */
static bool
drm_frl_dfm_dsc_audio_supported(struct drm_hdmi_frl_dfm *frl_dfm)
{
	return frl_dfm->params.hcblank_target < frl_dfm->params.hblank_audio_min;
}

/* Is DFM timing requirement is met with DSC */
static
bool drm_frl_dfm_dsc_is_timing_req_met(struct drm_hdmi_frl_dfm *frl_dfm)
{
	unsigned int ftb_avg_k;
	unsigned int tactive_ref_ns, tblank_ref_ns, tactive_target_ns, tblank_target_ns;
	unsigned int tb_borrowed, tb_delta, tb_worst;

	/* Get the avg no of tribytes sent per sec (Kbps) */
	ftb_avg_k = (frl_dfm->params.hcactive_target + frl_dfm->params.hcblank_target) *
		    (frl_dfm->params.pixel_clock_max_khz / (frl_dfm->config.hactive + frl_dfm->config.hblank));

	/* Time to send Active tribytes in nanoseconds */
	tactive_ref_ns = (frl_dfm->params.line_time_ns * frl_dfm->config.hactive) /
			 (frl_dfm->config.hactive + frl_dfm->config.hblank);

	/* Time to send Blanking tribytes in nanoseconds */
	tblank_ref_ns = (frl_dfm->params.line_time_ns * frl_dfm->config.hblank) /
			(frl_dfm->config.hactive + frl_dfm->config.hblank);

	tactive_target_ns = drm_frl_dsc_tactive_target_ns(frl_dfm->config.lanes,
							  frl_dfm->params.hcactive_target,
							  ftb_avg_k,
							  frl_dfm->params.char_rate_min_kbps,
							  frl_dfm->params.overhead_max);

	tblank_target_ns = frl_dfm->params.line_time_ns - tactive_target_ns;

	/* Get no. of tri bytes borrowed with DSC enabled */
	tb_borrowed = DIV_ROUND_UP(tactive_target_ns * ftb_avg_k * 1000, FRL_TIMING_NS_MULTIPLIER) -
		      frl_dfm->params.hcactive_target;

	tb_delta = drm_frl_get_dsc_tri_bytes_delta(tactive_target_ns,
						   tblank_target_ns,
						   tactive_ref_ns,
						   tblank_ref_ns,
						   frl_dfm->params.hcactive_target,
						   ftb_avg_k,
						   frl_dfm->config.hactive,
						   frl_dfm->config.hblank,
						   frl_dfm->params.line_time_ns);

	tb_worst = max(tb_borrowed, tb_delta);
	if (tb_worst > TB_BORROWED_MAX)
		return false;

	frl_dfm->params.ftb_avg_k = ftb_avg_k;
	frl_dfm->params.tb_borrowed = tb_borrowed;

	return true;
}

/* Check Utilization constraint with DSC */
static bool
drm_frl_dsc_check_utilization(struct drm_hdmi_frl_dfm *frl_dfm)
{
	unsigned int hcactive_target_tb = frl_dfm->params.hcactive_target;
	unsigned int hcblank_target_tb = frl_dfm->params.hcblank_target;
	unsigned int frl_char_per_line = frl_dfm->params.cfrl_line;
	unsigned int overhead_max = frl_dfm->params.overhead_max;
	unsigned int actual_frl_char_payload;
	unsigned int utilization;
	unsigned int utilization_with_overhead;

	/*
	 * Note:
	 * 1 FRL characters per 2 bytes in active period
	 * 1 FRL char per byte in Blanking period
	 */
	actual_frl_char_payload = DIV_ROUND_UP(3 * hcactive_target_tb, 2) +
				  hcblank_target_tb;

	utilization = (actual_frl_char_payload * EFFICIENCY_MULTIPLIER) /
		      frl_char_per_line;

	/*
	 * Utilization with overhead = utlization% +overhead %
	 * should be less than 100%
	 */
	utilization_with_overhead = utilization + overhead_max;
	if (utilization_with_overhead  > EFFICIENCY_MULTIPLIER)
		return false;

	return false;
}

/*
 * drm_frl_fm_dsc_requirement_met : Check if FRL DFM requirements are met with
 * the given bpp.
 * @frl_dfm: dfm structure
 *
 * Returns true if the frl dfm requirements are met, else returns false.
 */
bool drm_frl_dfm_dsc_requirement_met(struct drm_hdmi_frl_dfm *frl_dfm)
{
	if (!frl_dfm->config.slice_width || !frl_dfm->config.target_bpp_16)
		return false;

	drm_frl_dfm_compute_max_frl_link_overhead(frl_dfm);
	drm_frl_dfm_compute_link_characteristics(frl_dfm);
	drm_frl_dfm_compute_audio_hblank_min(frl_dfm);
	drm_frl_dfm_dsc_compute_tribytes(frl_dfm);

	if (!drm_frl_dfm_dsc_audio_supported(frl_dfm))
		return false;

	if (!drm_frl_dfm_dsc_is_timing_req_met(frl_dfm))
		return false;

	if (!drm_frl_dsc_check_utilization(frl_dfm))
		return false;

	return true;
}
EXPORT_SYMBOL(drm_frl_dfm_dsc_requirement_met);
