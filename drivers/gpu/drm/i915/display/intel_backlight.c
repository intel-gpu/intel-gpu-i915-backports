// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <linux/kernel.h>
#include <linux/pwm.h>
#include <linux/string_helpers.h>

#include "intel_backlight.h"
#include "intel_backlight_regs.h"
#include "intel_connector.h"
#include "intel_de.h"
#include "intel_display_types.h"
#include "intel_dp_aux_backlight.h"
#include "intel_dsi_dcs_backlight.h"
#include "intel_panel.h"
#include "intel_pci_config.h"
#ifndef BPM_DRM_LUMINANCE_RANGE_INFO_NOT_PRESENT
#include "intel_pps.h"
#endif

/**
 * scale - scale values from one range to another
 * @source_val: value in range [@source_min..@source_max]
 * @source_min: minimum legal value for @source_val
 * @source_max: maximum legal value for @source_val
 * @target_min: corresponding target value for @source_min
 * @target_max: corresponding target value for @source_max
 *
 * Return @source_val in range [@source_min..@source_max] scaled to range
 * [@target_min..@target_max].
 */
static u32 scale(u32 source_val,
		 u32 source_min, u32 source_max,
		 u32 target_min, u32 target_max)
{
	u64 target_val;

	WARN_ON(source_min > source_max);
	WARN_ON(target_min > target_max);

	/* defensive */
	source_val = clamp(source_val, source_min, source_max);

	/* avoid overflows */
	target_val = mul_u32_u32(source_val - source_min,
				 target_max - target_min);
	target_val = DIV_ROUND_CLOSEST_ULL(target_val, source_max - source_min);
	target_val += target_min;

	return target_val;
}

/*
 * Scale user_level in range [0..user_max] to [0..hw_max], clamping the result
 * to [hw_min..hw_max].
 */
static u32 clamp_user_to_hw(struct intel_connector *connector,
			    u32 user_level, u32 user_max)
{
	struct intel_panel *panel = &connector->panel;
	u32 hw_level;

	hw_level = scale(user_level, 0, user_max, 0, panel->backlight.max);
	hw_level = clamp(hw_level, panel->backlight.min, panel->backlight.max);

	return hw_level;
}

/* Scale hw_level in range [hw_min..hw_max] to [0..user_max]. */
static u32 scale_hw_to_user(struct intel_connector *connector,
			    u32 hw_level, u32 user_max)
{
	struct intel_panel *panel = &connector->panel;

	return scale(hw_level, panel->backlight.min, panel->backlight.max,
		     0, user_max);
}

u32 intel_backlight_invert_pwm_level(struct intel_connector *connector, u32 val)
{
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;

	drm_WARN_ON(&dev_priv->drm, panel->backlight.pwm_level_max == 0);

	if (dev_priv->params.invert_brightness < 0)
		return val;

	if (dev_priv->params.invert_brightness > 0 ||
	    dev_priv->quirks & QUIRK_INVERT_BRIGHTNESS) {
		return panel->backlight.pwm_level_max - val + panel->backlight.pwm_level_min;
	}

	return val;
}

void intel_backlight_set_pwm_level(const struct drm_connector_state *conn_state, u32 val)
{
	struct intel_connector *connector = to_intel_connector(conn_state->connector);
	struct drm_i915_private *i915 = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;

	drm_dbg_kms(&i915->drm, "set backlight PWM = %d\n", val);
	panel->backlight.pwm_funcs->set(conn_state, val);
}

u32 intel_backlight_level_to_pwm(struct intel_connector *connector, u32 val)
{
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;

	drm_WARN_ON_ONCE(&dev_priv->drm,
			 panel->backlight.max == 0 || panel->backlight.pwm_level_max == 0);

	val = scale(val, panel->backlight.min, panel->backlight.max,
		    panel->backlight.pwm_level_min, panel->backlight.pwm_level_max);

	return intel_backlight_invert_pwm_level(connector, val);
}

