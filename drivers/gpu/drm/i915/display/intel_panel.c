/*
 * Copyright © 2006-2010 Intel Corporation
 * Copyright (c) 2006 Dave Airlie <airlied@linux.ie>
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
 *      Dave Airlie <airlied@linux.ie>
 *      Jesse Barnes <jesse.barnes@intel.com>
 *      Chris Wilson <chris@chris-wilson.co.uk>
 */

#include <linux/kernel.h>
#include <linux/pwm.h>

#include "intel_backlight.h"
#include "intel_connector.h"
#include "intel_de.h"
#include "intel_display_types.h"
#include "intel_drrs.h"
#include "intel_panel.h"

bool intel_panel_use_ssc(struct drm_i915_private *i915)
{
	if (i915->params.panel_use_ssc >= 0)
		return i915->params.panel_use_ssc != 0;
	return i915->vbt.lvds_use_ssc
		&& !(i915->quirks & QUIRK_LVDS_SSC_DISABLE);
}

const struct drm_display_mode *
intel_panel_preferred_fixed_mode(struct intel_connector *connector)
{
	return list_first_entry_or_null(&connector->panel.fixed_modes,
					struct drm_display_mode, head);
}

const struct drm_display_mode *
intel_panel_fixed_mode(struct intel_connector *connector,
		       const struct drm_display_mode *mode)
{
	const struct drm_display_mode *fixed_mode, *best_mode = NULL;
	int vrefresh = drm_mode_vrefresh(mode);

	/* pick the fixed_mode that is closest in terms of vrefresh */
	list_for_each_entry(fixed_mode, &connector->panel.fixed_modes, head) {
		if (!best_mode ||
		    abs(drm_mode_vrefresh(fixed_mode) - vrefresh) <
		    abs(drm_mode_vrefresh(best_mode) - vrefresh))
			best_mode = fixed_mode;
	}

	return best_mode;
}

static bool is_alt_drrs_mode(const struct drm_display_mode *mode,
			     const struct drm_display_mode *preferred_mode)
{
	return drm_mode_match(mode, preferred_mode,
			      DRM_MODE_MATCH_TIMINGS |
			      DRM_MODE_MATCH_FLAGS |
			      DRM_MODE_MATCH_3D_FLAGS) &&
		mode->clock != preferred_mode->clock;
}

static bool is_alt_fixed_mode(const struct drm_display_mode *mode,
			      const struct drm_display_mode *preferred_mode)
{
	return drm_mode_match(mode, preferred_mode,
			      DRM_MODE_MATCH_FLAGS |
			      DRM_MODE_MATCH_3D_FLAGS) &&
		mode->hdisplay == preferred_mode->hdisplay &&
		mode->vdisplay == preferred_mode->vdisplay;
}

const struct drm_display_mode *
intel_panel_downclock_mode(struct intel_connector *connector,
			   const struct drm_display_mode *adjusted_mode)
{
	const struct drm_display_mode *fixed_mode, *best_mode = NULL;
	int min_vrefresh = connector->panel.vbt.seamless_drrs_min_refresh_rate;
	int max_vrefresh = drm_mode_vrefresh(adjusted_mode);

	/* pick the fixed_mode with the lowest refresh rate */
	list_for_each_entry(fixed_mode, &connector->panel.fixed_modes, head) {
		int vrefresh = drm_mode_vrefresh(fixed_mode);

		if (is_alt_drrs_mode(fixed_mode, adjusted_mode) &&
		    vrefresh >= min_vrefresh && vrefresh < max_vrefresh) {
			max_vrefresh = vrefresh;
			best_mode = fixed_mode;
		}
	}

	return best_mode;
}

