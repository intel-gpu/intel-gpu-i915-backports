/*
 * Copyright (C) 2021 Intel Corporation
 */

#include <drm/drm_dp_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_atomic_helper.h>

u8 dp_link_status(const u8 link_status[DP_LINK_STATUS_SIZE], int r)
{
	return link_status[r - DP_LANE0_1_STATUS];
}
EXPORT_SYMBOL(dp_link_status);

/* DP 2.0 128b/132b */
#ifdef DRM_DP_GET_ADJUST_NOT_PRESENT
u8 drm_dp_get_adjust_tx_ffe_preset(const u8 link_status[DP_LINK_STATUS_SIZE],
				   int lane)
{
	int i = DP_ADJUST_REQUEST_LANE0_1 + (lane >> 1);
	int s = ((lane & 1) ?
		 DP_ADJUST_TX_FFE_PRESET_LANE1_SHIFT :
		 DP_ADJUST_TX_FFE_PRESET_LANE0_SHIFT);
	u8 l = dp_link_status(link_status, i);

	return (l >> s) & 0xf;
}
EXPORT_SYMBOL(drm_dp_get_adjust_tx_ffe_preset);
#endif /* DRM_DP_GET_ADJUST_NOT_PRESENT */

#ifdef DRM_EDP_BACKLIGHT_NOT_PRESENT
static inline int
drm_edp_backlight_probe_level(struct drm_dp_aux *aux, struct drm_edp_backlight_info *bl,
			      u8 *current_mode)
{
	int ret;
	u8 buf[2];
	u8 mode_reg;

	ret = drm_dp_dpcd_readb(aux, DP_EDP_BACKLIGHT_MODE_SET_REGISTER, &mode_reg);
	if (ret != 1) {
		drm_dbg_kms(aux->drm_dev, "%s: Failed to read backlight mode: %d\n",
				aux->name, ret);
		return ret < 0 ? ret : -EIO;
	}

	*current_mode = (mode_reg & DP_EDP_BACKLIGHT_CONTROL_MODE_MASK);
	if (*current_mode == DP_EDP_BACKLIGHT_CONTROL_MODE_DPCD) {
		int size = 1 + bl->lsb_reg_used;

		ret = drm_dp_dpcd_read(aux, DP_EDP_BACKLIGHT_BRIGHTNESS_MSB, buf, size);
		if (ret != size) {
			drm_dbg_kms(aux->drm_dev, "%s: Failed to read backlight level: %d\n",
					aux->name, ret);
			return ret < 0 ? ret : -EIO;
		}

		if (bl->lsb_reg_used)
			return (buf[0] << 8) | buf[1];
		else
			return buf[0];
	}

	/*
	 * If we're not in DPCD control mode yet, the programmed brightness value is meaningless and
	 * the driver should assume max brightness
	 */
	return bl->max;
}

