#ifndef _BACKPORT_DRM_DP_HELPER_H_
#define _BACKPORT_DRM_DP_HELPER_H_

#ifdef BPM_DRM_DP_HELPER_DIR_DISPLAY_PRESENT
#include_next  <drm/display/drm_dp_helper.h>
#elif defined(BPM_DRM_DP_HELPER_DIR_DP_PRESENT)
#include_next  <drm/dp/drm_dp_helper.h>
#else
#include_next  <drm/drm_dp_helper.h>
#endif /* DRM_DP_HELPER_DIR_DISPLAY_PRESENT */

#ifdef BPM_DP_READ_LTTPR_CAPS_DPCD_ARG_NOT_PRESENT

#define drm_dp_read_lttpr_common_caps LINUX_I915_BACKPORT(drm_dp_read_lttpr_common_caps)
int drm_dp_read_lttpr_common_caps(struct drm_dp_aux *aux,
                                  u8 caps[DP_LTTPR_COMMON_CAP_SIZE]);

#define drm_dp_read_lttpr_phy_caps LINUX_I915_BACKPORT(drm_dp_read_lttpr_phy_caps)
int drm_dp_read_lttpr_phy_caps(struct drm_dp_aux *aux,
                               enum drm_dp_phy dp_phy,
                               u8 caps[DP_LTTPR_PHY_CAP_SIZE]);
#endif

#ifdef DRM_DP_GET_ADJUST_NOT_PRESENT

#define drm_dp_get_adjust_tx_ffe_preset LINUX_I915_BACKPORT(drm_dp_get_adjust_tx_ffe_preset)
u8 drm_dp_get_adjust_tx_ffe_preset(const u8 link_status[DP_LINK_STATUS_SIZE],
				   int lane);

#define DP_MAIN_LINK_CHANNEL_CODING_PHY_REPEATER        0xf0006 /* 2.0 */
#define DP_PHY_REPEATER_128B132B_SUPPORTED              (1 << 0)
/* See DP_128B132B_SUPPORTED_LINK_RATES for values */
#define DP_PHY_REPEATER_128B132B_RATES                  0xf0007 /* 2.0 */

#define DP_EDP_BACKLIGHT_CONTROL_MODE_MASK              (3 << 0)
#define DP_EDP_BACKLIGHT_CONTROL_MODE_DPCD              (2 << 0)
#define DP_EDP_BACKLIGHT_FREQ_AUX_SET_ENABLE            (1 << 3)
#define DP_EDP_BACKLIGHT_AUX_ENABLE_CAP                 (1 << 2)
#define DP_EDP_BACKLIGHT_BRIGHTNESS_BYTE_COUNT          (1 << 2)
#define DP_EDP_TCON_BACKLIGHT_ADJUSTMENT_CAP            (1 << 0)
#define DP_EDP_BACKLIGHT_BRIGHTNESS_AUX_SET_CAP         (1 << 1)
#define DP_EDP_BACKLIGHT_FREQ_AUX_SET_CAP               (1 << 5)
#define DP_EDP_BACKLIGHT_ENABLE                         (1 << 0)
#define DP_EDP_PWMGEN_BIT_COUNT_MASK                    (0x1f << 0)
#define DP_EDP_PWMGEN_BIT_COUNT                         0x724
#define DP_EDP_BACKLIGHT_FREQ_SET                       0x728
#define DP_EDP_BACKLIGHT_MODE_SET_REGISTER              0x721
#define EDP_DISPLAY_CTL_CAP_SIZE                        3
#define DP_EDP_BACKLIGHT_FREQ_BASE_KHZ                  27000
#define DP_EDP_DISPLAY_CONTROL_REGISTER                 0x720
#define DP_EDP_BACKLIGHT_BRIGHTNESS_MSB                 0x722

#endif /* DRM_DP_GET_ADJUST_NOT_PRESENT */