u32 intel_backlight_level_from_pwm(struct intel_connector *connector, u32 val)
{
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;

	drm_WARN_ON_ONCE(&dev_priv->drm,
			 panel->backlight.max == 0 || panel->backlight.pwm_level_max == 0);

	if (dev_priv->params.invert_brightness > 0 ||
	    (dev_priv->params.invert_brightness == 0 && dev_priv->quirks & QUIRK_INVERT_BRIGHTNESS))
		val = panel->backlight.pwm_level_max - (val - panel->backlight.pwm_level_min);

	return scale(val, panel->backlight.pwm_level_min, panel->backlight.pwm_level_max,
		     panel->backlight.min, panel->backlight.max);
}

static u32 bxt_get_backlight(struct intel_connector *connector, enum pipe unused)
{
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;

	return intel_de_read(dev_priv,
			     BXT_BLC_PWM_DUTY(panel->backlight.controller));
}

static void bxt_set_backlight(const struct drm_connector_state *conn_state, u32 level)
{
	struct intel_connector *connector = to_intel_connector(conn_state->connector);
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;

	intel_de_write(dev_priv,
		       BXT_BLC_PWM_DUTY(panel->backlight.controller), level);
}

static void
intel_panel_actually_set_backlight(const struct drm_connector_state *conn_state, u32 level)
{
	struct intel_connector *connector = to_intel_connector(conn_state->connector);
	struct drm_i915_private *i915 = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;

	drm_dbg_kms(&i915->drm, "set backlight level = %d\n", level);

	panel->backlight.funcs->set(conn_state, level);
}

/* set backlight brightness to level in range [0..max], assuming hw min is
 * respected.
 */
void intel_backlight_set_acpi(const struct drm_connector_state *conn_state,
			      u32 user_level, u32 user_max)
{
	struct intel_connector *connector = to_intel_connector(conn_state->connector);
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;
	u32 hw_level;

	/*
	 * Lack of crtc may occur during driver init because
	 * connection_mutex isn't held across the entire backlight
	 * setup + modeset readout, and the BIOS can issue the
	 * requests at any time.
	 */
	if (!panel->backlight.present || !conn_state->crtc)
		return;

	mutex_lock(&dev_priv->backlight_lock);

	drm_WARN_ON(&dev_priv->drm, panel->backlight.max == 0);

	hw_level = clamp_user_to_hw(connector, user_level, user_max);
	panel->backlight.level = hw_level;

	if (panel->backlight.device)
		panel->backlight.device->props.brightness =
			scale_hw_to_user(connector,
					 panel->backlight.level,
					 panel->backlight.device->props.max_brightness);

	if (panel->backlight.enabled)
		intel_panel_actually_set_backlight(conn_state, hw_level);

	mutex_unlock(&dev_priv->backlight_lock);
}

static void cnp_disable_backlight(const struct drm_connector_state *old_conn_state, u32 val)
{
	struct intel_connector *connector = to_intel_connector(old_conn_state->connector);
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;
	u32 tmp;

	intel_backlight_set_pwm_level(old_conn_state, val);

	tmp = intel_de_read(dev_priv,
			    BXT_BLC_PWM_CTL(panel->backlight.controller));
	intel_de_write(dev_priv, BXT_BLC_PWM_CTL(panel->backlight.controller),
		       tmp & ~BXT_BLC_PWM_ENABLE);
}

void intel_backlight_disable(const struct drm_connector_state *old_conn_state)
{
	struct intel_connector *connector = to_intel_connector(old_conn_state->connector);
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;

	if (!panel->backlight.present)
		return;

	/*
	 * Do not disable backlight on the vga_switcheroo path. When switching
	 * away from i915, the other client may depend on i915 to handle the
	 * backlight. This will leave the backlight on unnecessarily when
	 * another client is not activated.
	 */
	if (dev_priv->drm.switch_power_state == DRM_SWITCH_POWER_CHANGING) {
		drm_dbg_kms(&dev_priv->drm,
			    "Skipping backlight disable on vga switch\n");
		return;
	}

	mutex_lock(&dev_priv->backlight_lock);

	if (panel->backlight.device)
		panel->backlight.device->props.power = FB_BLANK_POWERDOWN;
	panel->backlight.enabled = false;
	panel->backlight.funcs->disable(old_conn_state, 0);

	mutex_unlock(&dev_priv->backlight_lock);
}