static inline int
drm_edp_backlight_probe_max(struct drm_dp_aux *aux, struct drm_edp_backlight_info *bl,
			    u16 driver_pwm_freq_hz, const u8 edp_dpcd[EDP_DISPLAY_CTL_CAP_SIZE])
{
	int fxp, fxp_min, fxp_max, fxp_actual, f = 1;
	int ret;
	u8 pn, pn_min, pn_max;

	ret = drm_dp_dpcd_readb(aux, DP_EDP_PWMGEN_BIT_COUNT, &pn);
	if (ret != 1) {
		drm_dbg_kms(aux->drm_dev, "%s: Failed to read pwmgen bit count cap: %d\n",
			    aux->name, ret);
		return -ENODEV;
	}

	pn &= DP_EDP_PWMGEN_BIT_COUNT_MASK;
	bl->max = (1 << pn) - 1;
	if (!driver_pwm_freq_hz)
		return 0;

	/*
	 * Set PWM Frequency divider to match desired frequency provided by the driver.
	 * The PWM Frequency is calculated as 27Mhz / (F x P).
	 * - Where F = PWM Frequency Pre-Divider value programmed by field 7:0 of the
	 *	       EDP_BACKLIGHT_FREQ_SET register (DPCD Address 00728h)
	 * - Where P = 2^Pn, where Pn is the value programmed by field 4:0 of the
	 *	       EDP_PWMGEN_BIT_COUNT register (DPCD Address 00724h)
	 */

	/* Find desired value of (F x P)
	 * Note that, if F x P is out of supported range, the maximum value or minimum value will
	 * applied automatically. So no need to check that.
	 */
	fxp = DIV_ROUND_CLOSEST(1000 * DP_EDP_BACKLIGHT_FREQ_BASE_KHZ, driver_pwm_freq_hz);

	/* Use highest possible value of Pn for more granularity of brightness adjustment while
	 * satifying the conditions below.
	 * - Pn is in the range of Pn_min and Pn_max
	 * - F is in the range of 1 and 255
	 * - FxP is within 25% of desired value.
	 *   Note: 25% is arbitrary value and may need some tweak.
	 */
	ret = drm_dp_dpcd_readb(aux, DP_EDP_PWMGEN_BIT_COUNT_CAP_MIN, &pn_min);
	if (ret != 1) {
		drm_dbg_kms(aux->drm_dev, "%s: Failed to read pwmgen bit count cap min: %d\n",
			    aux->name, ret);
		return 0;
	}
	ret = drm_dp_dpcd_readb(aux, DP_EDP_PWMGEN_BIT_COUNT_CAP_MAX, &pn_max);
	if (ret != 1) {
		drm_dbg_kms(aux->drm_dev, "%s: Failed to read pwmgen bit count cap max: %d\n",
			    aux->name, ret);
		return 0;
	}
	pn_min &= DP_EDP_PWMGEN_BIT_COUNT_MASK;
	pn_max &= DP_EDP_PWMGEN_BIT_COUNT_MASK;

	/* Ensure frequency is within 25% of desired value */
	fxp_min = DIV_ROUND_CLOSEST(fxp * 3, 4);
	fxp_max = DIV_ROUND_CLOSEST(fxp * 5, 4);
	if (fxp_min < (1 << pn_min) || (255 << pn_max) < fxp_max) {
		drm_dbg_kms(aux->drm_dev,
			    "%s: Driver defined backlight frequency (%d) out of range\n",
			    aux->name, driver_pwm_freq_hz);
		return 0;
	}

	for (pn = pn_max; pn >= pn_min; pn--) {
		f = clamp(DIV_ROUND_CLOSEST(fxp, 1 << pn), 1, 255);
		fxp_actual = f << pn;
		if (fxp_min <= fxp_actual && fxp_actual <= fxp_max)
			break;
	}

	ret = drm_dp_dpcd_writeb(aux, DP_EDP_PWMGEN_BIT_COUNT, pn);
	if (ret != 1) {
		drm_dbg_kms(aux->drm_dev, "%s: Failed to write aux pwmgen bit count: %d\n",
			    aux->name, ret);
		return 0;
	}
	bl->pwmgen_bit_count = pn;
	bl->max = (1 << pn) - 1;

	if (edp_dpcd[2] & DP_EDP_BACKLIGHT_FREQ_AUX_SET_CAP) {
		bl->pwm_freq_pre_divider = f;
		drm_dbg_kms(aux->drm_dev, "%s: Using backlight frequency from driver (%dHz)\n",
			    aux->name, driver_pwm_freq_hz);
	}

	return 0;
}

static int
drm_edp_backlight_set_enable(struct drm_dp_aux *aux, const struct drm_edp_backlight_info *bl,
			     bool enable)
{
	int ret;
	u8 buf;

	/* The panel uses something other then DPCD for enabling its backlight */
	if (!bl->aux_enable)
		return 0;

	ret = drm_dp_dpcd_readb(aux, DP_EDP_DISPLAY_CONTROL_REGISTER, &buf);
	if (ret != 1) {
		drm_err(aux->drm_dev, "%s: Failed to read eDP display control register: %d\n",
			aux->name, ret);
		return ret < 0 ? ret : -EIO;
	}
	if (enable)
		buf |= DP_EDP_BACKLIGHT_ENABLE;
	else
		buf &= ~DP_EDP_BACKLIGHT_ENABLE;

	ret = drm_dp_dpcd_writeb(aux, DP_EDP_DISPLAY_CONTROL_REGISTER, buf);
	if (ret != 1) {
		drm_err(aux->drm_dev, "%s: Failed to write eDP display control register: %d\n",
			aux->name, ret);
		return ret < 0 ? ret : -EIO;
	}

	return 0;
}