int intel_panel_get_modes(struct intel_connector *connector)
{
	const struct drm_display_mode *fixed_mode;
	int num_modes = 0;

	list_for_each_entry(fixed_mode, &connector->panel.fixed_modes, head) {
		struct drm_display_mode *mode;

		mode = drm_mode_duplicate(connector->base.dev, fixed_mode);
		if (mode) {
			drm_mode_probed_add(&connector->base, mode);
			num_modes++;
		}
	}

	return num_modes;
}

enum drrs_type intel_panel_drrs_type(struct intel_connector *connector)
{
	if (list_empty(&connector->panel.fixed_modes) ||
	    list_is_singular(&connector->panel.fixed_modes))
		return DRRS_TYPE_NONE;

	return connector->panel.vbt.drrs_type;
}

int intel_panel_compute_config(struct intel_connector *connector,
			       struct drm_display_mode *adjusted_mode)
{
	const struct drm_display_mode *fixed_mode =
		intel_panel_fixed_mode(connector, adjusted_mode);

	if (!fixed_mode)
		return 0;

	/*
	 * We don't want to lie too much to the user about the refresh
	 * rate they're going to get. But we have to allow a bit of latitude
	 * for Xorg since it likes to automagically cook up modes with slightly
	 * off refresh rates.
	 */
	if (abs(drm_mode_vrefresh(adjusted_mode) - drm_mode_vrefresh(fixed_mode)) > 1) {
		drm_dbg_kms(connector->base.dev,
			    "[CONNECTOR:%d:%s] Requested mode vrefresh (%d Hz) does not match fixed mode vrefresh (%d Hz)\n",
			    connector->base.base.id, connector->base.name,
			    drm_mode_vrefresh(adjusted_mode), drm_mode_vrefresh(fixed_mode));

		return -EINVAL;
	}

	drm_mode_copy(adjusted_mode, fixed_mode);

	drm_mode_set_crtcinfo(adjusted_mode, 0);

	return 0;
}

static void intel_panel_add_edid_alt_fixed_modes(struct intel_connector *connector)
{
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	const struct drm_display_mode *preferred_mode =
		intel_panel_preferred_fixed_mode(connector);
	struct drm_display_mode *mode, *next;

	list_for_each_entry_safe(mode, next, &connector->base.probed_modes, head) {
		if (!is_alt_fixed_mode(mode, preferred_mode))
			continue;

		drm_dbg_kms(&dev_priv->drm,
			    "[CONNECTOR:%d:%s] using alternate EDID fixed mode: " DRM_MODE_FMT "\n",
			    connector->base.base.id, connector->base.name,
			    DRM_MODE_ARG(mode));

		list_move_tail(&mode->head, &connector->panel.fixed_modes);
	}
}

static void intel_panel_add_edid_preferred_mode(struct intel_connector *connector)
{
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct drm_display_mode *scan, *fixed_mode = NULL;

	if (list_empty(&connector->base.probed_modes))
		return;

	/* make sure the preferred mode is first */
	list_for_each_entry(scan, &connector->base.probed_modes, head) {
		if (scan->type & DRM_MODE_TYPE_PREFERRED) {
			fixed_mode = scan;
			break;
		}
	}

	if (!fixed_mode)
		fixed_mode = list_first_entry(&connector->base.probed_modes,
					      typeof(*fixed_mode), head);

	drm_dbg_kms(&dev_priv->drm,
		    "[CONNECTOR:%d:%s] using %s EDID fixed mode: " DRM_MODE_FMT "\n",
		    connector->base.base.id, connector->base.name,
		    fixed_mode->type & DRM_MODE_TYPE_PREFERRED ? "preferred" : "first",
		    DRM_MODE_ARG(fixed_mode));

	fixed_mode->type |= DRM_MODE_TYPE_PREFERRED;

	list_move_tail(&fixed_mode->head, &connector->panel.fixed_modes);
}