static void cnp_enable_backlight(const struct intel_crtc_state *crtc_state,
				 const struct drm_connector_state *conn_state, u32 level)
{
	struct intel_connector *connector = to_intel_connector(conn_state->connector);
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;
	u32 pwm_ctl;

	pwm_ctl = intel_de_read(dev_priv,
				BXT_BLC_PWM_CTL(panel->backlight.controller));
	if (pwm_ctl & BXT_BLC_PWM_ENABLE) {
		drm_dbg_kms(&dev_priv->drm, "backlight already enabled\n");
		pwm_ctl &= ~BXT_BLC_PWM_ENABLE;
		intel_de_write(dev_priv,
			       BXT_BLC_PWM_CTL(panel->backlight.controller),
			       pwm_ctl);
	}

	intel_de_write(dev_priv,
		       BXT_BLC_PWM_FREQ(panel->backlight.controller),
		       panel->backlight.pwm_level_max);

	intel_backlight_set_pwm_level(conn_state, level);

	pwm_ctl = 0;
	if (panel->backlight.active_low_pwm)
		pwm_ctl |= BXT_BLC_PWM_POLARITY;

	intel_de_write(dev_priv, BXT_BLC_PWM_CTL(panel->backlight.controller),
		       pwm_ctl);
	intel_de_posting_read(dev_priv,
			      BXT_BLC_PWM_CTL(panel->backlight.controller));
	intel_de_write(dev_priv, BXT_BLC_PWM_CTL(panel->backlight.controller),
		       pwm_ctl | BXT_BLC_PWM_ENABLE);
}

static void __intel_backlight_enable(const struct intel_crtc_state *crtc_state,
				     const struct drm_connector_state *conn_state)
{
	struct intel_connector *connector = to_intel_connector(conn_state->connector);
	struct intel_panel *panel = &connector->panel;

	WARN_ON(panel->backlight.max == 0);

	if (panel->backlight.level <= panel->backlight.min) {
		panel->backlight.level = panel->backlight.max;
		if (panel->backlight.device)
			panel->backlight.device->props.brightness =
				scale_hw_to_user(connector,
						 panel->backlight.level,
						 panel->backlight.device->props.max_brightness);
	}

	panel->backlight.funcs->enable(crtc_state, conn_state, panel->backlight.level);
	panel->backlight.enabled = true;
	if (panel->backlight.device)
		panel->backlight.device->props.power = FB_BLANK_UNBLANK;
}

void intel_backlight_enable(const struct intel_crtc_state *crtc_state,
			    const struct drm_connector_state *conn_state)
{
	struct intel_connector *connector = to_intel_connector(conn_state->connector);
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;
	enum pipe pipe = to_intel_crtc(crtc_state->uapi.crtc)->pipe;

	if (!panel->backlight.present)
		return;

	drm_dbg_kms(&dev_priv->drm, "pipe %c\n", pipe_name(pipe));

	mutex_lock(&dev_priv->backlight_lock);

	__intel_backlight_enable(crtc_state, conn_state);

	mutex_unlock(&dev_priv->backlight_lock);
}

#if IS_ENABLED(CONFIG_BACKLIGHT_CLASS_DEVICE)
static u32 intel_panel_get_backlight(struct intel_connector *connector)
{
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;
	u32 val = 0;

	mutex_lock(&dev_priv->backlight_lock);

	if (panel->backlight.enabled)
		val = panel->backlight.funcs->get(connector, intel_connector_get_pipe(connector));

	mutex_unlock(&dev_priv->backlight_lock);

	drm_dbg_kms(&dev_priv->drm, "get backlight PWM = %d\n", val);
	return val;
}

/* Scale user_level in range [0..user_max] to [hw_min..hw_max]. */
static u32 scale_user_to_hw(struct intel_connector *connector,
			    u32 user_level, u32 user_max)
{
	struct intel_panel *panel = &connector->panel;

	return scale(user_level, 0, user_max,
		     panel->backlight.min, panel->backlight.max);
}