#ifdef DRM_EDP_BACKLIGHT_NOT_PRESENT
/**
* struct drm_edp_backlight_info - Probed eDP backlight info struct
* @pwmgen_bit_count: The pwmgen bit count
* @pwm_freq_pre_divider: The PWM frequency pre-divider value being used for this backlight, if any
* @max: The maximum backlight level that may be set
* @lsb_reg_used: Do we also write values to the DP_EDP_BACKLIGHT_BRIGHTNESS_LSB register?
* @aux_enable: Does the panel support the AUX enable cap?
*
* This structure contains various data about an eDP backlight, which can be populated by using
* drm_edp_backlight_init().
*/
#define drm_edp_backlight_info LINUX_I915_BACKPORT(drm_edp_backlight_info)
struct drm_edp_backlight_info {
		u8 pwmgen_bit_count;
		u8 pwm_freq_pre_divider;
		u16 max;

		bool lsb_reg_used : 1;
		bool aux_enable : 1;
		bool aux_set : 1;
};

#define drm_edp_backlight_init LINUX_I915_BACKPORT(drm_edp_backlight_init)
int
drm_edp_backlight_init(struct drm_dp_aux *aux, struct drm_edp_backlight_info *bl,
		       u16 driver_pwm_freq_hz, const u8 edp_dpcd[EDP_DISPLAY_CTL_CAP_SIZE],
		       u16 *current_level, u8 *current_mode);
#define drm_edp_backlight_set_level LINUX_I915_BACKPORT(drm_edp_backlight_set_level)
int drm_edp_backlight_set_level(struct drm_dp_aux *aux, const struct drm_edp_backlight_info *bl,
				u16 level);
#define drm_edp_backlight_enable LINUX_I915_BACKPORT(drm_edp_backlight_enable)
int drm_edp_backlight_enable(struct drm_dp_aux *aux, const struct drm_edp_backlight_info *bl,
			     u16 level);
#define drm_edp_backlight_disable LINUX_I915_BACKPORT(drm_edp_backlight_disable)
int drm_edp_backlight_disable(struct drm_dp_aux *aux, const struct drm_edp_backlight_info *bl);

#ifndef DRM_EDP_BACKLIGHT_SUPPORT_PRESENT
#define drm_edp_backlight_supported LINUX_I915_BACKPORT(drm_edp_backlight_supported)
static inline bool
drm_edp_backlight_supported(const u8 edp_dpcd[EDP_DISPLAY_CTL_CAP_SIZE])
{
       return (edp_dpcd[1] & DP_EDP_TCON_BACKLIGHT_ADJUSTMENT_CAP) &&
	       (edp_dpcd[2] & DP_EDP_BACKLIGHT_BRIGHTNESS_AUX_SET_CAP);
}
#endif /* DRM_EDP_BACKLIGHT_SUPPORT_PRESENT */
#endif /* DRM_EDP_BACKLIGHT_NOT_PRESENT */

#ifdef BPM_DISABLE_DRM_DMABUF
#define drm_hdmi_sink_max_frl_rate LINUX_I915_BACKPORT(drm_hdmi_sink_max_frl_rate)
int drm_hdmi_sink_max_frl_rate(struct drm_connector *connector);

#define drm_hdmi_sink_dsc_max_frl_rate LINUX_I915_BACKPORT(drm_hdmi_sink_dsc_max_frl_rate)
int drm_hdmi_sink_dsc_max_frl_rate(struct drm_connector *connector);
#endif

#ifdef BPM_DRM_DP_DSC_SINK_SUPPORTS_FORMAT_NOT_PRESENT
/**
 * drm_dp_dsc_sink_supports_format() - check if sink supports DSC with given output format
 * @dsc_dpcd : DSC-capability DPCDs of the sink
 * @output_format: output_format which is to be checked
 *
 * Returns true if the sink supports DSC with the given output_format, false otherwise.
 */
static inline bool
drm_dp_dsc_sink_supports_format(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE], u8 output_format)
{
       return dsc_dpcd[DP_DSC_DEC_COLOR_FORMAT_CAP - DP_DSC_SUPPORT] & output_format;
}
#endif
#endif /* _BACKPORT_DRM_DP_HELPER_H_ */