static void intel_panel_destroy_probed_modes(struct intel_connector *connector)
{
	struct drm_i915_private *i915 = to_i915(connector->base.dev);
	struct drm_display_mode *mode, *next;

	list_for_each_entry_safe(mode, next, &connector->base.probed_modes, head) {
		drm_dbg_kms(&i915->drm,
			    "[CONNECTOR:%d:%s] not using EDID mode: " DRM_MODE_FMT "\n",
			    connector->base.base.id, connector->base.name,
			    DRM_MODE_ARG(mode));
		list_del(&mode->head);
		drm_mode_destroy(&i915->drm, mode);
	}
}

void intel_panel_add_edid_fixed_modes(struct intel_connector *connector,
				      bool has_drrs, bool has_vrr)
{
	intel_panel_add_edid_preferred_mode(connector);
	if (intel_panel_preferred_fixed_mode(connector) && (has_drrs || has_vrr))
		intel_panel_add_edid_alt_fixed_modes(connector);
	intel_panel_destroy_probed_modes(connector);
}

static void intel_panel_add_fixed_mode(struct intel_connector *connector,
				       struct drm_display_mode *fixed_mode,
				       const char *type)
{
	struct drm_i915_private *i915 = to_i915(connector->base.dev);
	struct drm_display_info *info = &connector->base.display_info;

	if (!fixed_mode)
		return;

	fixed_mode->type |= DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;

	info->width_mm = fixed_mode->width_mm;
	info->height_mm = fixed_mode->height_mm;

	drm_dbg_kms(&i915->drm, "[CONNECTOR:%d:%s] using %s fixed mode: " DRM_MODE_FMT "\n",
		    connector->base.base.id, connector->base.name, type,
		    DRM_MODE_ARG(fixed_mode));

	list_add_tail(&fixed_mode->head, &connector->panel.fixed_modes);
}

void intel_panel_add_vbt_lfp_fixed_mode(struct intel_connector *connector)
{
	struct drm_i915_private *i915 = to_i915(connector->base.dev);
	const struct drm_display_mode *mode;

	mode = connector->panel.vbt.lfp_lvds_vbt_mode;
	if (!mode)
		return;

	intel_panel_add_fixed_mode(connector,
				   drm_mode_duplicate(&i915->drm, mode),
				   "VBT LFP");
}

void intel_panel_add_vbt_sdvo_fixed_mode(struct intel_connector *connector)
{
	struct drm_i915_private *i915 = to_i915(connector->base.dev);
	const struct drm_display_mode *mode;

	mode = connector->panel.vbt.sdvo_lvds_vbt_mode;
	if (!mode)
		return;

	intel_panel_add_fixed_mode(connector,
				   drm_mode_duplicate(&i915->drm, mode),
				   "VBT SDVO");
}

void intel_panel_add_encoder_fixed_mode(struct intel_connector *connector,
					struct intel_encoder *encoder)
{
	intel_panel_add_fixed_mode(connector,
				   intel_encoder_current_mode(encoder),
				   "current (BIOS)");
}