/* set backlight brightness to level in range [0..max], scaling wrt hw min */
static void intel_panel_set_backlight(const struct drm_connector_state *conn_state,
				      u32 user_level, u32 user_max)
{
	struct intel_connector *connector = to_intel_connector(conn_state->connector);
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;
	u32 hw_level;

	if (!panel->backlight.present)
		return;

	mutex_lock(&dev_priv->backlight_lock);

	drm_WARN_ON(&dev_priv->drm, panel->backlight.max == 0);

	hw_level = scale_user_to_hw(connector, user_level, user_max);
	panel->backlight.level = hw_level;

	if (panel->backlight.enabled)
		intel_panel_actually_set_backlight(conn_state, hw_level);

	mutex_unlock(&dev_priv->backlight_lock);
}

static int intel_backlight_device_update_status(struct backlight_device *bd)
{
	struct intel_connector *connector = bl_get_data(bd);
	struct intel_panel *panel = &connector->panel;
	struct drm_device *dev = connector->base.dev;

	drm_modeset_lock(&dev->mode_config.connection_mutex, NULL);
	DRM_DEBUG_KMS("updating intel_backlight, brightness=%d/%d\n",
		      bd->props.brightness, bd->props.max_brightness);
	intel_panel_set_backlight(connector->base.state, bd->props.brightness,
				  bd->props.max_brightness);

	/*
	 * Allow flipping bl_power as a sub-state of enabled. Sadly the
	 * backlight class device does not make it easy to differentiate
	 * between callbacks for brightness and bl_power, so our backlight_power
	 * callback needs to take this into account.
	 */
	if (panel->backlight.enabled) {
		if (panel->backlight.power) {
			bool enable = bd->props.power == FB_BLANK_UNBLANK &&
				bd->props.brightness != 0;
			panel->backlight.power(connector, enable);
		}
	} else {
		bd->props.power = FB_BLANK_POWERDOWN;
	}

	drm_modeset_unlock(&dev->mode_config.connection_mutex);
	return 0;
}

static int intel_backlight_device_get_brightness(struct backlight_device *bd)
{
	struct intel_connector *connector = bl_get_data(bd);
	struct drm_device *dev = connector->base.dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	intel_wakeref_t wakeref;
	int ret = 0;

	with_intel_runtime_pm(&dev_priv->runtime_pm, wakeref) {
		u32 hw_level;

		drm_modeset_lock(&dev->mode_config.connection_mutex, NULL);

		hw_level = intel_panel_get_backlight(connector);
		ret = scale_hw_to_user(connector,
				       hw_level, bd->props.max_brightness);

		drm_modeset_unlock(&dev->mode_config.connection_mutex);
	}

	return ret;
}

static const struct backlight_ops intel_backlight_device_ops = {
	.update_status = intel_backlight_device_update_status,
	.get_brightness = intel_backlight_device_get_brightness,
};