int drm_edp_backlight_set_level(struct drm_dp_aux *aux, const struct drm_edp_backlight_info *bl,
				u16 level)
{
	int ret;
	u8 buf[2] = { 0 };

	if (bl->lsb_reg_used) {
		buf[0] = (level & 0xff00) >> 8;
		buf[1] = (level & 0x00ff);
	} else {
		buf[0] = level;
	}

	ret = drm_dp_dpcd_write(aux, DP_EDP_BACKLIGHT_BRIGHTNESS_MSB, buf, sizeof(buf));
	if (ret != sizeof(buf)) {
		drm_err(aux->drm_dev,
			"%s: Failed to write aux backlight level: %d\n",
			aux->name, ret);
		return ret < 0 ? ret : -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(drm_edp_backlight_set_level);

int drm_edp_backlight_enable(struct drm_dp_aux *aux, const struct drm_edp_backlight_info *bl,
			     const u16 level)
{
	int ret;
	u8 dpcd_buf = DP_EDP_BACKLIGHT_CONTROL_MODE_DPCD;

	if (bl->pwmgen_bit_count) {
		ret = drm_dp_dpcd_writeb(aux, DP_EDP_PWMGEN_BIT_COUNT, bl->pwmgen_bit_count);
		if (ret != 1)
			drm_dbg_kms(aux->drm_dev, "%s: Failed to write aux pwmgen bit count: %d\n",
				    aux->name, ret);
	}

	if (bl->pwm_freq_pre_divider) {
		ret = drm_dp_dpcd_writeb(aux, DP_EDP_BACKLIGHT_FREQ_SET, bl->pwm_freq_pre_divider);
		if (ret != 1)
			drm_dbg_kms(aux->drm_dev,
				    "%s: Failed to write aux backlight frequency: %d\n",
				    aux->name, ret);
		else
			dpcd_buf |= DP_EDP_BACKLIGHT_FREQ_AUX_SET_ENABLE;
	}

	ret = drm_dp_dpcd_writeb(aux, DP_EDP_BACKLIGHT_MODE_SET_REGISTER, dpcd_buf);
	if (ret != 1) {
		drm_dbg_kms(aux->drm_dev, "%s: Failed to write aux backlight mode: %d\n",
			    aux->name, ret);
		return ret < 0 ? ret : -EIO;
	}

	ret = drm_edp_backlight_set_level(aux, bl, level);
	if (ret < 0)
		return ret;
	ret = drm_edp_backlight_set_enable(aux, bl, true);
	if (ret < 0)
		return ret;

	return 0;
}
EXPORT_SYMBOL(drm_edp_backlight_enable);

int drm_edp_backlight_disable(struct drm_dp_aux *aux, const struct drm_edp_backlight_info *bl)
{
	int ret;

	ret = drm_edp_backlight_set_enable(aux, bl, false);
	if (ret < 0)
		return ret;

	return 0;
}
EXPORT_SYMBOL(drm_edp_backlight_disable);

int
drm_edp_backlight_init(struct drm_dp_aux *aux, struct drm_edp_backlight_info *bl,
		       u16 driver_pwm_freq_hz, const u8 edp_dpcd[EDP_DISPLAY_CTL_CAP_SIZE],
		       u16 *current_level, u8 *current_mode)
{
	int ret;

	if (edp_dpcd[1] & DP_EDP_BACKLIGHT_AUX_ENABLE_CAP)
		bl->aux_enable = true;
	if (edp_dpcd[2] & DP_EDP_BACKLIGHT_BRIGHTNESS_BYTE_COUNT)
		bl->lsb_reg_used = true;

	ret = drm_edp_backlight_probe_max(aux, bl, driver_pwm_freq_hz, edp_dpcd);
	if (ret < 0)
		return ret;

	ret = drm_edp_backlight_probe_level(aux, bl, current_mode);
	if (ret < 0)
		return ret;
	*current_level = ret;

	drm_dbg_kms(aux->drm_dev,
		    "%s: Found backlight level=%d/%d pwm_freq_pre_divider=%d mode=%x\n",
		    aux->name, *current_level, bl->max, bl->pwm_freq_pre_divider, *current_mode);
	drm_dbg_kms(aux->drm_dev,
		    "%s: Backlight caps: pwmgen_bit_count=%d lsb_reg_used=%d aux_enable=%d\n",
		    aux->name, bl->pwmgen_bit_count, bl->lsb_reg_used, bl->aux_enable);
	return 0;
}
EXPORT_SYMBOL(drm_edp_backlight_init);
#endif /* DRM_EDP_BACKLIGHT_NOT_PRESENT */

/**
 * drm_hdmi_sink_max_frl_rate - get the max frl rate, if supported
 * @connector - connector with HDMI sink
 *
 * RETURNS:
 * max frl rate supported by the HDMI sink, 0 if FRL not supported
 */
int drm_hdmi_sink_max_frl_rate(struct drm_connector *connector)
{
        int max_lanes = connector->display_info.hdmi.max_lanes;
        int rate_per_lane = connector->display_info.hdmi.max_frl_rate_per_lane;

        return max_lanes * rate_per_lane;
}
EXPORT_SYMBOL(drm_hdmi_sink_max_frl_rate);

/**
 * drm_hdmi_sink_dsc_max_frl_rate - get the max frl rate from HDMI sink with
 * DSC1.2 compression.
 * @connector - connector with HDMI sink
 *
 * RETURNS:
 * max frl rate supported by the HDMI sink with DSC1.2, 0 if FRL not supported
 */
int drm_hdmi_sink_dsc_max_frl_rate(struct drm_connector *connector)
{
        int max_dsc_lanes, dsc_rate_per_lane;

        if (!connector->display_info.hdmi.dsc_cap.v_1p2)
                return 0;

        max_dsc_lanes = connector->display_info.hdmi.dsc_cap.max_lanes;
        dsc_rate_per_lane = connector->display_info.hdmi.dsc_cap.max_frl_rate_per_lane;

        return max_dsc_lanes * dsc_rate_per_lane;
}
EXPORT_SYMBOL(drm_hdmi_sink_dsc_max_frl_rate);