/* adjusted_mode has been preset to be the panel's fixed mode */
static int pch_panel_fitting(struct intel_crtc_state *crtc_state,
			     const struct drm_connector_state *conn_state)
{
	const struct drm_display_mode *adjusted_mode =
		&crtc_state->hw.adjusted_mode;
	int pipe_src_w = drm_rect_width(&crtc_state->pipe_src);
	int pipe_src_h = drm_rect_height(&crtc_state->pipe_src);
	int x, y, width, height;

	/* Native modes don't need fitting */
	if (adjusted_mode->crtc_hdisplay == pipe_src_w &&
	    adjusted_mode->crtc_vdisplay == pipe_src_h &&
	    crtc_state->output_format != INTEL_OUTPUT_FORMAT_YCBCR420)
		return 0;

	switch (conn_state->scaling_mode) {
	case DRM_MODE_SCALE_CENTER:
		width = pipe_src_w;
		height = pipe_src_h;
		x = (adjusted_mode->crtc_hdisplay - width + 1)/2;
		y = (adjusted_mode->crtc_vdisplay - height + 1)/2;
		break;

	case DRM_MODE_SCALE_ASPECT:
		/* Scale but preserve the aspect ratio */
		{
			u32 scaled_width = adjusted_mode->crtc_hdisplay * pipe_src_h;
			u32 scaled_height = pipe_src_w * adjusted_mode->crtc_vdisplay;
			if (scaled_width > scaled_height) { /* pillar */
				width = scaled_height / pipe_src_h;
				if (width & 1)
					width++;
				x = (adjusted_mode->crtc_hdisplay - width + 1) / 2;
				y = 0;
				height = adjusted_mode->crtc_vdisplay;
			} else if (scaled_width < scaled_height) { /* letter */
				height = scaled_width / pipe_src_w;
				if (height & 1)
				    height++;
				y = (adjusted_mode->crtc_vdisplay - height + 1) / 2;
				x = 0;
				width = adjusted_mode->crtc_hdisplay;
			} else {
				x = y = 0;
				width = adjusted_mode->crtc_hdisplay;
				height = adjusted_mode->crtc_vdisplay;
			}
		}
		break;

	case DRM_MODE_SCALE_NONE:
		WARN_ON(adjusted_mode->crtc_hdisplay != pipe_src_w);
		WARN_ON(adjusted_mode->crtc_vdisplay != pipe_src_h);
		fallthrough;
	case DRM_MODE_SCALE_FULLSCREEN:
		x = y = 0;
		width = adjusted_mode->crtc_hdisplay;
		height = adjusted_mode->crtc_vdisplay;
		break;

	default:
		MISSING_CASE(conn_state->scaling_mode);
		return -EINVAL;
	}

	drm_rect_init(&crtc_state->pch_pfit.dst,
		      x, y, width, height);
	crtc_state->pch_pfit.enabled = true;

	return 0;
}

int intel_panel_fitting(struct intel_crtc_state *crtc_state,
			const struct drm_connector_state *conn_state)
{
	return pch_panel_fitting(crtc_state, conn_state);
}

enum drm_connector_status
intel_panel_detect(struct drm_connector *connector, bool force)
{
	struct drm_i915_private *i915 = to_i915(connector->dev);

	if (!INTEL_DISPLAY_ENABLED(i915))
		return connector_status_disconnected;

	return connector_status_connected;
}

enum drm_mode_status
intel_panel_mode_valid(struct intel_connector *connector,
		       const struct drm_display_mode *mode)
{
	const struct drm_display_mode *fixed_mode =
		intel_panel_fixed_mode(connector, mode);

	if (!fixed_mode)
		return MODE_OK;

	if (mode->hdisplay != fixed_mode->hdisplay)
		return MODE_PANEL;

	if (mode->vdisplay != fixed_mode->vdisplay)
		return MODE_PANEL;

	if (drm_mode_vrefresh(mode) != drm_mode_vrefresh(fixed_mode))
		return MODE_PANEL;

	return MODE_OK;
}

int intel_panel_init(struct intel_connector *connector)
{
	struct intel_panel *panel = &connector->panel;

	intel_backlight_init_funcs(panel);

	drm_dbg_kms(connector->base.dev,
		    "[CONNECTOR:%d:%s] DRRS type: %s\n",
		    connector->base.base.id, connector->base.name,
		    intel_drrs_type_str(intel_panel_drrs_type(connector)));

	return 0;
}

void intel_panel_fini(struct intel_connector *connector)
{
	struct intel_panel *panel = &connector->panel;
	struct drm_display_mode *fixed_mode, *next;

	intel_backlight_destroy(panel);

	intel_bios_fini_panel(panel);

	list_for_each_entry_safe(fixed_mode, next, &panel->fixed_modes, head) {
		list_del(&fixed_mode->head);
		drm_mode_destroy(connector->base.dev, fixed_mode);
	}
}