int intel_backlight_device_register(struct intel_connector *connector)
{
	struct drm_i915_private *i915 = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;
	struct backlight_properties props;
	struct backlight_device *bd;
	const char *name;
	int ret = 0;

	if (WARN_ON(panel->backlight.device))
		return -ENODEV;

	if (!panel->backlight.present)
		return 0;

	WARN_ON(panel->backlight.max == 0);

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;

	/*
	 * Note: Everything should work even if the backlight device max
	 * presented to the userspace is arbitrarily chosen.
	 */
	props.max_brightness = panel->backlight.max;
	props.brightness = scale_hw_to_user(connector,
					    panel->backlight.level,
					    props.max_brightness);

	if (panel->backlight.enabled)
		props.power = FB_BLANK_UNBLANK;
	else
		props.power = FB_BLANK_POWERDOWN;

	name = kstrdup("intel_backlight", GFP_KERNEL);
	if (!name)
		return -ENOMEM;

#ifdef BPM_BACKLIGHT_DEV_GET_BY_NAME_NOT_PRESENT
	bd = backlight_device_register(name, connector->base.kdev, connector,
			&intel_backlight_device_ops, &props);

	/*
	 * Using the same name independent of the drm device or connector
	 * prevents registration of multiple backlight devices in the
	 * driver. However, we need to use the default name for backward
	 * compatibility. Use unique names for subsequent backlight devices as a
	 * fallback when the default name already exists.
	 */
	if (IS_ERR(bd) && PTR_ERR(bd) == -EEXIST) {
		kfree(name);
		name = kasprintf(GFP_KERNEL, "card%d-%s-backlight",
				i915->drm.primary->index, connector->base.name);
		if (!name)
			return -ENOMEM;

		bd = backlight_device_register(name, connector->base.kdev, connector,
				&intel_backlight_device_ops, &props);
	}
#else
	bd = backlight_device_get_by_name(name);
	if (bd) {
		put_device(&bd->dev);
		/*
		 * Using the same name independent of the drm device or connector
		 * prevents registration of multiple backlight devices in the
		 * driver. However, we need to use the default name for backward
		 * compatibility. Use unique names for subsequent backlight devices as a
		 * fallback when the default name already exists.
		 */
		kfree(name);
		name = kasprintf(GFP_KERNEL, "card%d-%s-backlight",
				 i915->drm.primary->index, connector->base.name);
		if (!name)
			return -ENOMEM;
	}
	bd = backlight_device_register(name, connector->base.kdev, connector,
				       &intel_backlight_device_ops, &props);
#endif

	if (IS_ERR(bd)) {
		drm_err(&i915->drm,
			"[CONNECTOR:%d:%s] backlight device %s register failed: %ld\n",
			connector->base.base.id, connector->base.name, name, PTR_ERR(bd));
		ret = PTR_ERR(bd);
		goto out;
	}

	panel->backlight.device = bd;

	drm_dbg_kms(&i915->drm,
		    "[CONNECTOR:%d:%s] backlight device %s registered\n",
		    connector->base.base.id, connector->base.name, name);

out:
	kfree(name);

	return ret;
}

void intel_backlight_device_unregister(struct intel_connector *connector)
{
	struct intel_panel *panel = &connector->panel;

	if (panel->backlight.device) {
		backlight_device_unregister(panel->backlight.device);
		panel->backlight.device = NULL;
	}
}
#endif /* CONFIG_BACKLIGHT_CLASS_DEVICE */

/*
 * CNP: PWM clock frequency is 19.2 MHz or 24 MHz.
 *      PWM increment = 1
 */
static u32 cnp_hz_to_pwm(struct intel_connector *connector, u32 pwm_freq_hz)
{
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);

	return DIV_ROUND_CLOSEST(KHz(RUNTIME_INFO(dev_priv)->rawclk_freq),
				 pwm_freq_hz);
}

static u16 get_vbt_pwm_freq(struct intel_connector *connector)
{
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	u16 pwm_freq_hz = connector->panel.vbt.backlight.pwm_freq_hz;

	if (pwm_freq_hz) {
		drm_dbg_kms(&dev_priv->drm,
			    "VBT defined backlight frequency %u Hz\n",
			    pwm_freq_hz);
	} else {
		pwm_freq_hz = 200;
		drm_dbg_kms(&dev_priv->drm,
			    "default backlight frequency %u Hz\n",
			    pwm_freq_hz);
	}

	return pwm_freq_hz;
}

static u32 get_backlight_max_vbt(struct intel_connector *connector)
{
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;
	u16 pwm_freq_hz = get_vbt_pwm_freq(connector);
	u32 pwm;

	if (!panel->backlight.pwm_funcs->hz_to_pwm) {
		drm_dbg_kms(&dev_priv->drm,
			    "backlight frequency conversion not supported\n");
		return 0;
	}

	pwm = panel->backlight.pwm_funcs->hz_to_pwm(connector, pwm_freq_hz);
	if (!pwm) {
		drm_dbg_kms(&dev_priv->drm,
			    "backlight frequency conversion failed\n");
		return 0;
	}

	return pwm;
}

/*
 * Note: The setup hooks can't assume pipe is set!
 */
static u32 get_backlight_min_vbt(struct intel_connector *connector)
{
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;
	int min;

	drm_WARN_ON(&dev_priv->drm, panel->backlight.pwm_level_max == 0);

	/*
	 * XXX: If the vbt value is 255, it makes min equal to max, which leads
	 * to problems. There are such machines out there. Either our
	 * interpretation is wrong or the vbt has bogus data. Or both. Safeguard
	 * against this by letting the minimum be at most (arbitrarily chosen)
	 * 25% of the max.
	 */
	min = clamp_t(int, connector->panel.vbt.backlight.min_brightness, 0, 64);
	if (min != connector->panel.vbt.backlight.min_brightness) {
		drm_dbg_kms(&dev_priv->drm,
			    "clamping VBT min backlight %d/255 to %d/255\n",
			    connector->panel.vbt.backlight.min_brightness, min);
	}

	/* vbt value is a coefficient in range [0..255] */
	return scale(min, 0, 255, 0, panel->backlight.pwm_level_max);
}

static int
cnp_setup_backlight(struct intel_connector *connector, enum pipe unused)
{
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;
	u32 pwm_ctl;

	/*
	 * CNP has the BXT implementation of backlight, but with only one
	 * controller. TODO: ICP has multiple controllers but we only use
	 * controller 0 for now.
	 */
	panel->backlight.controller = 0;

	pwm_ctl = intel_de_read(dev_priv,
				BXT_BLC_PWM_CTL(panel->backlight.controller));

	panel->backlight.active_low_pwm = pwm_ctl & BXT_BLC_PWM_POLARITY;
	panel->backlight.pwm_level_max =
		intel_de_read(dev_priv, BXT_BLC_PWM_FREQ(panel->backlight.controller));

	if (!panel->backlight.pwm_level_max)
		panel->backlight.pwm_level_max = get_backlight_max_vbt(connector);

	if (!panel->backlight.pwm_level_max)
		return -ENODEV;

	panel->backlight.pwm_level_min = get_backlight_min_vbt(connector);

	panel->backlight.pwm_enabled = pwm_ctl & BXT_BLC_PWM_ENABLE;

	return 0;
}

static void intel_pwm_set_backlight(const struct drm_connector_state *conn_state, u32 level)
{
	struct intel_connector *connector = to_intel_connector(conn_state->connector);
	struct intel_panel *panel = &connector->panel;

	panel->backlight.pwm_funcs->set(conn_state,
					intel_backlight_invert_pwm_level(connector, level));
}

static u32 intel_pwm_get_backlight(struct intel_connector *connector, enum pipe pipe)
{
	struct intel_panel *panel = &connector->panel;

	return intel_backlight_invert_pwm_level(connector,
					    panel->backlight.pwm_funcs->get(connector, pipe));
}

static void intel_pwm_enable_backlight(const struct intel_crtc_state *crtc_state,
				       const struct drm_connector_state *conn_state, u32 level)
{
	struct intel_connector *connector = to_intel_connector(conn_state->connector);
	struct intel_panel *panel = &connector->panel;

	panel->backlight.pwm_funcs->enable(crtc_state, conn_state,
					   intel_backlight_invert_pwm_level(connector, level));
}

static void intel_pwm_disable_backlight(const struct drm_connector_state *conn_state, u32 level)
{
	struct intel_connector *connector = to_intel_connector(conn_state->connector);
	struct intel_panel *panel = &connector->panel;

	panel->backlight.pwm_funcs->disable(conn_state,
					    intel_backlight_invert_pwm_level(connector, level));
}

static int intel_pwm_setup_backlight(struct intel_connector *connector, enum pipe pipe)
{
	struct intel_panel *panel = &connector->panel;
	int ret = panel->backlight.pwm_funcs->setup(connector, pipe);

	if (ret < 0)
		return ret;

	panel->backlight.min = panel->backlight.pwm_level_min;
	panel->backlight.max = panel->backlight.pwm_level_max;
	panel->backlight.level = intel_pwm_get_backlight(connector, pipe);
	panel->backlight.enabled = panel->backlight.pwm_enabled;

	return 0;
}

void intel_backlight_update(struct intel_atomic_state *state,
			    struct intel_encoder *encoder,
			    const struct intel_crtc_state *crtc_state,
			    const struct drm_connector_state *conn_state)
{
	struct intel_connector *connector = to_intel_connector(conn_state->connector);
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;

	if (!panel->backlight.present)
		return;

	mutex_lock(&dev_priv->backlight_lock);
	if (!panel->backlight.enabled)
		__intel_backlight_enable(crtc_state, conn_state);

	mutex_unlock(&dev_priv->backlight_lock);
}

int intel_backlight_setup(struct intel_connector *connector, enum pipe pipe)
{
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_panel *panel = &connector->panel;
	int ret;

	if (!connector->panel.vbt.backlight.present) {
		if (dev_priv->quirks & QUIRK_BACKLIGHT_PRESENT) {
			drm_dbg_kms(&dev_priv->drm,
				    "no backlight present per VBT, but present per quirk\n");
		} else {
			drm_dbg_kms(&dev_priv->drm,
				    "no backlight present per VBT\n");
			return 0;
		}
	}

	/* ensure intel_panel has been initialized first */
	if (drm_WARN_ON(&dev_priv->drm, !panel->backlight.funcs))
		return -ENODEV;

	/* set level and max in panel struct */
	mutex_lock(&dev_priv->backlight_lock);
	ret = panel->backlight.funcs->setup(connector, pipe);
	mutex_unlock(&dev_priv->backlight_lock);

	if (ret) {
		drm_dbg_kms(&dev_priv->drm,
			    "failed to setup backlight for connector %s\n",
			    connector->base.name);
		return ret;
	}

	panel->backlight.present = true;

	drm_dbg_kms(&dev_priv->drm,
		    "Connector %s backlight initialized, %s, brightness %u/%u\n",
		    connector->base.name,
		    str_enabled_disabled(panel->backlight.enabled),
		    panel->backlight.level, panel->backlight.max);

	return 0;
}

void intel_backlight_destroy(struct intel_panel *panel)
{
	/* dispose of the pwm */
	if (panel->backlight.pwm)
		pwm_put(panel->backlight.pwm);

	panel->backlight.present = false;
}

static const struct intel_panel_bl_funcs cnp_pwm_funcs = {
	.setup = cnp_setup_backlight,
	.enable = cnp_enable_backlight,
	.disable = cnp_disable_backlight,
	.set = bxt_set_backlight,
	.get = bxt_get_backlight,
	.hz_to_pwm = cnp_hz_to_pwm,
};

static const struct intel_panel_bl_funcs pwm_bl_funcs = {
	.setup = intel_pwm_setup_backlight,
	.enable = intel_pwm_enable_backlight,
	.disable = intel_pwm_disable_backlight,
	.set = intel_pwm_set_backlight,
	.get = intel_pwm_get_backlight,
};

/* Set up chip specific backlight functions */
void intel_backlight_init_funcs(struct intel_panel *panel)
{
	struct intel_connector *connector =
		container_of(panel, struct intel_connector, panel);
#ifndef BPM_DRM_LUMINANCE_RANGE_INFO_NOT_PRESENT
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
#endif

	if (connector->base.connector_type == DRM_MODE_CONNECTOR_DSI &&
	    intel_dsi_dcs_init_backlight_funcs(connector) == 0)
		return;

	panel->backlight.pwm_funcs = &cnp_pwm_funcs;

#ifdef BPM_DRM_LUMINANCE_RANGE_INFO_NOT_PRESENT
	if (connector->base.connector_type == DRM_MODE_CONNECTOR_eDP &&
	    intel_dp_aux_init_backlight_funcs(connector) == 0)
		return;
#else
	if (connector->base.connector_type == DRM_MODE_CONNECTOR_eDP) {
		if (intel_dp_aux_init_backlight_funcs(connector) == 0)
			return;

		if (!(dev_priv->quirks & QUIRK_NO_PPS_BACKLIGHT_POWER_HOOK))
			connector->panel.backlight.power = intel_pps_backlight_power;
	}
#endif

	/* We're using a standard PWM backlight interface */
	panel->backlight.funcs = &pwm_bl_funcs;
}
