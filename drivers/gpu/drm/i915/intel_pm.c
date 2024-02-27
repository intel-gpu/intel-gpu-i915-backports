/*
 * Copyright © 2012 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Eugeni Dodonov <eugeni.dodonov@intel.com>
 *
 */

#include <linux/module.h>
#include <linux/string_helpers.h>
#include <linux/pm_runtime.h>
#include <linux/bitops.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_plane_helper.h>

#include "display/intel_atomic.h"
#include "display/intel_atomic_plane.h"
#include "display/intel_bw.h"
#include "display/intel_cdclk.h"
#include "display/intel_de.h"
#include "display/intel_display_trace.h"
#include "display/intel_display_types.h"
#include "display/intel_fb.h"
#include "display/intel_fbc.h"
#include "display/intel_sprite.h"
#include "display/skl_universal_plane.h"
#include "display/intel_cx0_phy.h"

#include "gt/intel_engine_regs.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_mcr.h"
#include "gt/intel_gt_regs.h"
#include "gt/intel_llc.h"

#include "i915_drv.h"
#include "i915_fixed.h"
#include "i915_irq.h"
#include "intel_mchbar_regs.h"
#include "intel_pcode.h"
#include "intel_pm.h"

struct drm_i915_clock_gating_funcs {
	void (*init_clock_gating)(struct drm_i915_private *i915);
};

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
/* Stores plane specific WM parameters */
struct skl_wm_params {
	bool x_tiled, y_tiled;
	bool rc_surface;
	bool is_planar;
	u32 width;
	u8 cpp;
	u32 plane_pixel_rate;
	u32 y_min_scanlines;
	u32 plane_bytes_per_line;
	uint_fixed_16_16_t plane_blocks_per_line;
	uint_fixed_16_16_t y_tile_minimum;
	u32 linetime_us;
	u32 dbuf_block_size;
};
#endif

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
#define FW_WM(value, plane) \
	(((value) << DSPFW_ ## plane ## _SHIFT) & DSPFW_ ## plane ## _MASK)

static bool intel_wm_plane_visible(const struct intel_crtc_state *crtc_state,
				   const struct intel_plane_state *plane_state)
{
	struct intel_plane *plane = to_intel_plane(plane_state->uapi.plane);

	/* FIXME check the 'enable' instead */
	if (!crtc_state->hw.active)
		return false;

	/*
	 * Treat cursor with fb as always visible since cursor updates
	 * can happen faster than the vrefresh rate, and the current
	 * watermark code doesn't handle that correctly. Cursor updates
	 * which set/clear the fb or change the cursor size are going
	 * to get throttled by intel_legacy_cursor_update() to work
	 * around this problem with the watermark code.
	 */
	if (plane->id == PLANE_CURSOR)
		return plane_state->hw.fb != NULL;
	else
		return plane_state->uapi.visible;
}

static void
adjust_wm_latency(struct drm_i915_private *i915,
		  u16 wm[], int max_level, int read_latency)
{
	bool wm_lv_0_adjust_needed = i915->dram_info.wm_lv_0_adjust_needed;
	int i, level;

	/*
	 * If a level n (n > 1) has a 0us latency, all levels m (m >= n)
	 * need to be disabled. We make sure to sanitize the values out
	 * of the punit to satisfy this requirement.
	 */
	for (level = 1; level <= max_level; level++) {
		if (wm[level] == 0) {
			for (i = level + 1; i <= max_level; i++)
				wm[i] = 0;

			max_level = level - 1;
			break;
		}
	}

	/*
	 * WaWmMemoryReadLatency
	 *
	 * punit doesn't take into account the read latency so we need
	 * to add proper adjustement to each valid level we retrieve
	 * from the punit when level 0 response data is 0us.
	 */
	if (wm[0] == 0) {
		for (level = 0; level <= max_level; level++)
			wm[level] += read_latency;
	}

	/*
	 * WA Level-0 adjustment for 16GB DIMMs: SKL+
	 * If we could not get dimm info enable this WA to prevent from
	 * any underrun. If not able to get Dimm info assume 16GB dimm
	 * to avoid any underrun.
	 */
	if (wm_lv_0_adjust_needed)
		wm[0] += 1;
}

static void intel_read_wm_latency(struct drm_i915_private *dev_priv,
				  u16 wm[])
{
	struct intel_uncore *uncore = &dev_priv->uncore;
	int max_level = ilk_wm_max_level(dev_priv);

	if (DISPLAY_VER(dev_priv) >= 14) {
		u32 val;

		val = intel_uncore_read(uncore, MTL_LATENCY_LP0_LP1);
		wm[0] = REG_FIELD_GET(MTL_LATENCY_LEVEL_EVEN_MASK, val);
		wm[1] = REG_FIELD_GET(MTL_LATENCY_LEVEL_ODD_MASK, val);
		val = intel_uncore_read(uncore, MTL_LATENCY_LP2_LP3);
		wm[2] = REG_FIELD_GET(MTL_LATENCY_LEVEL_EVEN_MASK, val);
		wm[3] = REG_FIELD_GET(MTL_LATENCY_LEVEL_ODD_MASK, val);
		val = intel_uncore_read(uncore, MTL_LATENCY_LP4_LP5);
		wm[4] = REG_FIELD_GET(MTL_LATENCY_LEVEL_EVEN_MASK, val);
		wm[5] = REG_FIELD_GET(MTL_LATENCY_LEVEL_ODD_MASK, val);

		adjust_wm_latency(dev_priv, wm, max_level, 6);
	} else {
		int read_latency = DISPLAY_VER(dev_priv) >= 12 ? 3 : 2;
		int mult = IS_DG2(dev_priv) ? 2 : 1;
		u32 val;
		int ret;

		/* read the first set of memory latencies[0:3] */
		val = 0; /* data0 to be programmed to 0 for first set */
		ret = snb_pcode_read(&dev_priv->uncore, GEN9_PCODE_READ_MEM_LATENCY,
				     &val, NULL);

		if (ret) {
			drm_err(&dev_priv->drm,
				"SKL Mailbox read error = %d\n", ret);
			return;
		}

		wm[0] = (val & GEN9_MEM_LATENCY_LEVEL_MASK) * mult;
		wm[1] = ((val >> GEN9_MEM_LATENCY_LEVEL_1_5_SHIFT) &
				GEN9_MEM_LATENCY_LEVEL_MASK) * mult;
		wm[2] = ((val >> GEN9_MEM_LATENCY_LEVEL_2_6_SHIFT) &
				GEN9_MEM_LATENCY_LEVEL_MASK) * mult;
		wm[3] = ((val >> GEN9_MEM_LATENCY_LEVEL_3_7_SHIFT) &
				GEN9_MEM_LATENCY_LEVEL_MASK) * mult;

		/* read the second set of memory latencies[4:7] */
		val = 1; /* data0 to be programmed to 1 for second set */
		ret = snb_pcode_read(&dev_priv->uncore, GEN9_PCODE_READ_MEM_LATENCY,
				     &val, NULL);
		if (ret) {
			drm_err(&dev_priv->drm,
				"SKL Mailbox read error = %d\n", ret);
			return;
		}

		wm[4] = (val & GEN9_MEM_LATENCY_LEVEL_MASK) * mult;
		wm[5] = ((val >> GEN9_MEM_LATENCY_LEVEL_1_5_SHIFT) &
				GEN9_MEM_LATENCY_LEVEL_MASK) * mult;
		wm[6] = ((val >> GEN9_MEM_LATENCY_LEVEL_2_6_SHIFT) &
				GEN9_MEM_LATENCY_LEVEL_MASK) * mult;
		wm[7] = ((val >> GEN9_MEM_LATENCY_LEVEL_3_7_SHIFT) &
				GEN9_MEM_LATENCY_LEVEL_MASK) * mult;

		adjust_wm_latency(dev_priv, wm, max_level, read_latency);
	}
}

int ilk_wm_max_level(const struct drm_i915_private *dev_priv)
{
	/* how many WM levels are we expecting */
	if (HAS_HW_SAGV_WM(dev_priv))
		return 5;
	else
		return 7;
}

static void intel_print_wm_latency(struct drm_i915_private *dev_priv,
				   const char *name,
				   const u16 wm[])
{
	int level, max_level = ilk_wm_max_level(dev_priv);

	for (level = 0; level <= max_level; level++) {
		unsigned int latency = wm[level];

		if (latency == 0) {
			drm_dbg_kms(&dev_priv->drm,
				    "%s WM%d latency not provided\n",
				    name, level);
			continue;
		}

		/*
		 * - latencies are in us on gen9.
		 * - before then, WM1+ latency values are in 0.5us units
		 */
		latency *= 10;

		drm_dbg_kms(&dev_priv->drm,
			    "%s WM%d latency %u (%u.%u usec)\n", name, level,
			    wm[level], latency / 10, latency % 10);
	}
}

static void skl_setup_wm_latency(struct drm_i915_private *dev_priv)
{
	intel_read_wm_latency(dev_priv, dev_priv->wm.skl_latency);
	intel_print_wm_latency(dev_priv, "Gen9 Plane", dev_priv->wm.skl_latency);
}

/* dirty bits used to track which watermarks need changes */
#define WM_DIRTY_PIPE(pipe) (1 << (pipe))
#define WM_DIRTY_LP(wm_lp) (1 << (15 + (wm_lp)))
#define WM_DIRTY_LP_ALL (WM_DIRTY_LP(1) | WM_DIRTY_LP(2) | WM_DIRTY_LP(3))
#define WM_DIRTY_FBC (1 << 24)
#define WM_DIRTY_DDB (1 << 25)

static bool _ilk_disable_lp_wm(struct drm_i915_private *dev_priv,
			       unsigned int dirty)
{
	struct ilk_wm_values *previous = &dev_priv->wm.hw;
	bool changed = false;

	if (dirty & WM_DIRTY_LP(3) && previous->wm_lp[2] & WM_LP_ENABLE) {
		previous->wm_lp[2] &= ~WM_LP_ENABLE;
		intel_uncore_write(&dev_priv->uncore, WM3_LP_ILK, previous->wm_lp[2]);
		changed = true;
	}
	if (dirty & WM_DIRTY_LP(2) && previous->wm_lp[1] & WM_LP_ENABLE) {
		previous->wm_lp[1] &= ~WM_LP_ENABLE;
		intel_uncore_write(&dev_priv->uncore, WM2_LP_ILK, previous->wm_lp[1]);
		changed = true;
	}
	if (dirty & WM_DIRTY_LP(1) && previous->wm_lp[0] & WM_LP_ENABLE) {
		previous->wm_lp[0] &= ~WM_LP_ENABLE;
		intel_uncore_write(&dev_priv->uncore, WM1_LP_ILK, previous->wm_lp[0]);
		changed = true;
	}

	/*
	 * Don't touch WM_LP_SPRITE_ENABLE here.
	 * Doing so could cause underruns.
	 */

	return changed;
}

bool ilk_disable_lp_wm(struct drm_i915_private *dev_priv)
{
	return _ilk_disable_lp_wm(dev_priv, WM_DIRTY_LP_ALL);
}

u8 intel_enabled_dbuf_slices_mask(struct drm_i915_private *dev_priv)
{
	u8 enabled_slices = 0;
	enum dbuf_slice slice;

	for_each_dbuf_slice(dev_priv, slice) {
		if (intel_uncore_read(&dev_priv->uncore,
				      DBUF_CTL_S(slice)) & DBUF_POWER_STATE)
			enabled_slices |= BIT(slice);
	}

	return enabled_slices;
}
/*
 * FIXME: We still don't have the proper code detect if we need to apply the WA,
 * so assume we'll always need it in order to avoid underruns.
 */

static bool skl_needs_memory_bw_wa(struct drm_i915_private *dev_priv)
{
	return false;
}
#endif

static bool
intel_has_sagv(struct drm_i915_private *dev_priv)
{
	return !IS_LP(dev_priv) && dev_priv->sagv_status != I915_SAGV_NOT_CONTROLLED;
}

static u32
intel_sagv_block_time(struct drm_i915_private *dev_priv)
{
	if (DISPLAY_VER(dev_priv) >= 14) {
		u32 val;

		val = intel_uncore_read(&dev_priv->uncore, MTL_LATENCY_SAGV);

		return REG_FIELD_GET(MTL_LATENCY_QCLK_SAGV, val);
	} else {
		u32 val = 0;
		int ret;

		ret = snb_pcode_read(&dev_priv->uncore,
				     GEN12_PCODE_READ_SAGV_BLOCK_TIME_US,
				     &val, NULL);
		if (ret) {
			drm_dbg_kms(&dev_priv->drm, "Couldn't read SAGV block time!\n");
			return 0;
		}

		return val;
	}
}

static void intel_sagv_init(struct drm_i915_private *i915)
{
	if (!intel_has_sagv(i915))
		i915->sagv_status = I915_SAGV_NOT_CONTROLLED;

	drm_WARN_ON(&i915->drm, i915->sagv_status == I915_SAGV_UNKNOWN);

	i915->sagv_block_time_us = intel_sagv_block_time(i915);

	drm_dbg_kms(&i915->drm, "SAGV supported: %s, original SAGV block time: %u us\n",
		    str_yes_no(intel_has_sagv(i915)), i915->sagv_block_time_us);

	/* avoid overflow when adding with wm0 latency/etc. */
	if (drm_WARN(&i915->drm, i915->sagv_block_time_us > U16_MAX,
		     "Excessive SAGV block time %u, ignoring\n",
		     i915->sagv_block_time_us))
		i915->sagv_block_time_us = 0;

	if (!intel_has_sagv(i915))
		i915->sagv_block_time_us = 0;
}

/*
 * SAGV dynamically adjusts the system agent voltage and clock frequencies
 * depending on power and performance requirements. The display engine access
 * to system memory is blocked during the adjustment time. Because of the
 * blocking time, having this enabled can cause full system hangs and/or pipe
 * underruns if we don't meet all of the following requirements:
 *
 *  - <= 1 pipe enabled
 *  - All planes can enable watermarks for latencies >= SAGV engine block time
 *  - We're not using an interlaced display configuration
 */
#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
static void icl_sagv_pre_plane_update(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	const struct intel_bw_state *old_bw_state =
		intel_atomic_get_old_bw_state(state);
	const struct intel_bw_state *new_bw_state =
		intel_atomic_get_new_bw_state(state);
	u16 old_mask, new_mask;

	if (!new_bw_state)
		return;

	old_mask = old_bw_state->qgv_points_mask;
	new_mask = old_bw_state->qgv_points_mask | new_bw_state->qgv_points_mask;

	if (old_mask == new_mask)
		return;

	WARN_ON(!new_bw_state->base.changed);

	drm_dbg_kms(&dev_priv->drm, "Restricting QGV points: 0x%x -> 0x%x\n",
		    old_mask, new_mask);

	/*
	 * Restrict required qgv points before updating the configuration.
	 * According to BSpec we can't mask and unmask qgv points at the same
	 * time. Also masking should be done before updating the configuration
	 * and unmasking afterwards.
	 */
	icl_pcode_restrict_qgv_points(dev_priv, new_mask);
}

static void icl_sagv_post_plane_update(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	const struct intel_bw_state *old_bw_state =
		intel_atomic_get_old_bw_state(state);
	const struct intel_bw_state *new_bw_state =
		intel_atomic_get_new_bw_state(state);
	u16 old_mask, new_mask;

	if (!new_bw_state)
		return;

	old_mask = old_bw_state->qgv_points_mask | new_bw_state->qgv_points_mask;
	new_mask = new_bw_state->qgv_points_mask;

	if (old_mask == new_mask)
		return;

	WARN_ON(!new_bw_state->base.changed);

	drm_dbg_kms(&dev_priv->drm, "Relaxing QGV points: 0x%x -> 0x%x\n",
		    old_mask, new_mask);

	/*
	 * Allow required qgv points after updating the configuration.
	 * According to BSpec we can't mask and unmask qgv points at the same
	 * time. Also masking should be done before updating the configuration
	 * and unmasking afterwards.
	 */
	icl_pcode_restrict_qgv_points(dev_priv, new_mask);
}

void intel_sagv_pre_plane_update(struct intel_atomic_state *state)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);

	/*
	 * No need to update mask value/restrict because
	 * "Pcode only wants to use GV bandwidth value, not the mask value."
	 * for DISPLAY_VER() >= 14.
	 */
	if (DISPLAY_VER(i915) >= 14)
		return;

	/*
	 * Just return if we can't control SAGV or don't have it.
	 * This is different from situation when we have SAGV but just can't
	 * afford it due to DBuf limitation - in case if SAGV is completely
	 * disabled in a BIOS, we are not even allowed to send a PCode request,
	 * as it will throw an error. So have to check it here.
	 */
	if (!intel_has_sagv(i915))
		return;

	icl_sagv_pre_plane_update(state);
}

void intel_sagv_post_plane_update(struct intel_atomic_state *state)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);

	/*
	 * No need to update mask value/restrict because
	 * "Pcode only wants to use GV bandwidth value, not the mask value."
	 * for DISPLAY_VER() >= 14.
	 *
	 * GV bandwidth will be set by intel_pmdemand_post_plane_update()
	 */
	if (DISPLAY_VER(i915) >= 14)
		return;

	/*
	 * Just return if we can't control SAGV or don't have it.
	 * This is different from situation when we have SAGV but just can't
	 * afford it due to DBuf limitation - in case if SAGV is completely
	 * disabled in a BIOS, we are not even allowed to send a PCode request,
	 * as it will throw an error. So have to check it here.
	 */
	if (!intel_has_sagv(i915))
		return;

	icl_sagv_post_plane_update(state);
}

static bool tgl_crtc_can_enable_sagv(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	enum plane_id plane_id;

	if (!crtc_state->hw.active)
		return true;

	for_each_plane_id_on_crtc(crtc, plane_id) {
		const struct skl_plane_wm *wm =
			&crtc_state->wm.skl.optimal.planes[plane_id];

		if (wm->wm[0].enable && !wm->sagv.wm0.enable)
			return false;
	}

	return true;
}

static bool intel_crtc_can_enable_sagv(const struct intel_crtc_state *crtc_state)
{
	return tgl_crtc_can_enable_sagv(crtc_state);
}

bool intel_can_enable_sagv(struct drm_i915_private *dev_priv,
			   const struct intel_bw_state *bw_state)
{
	return bw_state->pipe_sagv_reject == 0;
}

static int intel_compute_sagv_mask(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	int ret;
	struct intel_crtc *crtc;
	struct intel_crtc_state *new_crtc_state;
	struct intel_bw_state *new_bw_state = NULL;
	const struct intel_bw_state *old_bw_state = NULL;
	int i;

	for_each_new_intel_crtc_in_state(state, crtc,
					 new_crtc_state, i) {
		new_bw_state = intel_atomic_get_bw_state(state);
		if (IS_ERR(new_bw_state))
			return PTR_ERR(new_bw_state);

		old_bw_state = intel_atomic_get_old_bw_state(state);

		if (intel_crtc_can_enable_sagv(new_crtc_state))
			new_bw_state->pipe_sagv_reject &= ~BIT(crtc->pipe);
		else
			new_bw_state->pipe_sagv_reject |= BIT(crtc->pipe);
	}

	if (!new_bw_state)
		return 0;

	new_bw_state->active_pipes =
		intel_calc_active_pipes(state, old_bw_state->active_pipes);

	if (new_bw_state->active_pipes != old_bw_state->active_pipes) {
		ret = intel_atomic_lock_global_state(&new_bw_state->base);
		if (ret)
			return ret;
	}

	if (intel_can_enable_sagv(dev_priv, new_bw_state) !=
	    intel_can_enable_sagv(dev_priv, old_bw_state)) {
		ret = intel_atomic_serialize_global_state(&new_bw_state->base);
		if (ret)
			return ret;
	} else if (new_bw_state->pipe_sagv_reject != old_bw_state->pipe_sagv_reject) {
		ret = intel_atomic_lock_global_state(&new_bw_state->base);
		if (ret)
			return ret;
	}

	for_each_new_intel_crtc_in_state(state, crtc,
					 new_crtc_state, i) {
		struct skl_pipe_wm *pipe_wm = &new_crtc_state->wm.skl.optimal;

		/*
		 * We store use_sagv_wm in the crtc state rather than relying on
		 * that bw state since we have no convenient way to get at the
		 * latter from the plane commit hooks (especially in the legacy
		 * cursor case)
		 */
		pipe_wm->use_sagv_wm = !HAS_HW_SAGV_WM(dev_priv) &&
			intel_can_enable_sagv(dev_priv, new_bw_state);
	}

	return 0;
}

static u16 skl_ddb_entry_init(struct skl_ddb_entry *entry,
			      u16 start, u16 end)
{
	entry->start = start;
	entry->end = end;

	return end;
}

static int intel_dbuf_slice_size(struct drm_i915_private *dev_priv)
{
	return INTEL_INFO(dev_priv)->display.dbuf.size /
		hweight8(INTEL_INFO(dev_priv)->display.dbuf.slice_mask);
}

static void
skl_ddb_entry_for_slices(struct drm_i915_private *dev_priv, u8 slice_mask,
			 struct skl_ddb_entry *ddb)
{
	int slice_size = intel_dbuf_slice_size(dev_priv);

	if (!slice_mask) {
		ddb->start = 0;
		ddb->end = 0;
		return;
	}

	ddb->start = (ffs(slice_mask) - 1) * slice_size;
	ddb->end = fls(slice_mask) * slice_size;

	WARN_ON(ddb->start >= ddb->end);
	WARN_ON(ddb->end > INTEL_INFO(dev_priv)->display.dbuf.size);
}

static unsigned int mbus_ddb_offset(struct drm_i915_private *i915, u8 slice_mask)
{
	struct skl_ddb_entry ddb;

	if (slice_mask & (BIT(DBUF_S1) | BIT(DBUF_S2)))
		slice_mask = BIT(DBUF_S1);
	else if (slice_mask & (BIT(DBUF_S3) | BIT(DBUF_S4)))
		slice_mask = BIT(DBUF_S3);

	skl_ddb_entry_for_slices(i915, slice_mask, &ddb);

	return ddb.start;
}

u32 skl_ddb_dbuf_slice_mask(struct drm_i915_private *dev_priv,
			    const struct skl_ddb_entry *entry)
{
	int slice_size = intel_dbuf_slice_size(dev_priv);
	enum dbuf_slice start_slice, end_slice;
	u8 slice_mask = 0;

	if (!skl_ddb_entry_size(entry))
		return 0;

	start_slice = entry->start / slice_size;
	end_slice = (entry->end - 1) / slice_size;

	/*
	 * Per plane DDB entry can in a really worst case be on multiple slices
	 * but single entry is anyway contigious.
	 */
	while (start_slice <= end_slice) {
		slice_mask |= BIT(start_slice);
		start_slice++;
	}

	return slice_mask;
}

static unsigned int intel_crtc_ddb_weight(const struct intel_crtc_state *crtc_state)
{
	const struct drm_display_mode *pipe_mode = &crtc_state->hw.pipe_mode;
	int hdisplay, vdisplay;

	if (!crtc_state->hw.active)
		return 0;

	/*
	 * Watermark/ddb requirement highly depends upon width of the
	 * framebuffer, So instead of allocating DDB equally among pipes
	 * distribute DDB based on resolution/width of the display.
	 */
	drm_mode_get_hv_timing(pipe_mode, &hdisplay, &vdisplay);

	return hdisplay;
}

static void intel_crtc_dbuf_weights(const struct intel_dbuf_state *dbuf_state,
				    enum pipe for_pipe,
				    unsigned int *weight_start,
				    unsigned int *weight_end,
				    unsigned int *weight_total)
{
	struct drm_i915_private *dev_priv =
		to_i915(dbuf_state->base.state->base.dev);
	enum pipe pipe;

	*weight_start = 0;
	*weight_end = 0;
	*weight_total = 0;

	for_each_pipe(dev_priv, pipe) {
		int weight = dbuf_state->weight[pipe];

		/*
		 * Do not account pipes using other slice sets
		 * luckily as of current BSpec slice sets do not partially
		 * intersect(pipes share either same one slice or same slice set
		 * i.e no partial intersection), so it is enough to check for
		 * equality for now.
		 */
		if (dbuf_state->slices[pipe] != dbuf_state->slices[for_pipe])
			continue;

		*weight_total += weight;
		if (pipe < for_pipe) {
			*weight_start += weight;
			*weight_end += weight;
		} else if (pipe == for_pipe) {
			*weight_end += weight;
		}
	}
}

static int
skl_crtc_allocate_ddb(struct intel_atomic_state *state, struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	unsigned int weight_total, weight_start, weight_end;
	const struct intel_dbuf_state *old_dbuf_state =
		intel_atomic_get_old_dbuf_state(state);
	struct intel_dbuf_state *new_dbuf_state =
		intel_atomic_get_new_dbuf_state(state);
	struct intel_crtc_state *crtc_state;
	struct skl_ddb_entry ddb_slices;
	enum pipe pipe = crtc->pipe;
	unsigned int mbus_offset = 0;
	u32 ddb_range_size;
	u32 dbuf_slice_mask;
	u32 start, end;
	int ret;

	if (new_dbuf_state->weight[pipe] == 0) {
		skl_ddb_entry_init(&new_dbuf_state->ddb[pipe], 0, 0);
		goto out;
	}

	dbuf_slice_mask = new_dbuf_state->slices[pipe];

	skl_ddb_entry_for_slices(dev_priv, dbuf_slice_mask, &ddb_slices);
	mbus_offset = mbus_ddb_offset(dev_priv, dbuf_slice_mask);
	ddb_range_size = skl_ddb_entry_size(&ddb_slices);

	intel_crtc_dbuf_weights(new_dbuf_state, pipe,
				&weight_start, &weight_end, &weight_total);

	start = ddb_range_size * weight_start / weight_total;
	end = ddb_range_size * weight_end / weight_total;

	skl_ddb_entry_init(&new_dbuf_state->ddb[pipe],
			   ddb_slices.start - mbus_offset + start,
			   ddb_slices.start - mbus_offset + end);

out:
	if (old_dbuf_state->slices[pipe] == new_dbuf_state->slices[pipe] &&
	    skl_ddb_entry_equal(&old_dbuf_state->ddb[pipe],
				&new_dbuf_state->ddb[pipe]))
		return 0;

	ret = intel_atomic_lock_global_state(&new_dbuf_state->base);
	if (ret)
		return ret;

	crtc_state = intel_atomic_get_crtc_state(&state->base, crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	/*
	 * Used for checking overlaps, so we need absolute
	 * offsets instead of MBUS relative offsets.
	 */
	crtc_state->wm.skl.ddb.start = mbus_offset + new_dbuf_state->ddb[pipe].start;
	crtc_state->wm.skl.ddb.end = mbus_offset + new_dbuf_state->ddb[pipe].end;

	drm_dbg_kms(&dev_priv->drm,
		    "[CRTC:%d:%s] dbuf slices 0x%x -> 0x%x, ddb (%d - %d) -> (%d - %d), active pipes 0x%x -> 0x%x\n",
		    crtc->base.base.id, crtc->base.name,
		    old_dbuf_state->slices[pipe], new_dbuf_state->slices[pipe],
		    old_dbuf_state->ddb[pipe].start, old_dbuf_state->ddb[pipe].end,
		    new_dbuf_state->ddb[pipe].start, new_dbuf_state->ddb[pipe].end,
		    old_dbuf_state->active_pipes, new_dbuf_state->active_pipes);

	return 0;
}

static int skl_compute_wm_params(const struct intel_crtc_state *crtc_state,
				 int width, const struct drm_format_info *format,
				 u64 modifier, unsigned int rotation,
				 u32 plane_pixel_rate, struct skl_wm_params *wp,
				 int color_plane);

static void skl_compute_plane_wm(const struct intel_crtc_state *crtc_state,
				 struct intel_plane *plane,
				 int level,
				 unsigned int latency,
				 const struct skl_wm_params *wp,
				 const struct skl_wm_level *result_prev,
				 struct skl_wm_level *result /* out */);

static unsigned int
skl_cursor_allocation(const struct intel_crtc_state *crtc_state,
		      int num_active)
{
	struct intel_plane *plane = to_intel_plane(crtc_state->uapi.crtc->cursor);
	struct drm_i915_private *dev_priv = to_i915(crtc_state->uapi.crtc->dev);
	int level, max_level = ilk_wm_max_level(dev_priv);
	struct skl_wm_level wm = {};
	int ret, min_ddb_alloc = 0;
	struct skl_wm_params wp;

	ret = skl_compute_wm_params(crtc_state, 256,
				    drm_format_info(DRM_FORMAT_ARGB8888),
				    DRM_FORMAT_MOD_LINEAR,
				    DRM_MODE_ROTATE_0,
				    crtc_state->pixel_rate, &wp, 0);
	drm_WARN_ON(&dev_priv->drm, ret);

	for (level = 0; level <= max_level; level++) {
		unsigned int latency = dev_priv->wm.skl_latency[level];

		skl_compute_plane_wm(crtc_state, plane, level, latency, &wp, &wm, &wm);
		if (wm.min_ddb_alloc == U16_MAX)
			break;

		min_ddb_alloc = wm.min_ddb_alloc;
	}

	return max(num_active == 1 ? 32 : 8, min_ddb_alloc);
}

static void skl_ddb_entry_init_from_hw(struct skl_ddb_entry *entry, u32 reg)
{
	skl_ddb_entry_init(entry,
			   REG_FIELD_GET(PLANE_BUF_START_MASK, reg),
			   REG_FIELD_GET(PLANE_BUF_END_MASK, reg));
	if (entry->end)
		entry->end++;
}

static void
skl_ddb_get_hw_plane_state(struct drm_i915_private *dev_priv,
			   const enum pipe pipe,
			   const enum plane_id plane_id,
			   struct skl_ddb_entry *ddb,
			   struct skl_ddb_entry *ddb_y)
{
	u32 val;

	/* Cursor doesn't support NV12/planar, so no extra calculation needed */
	if (plane_id == PLANE_CURSOR) {
		val = intel_uncore_read(&dev_priv->uncore, CUR_BUF_CFG(pipe));
		skl_ddb_entry_init_from_hw(ddb, val);
		return;
	}

	val = intel_uncore_read(&dev_priv->uncore, PLANE_BUF_CFG(pipe, plane_id));
	skl_ddb_entry_init_from_hw(ddb, val);
}

static void skl_pipe_ddb_get_hw_state(struct intel_crtc *crtc,
				      struct skl_ddb_entry *ddb,
				      struct skl_ddb_entry *ddb_y)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum intel_display_power_domain power_domain;
	enum pipe pipe = crtc->pipe;
	intel_wakeref_t wakeref;
	enum plane_id plane_id;

	power_domain = POWER_DOMAIN_PIPE(pipe);
	wakeref = intel_display_power_get_if_enabled(dev_priv, power_domain);
	if (!wakeref)
		return;

	for_each_plane_id_on_crtc(crtc, plane_id)
		skl_ddb_get_hw_plane_state(dev_priv, pipe,
					   plane_id,
					   &ddb[plane_id],
					   &ddb_y[plane_id]);

	intel_display_power_put(dev_priv, power_domain, wakeref);
}

struct dbuf_slice_conf_entry {
	u8 active_pipes;
	u8 dbuf_mask[I915_MAX_PIPES];
	bool join_mbus;
};

/*
 * Table taken from Bspec 49255
 * Pipes do have some preferred DBuf slice affinity,
 * plus there are some hardcoded requirements on how
 * those should be distributed for multipipe scenarios.
 * For more DBuf slices algorithm can get even more messy
 * and less readable, so decided to use a table almost
 * as is from BSpec itself - that way it is at least easier
 * to compare, change and check.
 */
static const struct dbuf_slice_conf_entry tgl_allowed_dbufs[] =
/* Autogenerated with igt/tools/intel_dbuf_map tool: */
{
	{
		.active_pipes = BIT(PIPE_A),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1) | BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_B),
		.dbuf_mask = {
			[PIPE_B] = BIT(DBUF_S1) | BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_B),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S2),
			[PIPE_B] = BIT(DBUF_S1),
		},
	},
	{
		.active_pipes = BIT(PIPE_C),
		.dbuf_mask = {
			[PIPE_C] = BIT(DBUF_S2) | BIT(DBUF_S1),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_C),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1),
			[PIPE_C] = BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_B) | BIT(PIPE_C),
		.dbuf_mask = {
			[PIPE_B] = BIT(DBUF_S1),
			[PIPE_C] = BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_B) | BIT(PIPE_C),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1),
			[PIPE_B] = BIT(DBUF_S1),
			[PIPE_C] = BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_D] = BIT(DBUF_S2) | BIT(DBUF_S1),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1),
			[PIPE_D] = BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_B) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_B] = BIT(DBUF_S1),
			[PIPE_D] = BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_B) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1),
			[PIPE_B] = BIT(DBUF_S1),
			[PIPE_D] = BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_C) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_C] = BIT(DBUF_S1),
			[PIPE_D] = BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_C) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1),
			[PIPE_C] = BIT(DBUF_S2),
			[PIPE_D] = BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_B) | BIT(PIPE_C) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_B] = BIT(DBUF_S1),
			[PIPE_C] = BIT(DBUF_S2),
			[PIPE_D] = BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_B) | BIT(PIPE_C) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1),
			[PIPE_B] = BIT(DBUF_S1),
			[PIPE_C] = BIT(DBUF_S2),
			[PIPE_D] = BIT(DBUF_S2),
		},
	},
	{}
};

static const struct dbuf_slice_conf_entry dg2_allowed_dbufs[] = {
	{
		.active_pipes = BIT(PIPE_A),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1) | BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_B),
		.dbuf_mask = {
			[PIPE_B] = BIT(DBUF_S1) | BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_B),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1),
			[PIPE_B] = BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_C),
		.dbuf_mask = {
			[PIPE_C] = BIT(DBUF_S3) | BIT(DBUF_S4),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_C),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1) | BIT(DBUF_S2),
			[PIPE_C] = BIT(DBUF_S3) | BIT(DBUF_S4),
		},
	},
	{
		.active_pipes = BIT(PIPE_B) | BIT(PIPE_C),
		.dbuf_mask = {
			[PIPE_B] = BIT(DBUF_S1) | BIT(DBUF_S2),
			[PIPE_C] = BIT(DBUF_S3) | BIT(DBUF_S4),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_B) | BIT(PIPE_C),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1),
			[PIPE_B] = BIT(DBUF_S2),
			[PIPE_C] = BIT(DBUF_S3) | BIT(DBUF_S4),
		},
	},
	{
		.active_pipes = BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_D] = BIT(DBUF_S3) | BIT(DBUF_S4),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1) | BIT(DBUF_S2),
			[PIPE_D] = BIT(DBUF_S3) | BIT(DBUF_S4),
		},
	},
	{
		.active_pipes = BIT(PIPE_B) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_B] = BIT(DBUF_S1) | BIT(DBUF_S2),
			[PIPE_D] = BIT(DBUF_S3) | BIT(DBUF_S4),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_B) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1),
			[PIPE_B] = BIT(DBUF_S2),
			[PIPE_D] = BIT(DBUF_S3) | BIT(DBUF_S4),
		},
	},
	{
		.active_pipes = BIT(PIPE_C) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_C] = BIT(DBUF_S3),
			[PIPE_D] = BIT(DBUF_S4),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_C) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1) | BIT(DBUF_S2),
			[PIPE_C] = BIT(DBUF_S3),
			[PIPE_D] = BIT(DBUF_S4),
		},
	},
	{
		.active_pipes = BIT(PIPE_B) | BIT(PIPE_C) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_B] = BIT(DBUF_S1) | BIT(DBUF_S2),
			[PIPE_C] = BIT(DBUF_S3),
			[PIPE_D] = BIT(DBUF_S4),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_B) | BIT(PIPE_C) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1),
			[PIPE_B] = BIT(DBUF_S2),
			[PIPE_C] = BIT(DBUF_S3),
			[PIPE_D] = BIT(DBUF_S4),
		},
	},
	{}
};

static const struct dbuf_slice_conf_entry adlp_allowed_dbufs[] = {
	/*
	 * Keep the join_mbus cases first so check_mbus_joined()
	 * will prefer them over the !join_mbus cases.
	 */
	{
		.active_pipes = BIT(PIPE_A),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1) | BIT(DBUF_S2) | BIT(DBUF_S3) | BIT(DBUF_S4),
		},
		.join_mbus = true,
	},
	{
		.active_pipes = BIT(PIPE_B),
		.dbuf_mask = {
			[PIPE_B] = BIT(DBUF_S1) | BIT(DBUF_S2) | BIT(DBUF_S3) | BIT(DBUF_S4),
		},
		.join_mbus = true,
	},
	{
		.active_pipes = BIT(PIPE_A),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1) | BIT(DBUF_S2),
		},
		.join_mbus = false,
	},
	{
		.active_pipes = BIT(PIPE_B),
		.dbuf_mask = {
			[PIPE_B] = BIT(DBUF_S3) | BIT(DBUF_S4),
		},
		.join_mbus = false,
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_B),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1) | BIT(DBUF_S2),
			[PIPE_B] = BIT(DBUF_S3) | BIT(DBUF_S4),
		},
	},
	{
		.active_pipes = BIT(PIPE_C),
		.dbuf_mask = {
			[PIPE_C] = BIT(DBUF_S3) | BIT(DBUF_S4),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_C),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1) | BIT(DBUF_S2),
			[PIPE_C] = BIT(DBUF_S3) | BIT(DBUF_S4),
		},
	},
	{
		.active_pipes = BIT(PIPE_B) | BIT(PIPE_C),
		.dbuf_mask = {
			[PIPE_B] = BIT(DBUF_S3) | BIT(DBUF_S4),
			[PIPE_C] = BIT(DBUF_S3) | BIT(DBUF_S4),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_B) | BIT(PIPE_C),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1) | BIT(DBUF_S2),
			[PIPE_B] = BIT(DBUF_S3) | BIT(DBUF_S4),
			[PIPE_C] = BIT(DBUF_S3) | BIT(DBUF_S4),
		},
	},
	{
		.active_pipes = BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_D] = BIT(DBUF_S1) | BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1) | BIT(DBUF_S2),
			[PIPE_D] = BIT(DBUF_S1) | BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_B) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_B] = BIT(DBUF_S3) | BIT(DBUF_S4),
			[PIPE_D] = BIT(DBUF_S1) | BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_B) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1) | BIT(DBUF_S2),
			[PIPE_B] = BIT(DBUF_S3) | BIT(DBUF_S4),
			[PIPE_D] = BIT(DBUF_S1) | BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_C) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_C] = BIT(DBUF_S3) | BIT(DBUF_S4),
			[PIPE_D] = BIT(DBUF_S1) | BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_C) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1) | BIT(DBUF_S2),
			[PIPE_C] = BIT(DBUF_S3) | BIT(DBUF_S4),
			[PIPE_D] = BIT(DBUF_S1) | BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_B) | BIT(PIPE_C) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_B] = BIT(DBUF_S3) | BIT(DBUF_S4),
			[PIPE_C] = BIT(DBUF_S3) | BIT(DBUF_S4),
			[PIPE_D] = BIT(DBUF_S1) | BIT(DBUF_S2),
		},
	},
	{
		.active_pipes = BIT(PIPE_A) | BIT(PIPE_B) | BIT(PIPE_C) | BIT(PIPE_D),
		.dbuf_mask = {
			[PIPE_A] = BIT(DBUF_S1) | BIT(DBUF_S2),
			[PIPE_B] = BIT(DBUF_S3) | BIT(DBUF_S4),
			[PIPE_C] = BIT(DBUF_S3) | BIT(DBUF_S4),
			[PIPE_D] = BIT(DBUF_S1) | BIT(DBUF_S2),
		},
	},
	{}

};

static bool check_mbus_joined(u8 active_pipes,
			      const struct dbuf_slice_conf_entry *dbuf_slices)
{
	int i;

	for (i = 0; dbuf_slices[i].active_pipes != 0; i++) {
		if (dbuf_slices[i].active_pipes == active_pipes)
			return dbuf_slices[i].join_mbus;
	}
	return false;
}

static bool adlp_check_mbus_joined(u8 active_pipes)
{
	return check_mbus_joined(active_pipes, adlp_allowed_dbufs);
}

static u8 compute_dbuf_slices(enum pipe pipe, u8 active_pipes, bool join_mbus,
			      const struct dbuf_slice_conf_entry *dbuf_slices)
{
	int i;

	for (i = 0; dbuf_slices[i].active_pipes != 0; i++) {
		if (dbuf_slices[i].active_pipes == active_pipes &&
		    dbuf_slices[i].join_mbus == join_mbus)
			return dbuf_slices[i].dbuf_mask[pipe];
	}
	return 0;
}

static u8 tgl_compute_dbuf_slices(enum pipe pipe, u8 active_pipes, bool join_mbus)
{
	return compute_dbuf_slices(pipe, active_pipes, join_mbus,
				   tgl_allowed_dbufs);
}

static u8 adlp_compute_dbuf_slices(enum pipe pipe, u8 active_pipes, bool join_mbus)
{
	return compute_dbuf_slices(pipe, active_pipes, join_mbus,
				   adlp_allowed_dbufs);
}

static u8 dg2_compute_dbuf_slices(enum pipe pipe, u8 active_pipes, bool join_mbus)
{
	return compute_dbuf_slices(pipe, active_pipes, join_mbus,
				   dg2_allowed_dbufs);
}

static u8 skl_compute_dbuf_slices(struct intel_crtc *crtc, u8 active_pipes, bool join_mbus)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum pipe pipe = crtc->pipe;

	if (IS_DG2(dev_priv))
		return dg2_compute_dbuf_slices(pipe, active_pipes, join_mbus);
	else if (DISPLAY_VER(dev_priv) >= 13)
		return adlp_compute_dbuf_slices(pipe, active_pipes, join_mbus);
	else
		return tgl_compute_dbuf_slices(pipe, active_pipes, join_mbus);
}

static bool
use_minimal_wm0_only(const struct intel_crtc_state *crtc_state,
		     struct intel_plane *plane)
{
	struct drm_i915_private *i915 = to_i915(plane->base.dev);

	return DISPLAY_VER(i915) >= 13 &&
	       crtc_state->uapi.async_flip &&
	       plane->async_flip;
}

static u64
skl_total_relative_data_rate(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	enum plane_id plane_id;
	u64 data_rate = 0;

	for_each_plane_id_on_crtc(crtc, plane_id) {
		if (plane_id == PLANE_CURSOR)
			continue;

		data_rate += crtc_state->rel_data_rate[plane_id];
	}

	return data_rate;
}

static const struct skl_wm_level *
skl_plane_wm_level(const struct skl_pipe_wm *pipe_wm,
		   enum plane_id plane_id,
		   int level)
{
	const struct skl_plane_wm *wm = &pipe_wm->planes[plane_id];

	if (level == 0 && pipe_wm->use_sagv_wm)
		return &wm->sagv.wm0;

	return &wm->wm[level];
}

static const struct skl_wm_level *
skl_plane_trans_wm(const struct skl_pipe_wm *pipe_wm,
		   enum plane_id plane_id)
{
	const struct skl_plane_wm *wm = &pipe_wm->planes[plane_id];

	if (pipe_wm->use_sagv_wm)
		return &wm->sagv.trans_wm;

	return &wm->trans_wm;
}

/*
 * We only disable the watermarks for each plane if
 * they exceed the ddb allocation of said plane. This
 * is done so that we don't end up touching cursor
 * watermarks needlessly when some other plane reduces
 * our max possible watermark level.
 *
 * Bspec has this to say about the PLANE_WM enable bit:
 * "All the watermarks at this level for all enabled
 *  planes must be enabled before the level will be used."
 * So this is actually safe to do.
 */
static void
skl_check_wm_level(struct skl_wm_level *wm, const struct skl_ddb_entry *ddb)
{
	if (wm->min_ddb_alloc > skl_ddb_entry_size(ddb))
		memset(wm, 0, sizeof(*wm));
}

static bool icl_need_wm1_wa(struct drm_i915_private *i915,
			    enum plane_id plane_id)
{
	/*
	 * Wa_1408961008:icl, ehl
	 * Wa_14012656716:tgl, adl
	 * Underruns with WM1+ disabled
	 */
	return (IS_DISPLAY_VER(i915, 12, 13) && plane_id == PLANE_CURSOR);
}

struct skl_plane_ddb_iter {
	u64 data_rate;
	u16 start, size;
};

static void
skl_allocate_plane_ddb(struct skl_plane_ddb_iter *iter,
		       struct skl_ddb_entry *ddb,
		       const struct skl_wm_level *wm,
		       u64 data_rate)
{
	u16 size, extra = 0;

	if (data_rate) {
		extra = min_t(u16, iter->size,
			      DIV64_U64_ROUND_UP(iter->size * data_rate,
						 iter->data_rate));
		iter->size -= extra;
		iter->data_rate -= data_rate;
	}

	/*
	 * Keep ddb entry of all disabled planes explicitly zeroed
	 * to avoid skl_ddb_add_affected_planes() adding them to
	 * the state when other planes change their allocations.
	 */
	size = wm->min_ddb_alloc + extra;
	if (size)
		iter->start = skl_ddb_entry_init(ddb, iter->start,
						 iter->start + size);
}

static int
skl_crtc_allocate_plane_ddb(struct intel_atomic_state *state,
			    struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	struct intel_crtc_state *crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	const struct intel_dbuf_state *dbuf_state =
		intel_atomic_get_new_dbuf_state(state);
	const struct skl_ddb_entry *alloc = &dbuf_state->ddb[crtc->pipe];
	int num_active = hweight8(dbuf_state->active_pipes);
	struct skl_plane_ddb_iter iter;
	enum plane_id plane_id;
	u16 cursor_size;
	u32 blocks;
	int level;

	/* Clear the partitioning for disabled planes. */
	memset(crtc_state->wm.skl.plane_ddb, 0, sizeof(crtc_state->wm.skl.plane_ddb));
	memset(crtc_state->wm.skl.plane_ddb_y, 0, sizeof(crtc_state->wm.skl.plane_ddb_y));

	if (!crtc_state->hw.active)
		return 0;

	iter.start = alloc->start;
	iter.size = skl_ddb_entry_size(alloc);
	if (iter.size == 0)
		return 0;

	/* Allocate fixed number of blocks for cursor. */
	cursor_size = skl_cursor_allocation(crtc_state, num_active);
	iter.size -= cursor_size;
	skl_ddb_entry_init(&crtc_state->wm.skl.plane_ddb[PLANE_CURSOR],
			   alloc->end - cursor_size, alloc->end);

	iter.data_rate = skl_total_relative_data_rate(crtc_state);

	/*
	 * Find the highest watermark level for which we can satisfy the block
	 * requirement of active planes.
	 */
	for (level = ilk_wm_max_level(dev_priv); level >= 0; level--) {
		blocks = 0;
		for_each_plane_id_on_crtc(crtc, plane_id) {
			const struct skl_plane_wm *wm =
				&crtc_state->wm.skl.optimal.planes[plane_id];

			if (plane_id == PLANE_CURSOR) {
				const struct skl_ddb_entry *ddb =
					&crtc_state->wm.skl.plane_ddb[plane_id];

				if (wm->wm[level].min_ddb_alloc > skl_ddb_entry_size(ddb)) {
					drm_WARN_ON(&dev_priv->drm,
						    wm->wm[level].min_ddb_alloc != U16_MAX);
					blocks = U32_MAX;
					break;
				}
				continue;
			}

			blocks += wm->wm[level].min_ddb_alloc;
			blocks += wm->uv_wm[level].min_ddb_alloc;
		}

		if (blocks <= iter.size) {
			iter.size -= blocks;
			break;
		}
	}

	if (level < 0) {
		drm_dbg_kms(&dev_priv->drm,
			    "Requested display configuration exceeds system DDB limitations");
		drm_dbg_kms(&dev_priv->drm, "minimum required %d/%d\n",
			    blocks, iter.size);
		return -EINVAL;
	}

	/* avoid the WARN later when we don't allocate any extra DDB */
	if (iter.data_rate == 0)
		iter.size = 0;

	/*
	 * Grant each plane the blocks it requires at the highest achievable
	 * watermark level, plus an extra share of the leftover blocks
	 * proportional to its relative data rate.
	 */
	for_each_plane_id_on_crtc(crtc, plane_id) {
		struct skl_ddb_entry *ddb =
			&crtc_state->wm.skl.plane_ddb[plane_id];
		const struct skl_plane_wm *wm =
			&crtc_state->wm.skl.optimal.planes[plane_id];

		if (plane_id == PLANE_CURSOR)
			continue;

		skl_allocate_plane_ddb(&iter, ddb, &wm->wm[level],
				       crtc_state->rel_data_rate[plane_id]);
	}
	drm_WARN_ON(&dev_priv->drm, iter.size != 0 || iter.data_rate != 0);

	/*
	 * When we calculated watermark values we didn't know how high
	 * of a level we'd actually be able to hit, so we just marked
	 * all levels as "enabled."  Go back now and disable the ones
	 * that aren't actually possible.
	 */
	for (level++; level <= ilk_wm_max_level(dev_priv); level++) {
		for_each_plane_id_on_crtc(crtc, plane_id) {
			const struct skl_ddb_entry *ddb =
				&crtc_state->wm.skl.plane_ddb[plane_id];
			struct skl_plane_wm *wm =
				&crtc_state->wm.skl.optimal.planes[plane_id];

			skl_check_wm_level(&wm->wm[level], ddb);

			if (icl_need_wm1_wa(dev_priv, plane_id) &&
			    level == 1 && wm->wm[0].enable) {
				wm->wm[level].blocks = wm->wm[0].blocks;
				wm->wm[level].lines = wm->wm[0].lines;
				wm->wm[level].ignore_lines = wm->wm[0].ignore_lines;
			}
		}
	}

	/*
	 * Go back and disable the transition and SAGV watermarks
	 * if it turns out we don't have enough DDB blocks for them.
	 */
	for_each_plane_id_on_crtc(crtc, plane_id) {
		const struct skl_ddb_entry *ddb =
			&crtc_state->wm.skl.plane_ddb[plane_id];
		const struct skl_ddb_entry *ddb_y =
			&crtc_state->wm.skl.plane_ddb_y[plane_id];
		struct skl_plane_wm *wm =
			&crtc_state->wm.skl.optimal.planes[plane_id];

		WARN_ON(skl_ddb_entry_size(ddb_y));

		skl_check_wm_level(&wm->trans_wm, ddb);

		skl_check_wm_level(&wm->sagv.wm0, ddb);
		skl_check_wm_level(&wm->sagv.trans_wm, ddb);
	}

	return 0;
}

/*
 * The max latency should be 257 (max the punit can code is 255 and we add 2us
 * for the read latency) and cpp should always be <= 8, so that
 * should allow pixel_rate up to ~2 GHz which seems sufficient since max
 * 2xcdclk is 1350 MHz and the pixel rate should never exceed that.
*/
static uint_fixed_16_16_t
skl_wm_method1(const struct drm_i915_private *dev_priv, u32 pixel_rate,
	       u8 cpp, u32 latency, u32 dbuf_block_size)
{
	u32 wm_intermediate_val;
	uint_fixed_16_16_t ret;

	if (latency == 0)
		return FP_16_16_MAX;

	wm_intermediate_val = latency * pixel_rate * cpp;
	ret = div_fixed16(wm_intermediate_val, 1000 * dbuf_block_size);
	ret = add_fixed16_u32(ret, 1);

	return ret;
}

static uint_fixed_16_16_t
skl_wm_method2(u32 pixel_rate, u32 pipe_htotal, u32 latency,
	       uint_fixed_16_16_t plane_blocks_per_line)
{
	u32 wm_intermediate_val;
	uint_fixed_16_16_t ret;

	if (latency == 0)
		return FP_16_16_MAX;

	wm_intermediate_val = latency * pixel_rate;
	wm_intermediate_val = DIV_ROUND_UP(wm_intermediate_val,
					   pipe_htotal * 1000);
	ret = mul_u32_fixed16(wm_intermediate_val, plane_blocks_per_line);
	return ret;
}

static uint_fixed_16_16_t
intel_get_linetime_us(const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(crtc_state->uapi.crtc->dev);
	u32 pixel_rate;
	u32 crtc_htotal;
	uint_fixed_16_16_t linetime_us;

	if (!crtc_state->hw.active)
		return u32_to_fixed16(0);

	pixel_rate = crtc_state->pixel_rate;

	if (drm_WARN_ON(&dev_priv->drm, pixel_rate == 0))
		return u32_to_fixed16(0);

	crtc_htotal = crtc_state->hw.pipe_mode.crtc_htotal;
	linetime_us = div_fixed16(crtc_htotal * 1000, pixel_rate);

	return linetime_us;
}

static int
skl_compute_wm_params(const struct intel_crtc_state *crtc_state,
		      int width, const struct drm_format_info *format,
		      u64 modifier, unsigned int rotation,
		      u32 plane_pixel_rate, struct skl_wm_params *wp,
		      int color_plane)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	u32 interm_pbpl;

	/* only planar format has two planes */
	if (color_plane == 1 &&
	    !intel_format_info_is_yuv_semiplanar(format, modifier)) {
		drm_dbg_kms(&dev_priv->drm,
			    "Non planar format have single plane\n");
		return -EINVAL;
	}

	wp->y_tiled = modifier == I915_FORMAT_MOD_Y_TILED ||
		      modifier == I915_FORMAT_MOD_4_TILED ||
		      modifier == I915_FORMAT_MOD_Yf_TILED ||
		      modifier == I915_FORMAT_MOD_Y_TILED_CCS ||
		      modifier == I915_FORMAT_MOD_Yf_TILED_CCS;
	wp->x_tiled = modifier == I915_FORMAT_MOD_X_TILED;
	wp->rc_surface = modifier == I915_FORMAT_MOD_Y_TILED_CCS ||
			 modifier == I915_FORMAT_MOD_Yf_TILED_CCS;
	wp->is_planar = intel_format_info_is_yuv_semiplanar(format, modifier);

	wp->width = width;
	if (color_plane == 1 && wp->is_planar)
		wp->width /= 2;

	wp->cpp = format->cpp[color_plane];
	wp->plane_pixel_rate = plane_pixel_rate;

	if (modifier == I915_FORMAT_MOD_Yf_TILED  && wp->cpp == 1)
		wp->dbuf_block_size = 256;
	else
		wp->dbuf_block_size = 512;

	if (drm_rotation_90_or_270(rotation)) {
		switch (wp->cpp) {
		case 1:
			wp->y_min_scanlines = 16;
			break;
		case 2:
			wp->y_min_scanlines = 8;
			break;
		case 4:
			wp->y_min_scanlines = 4;
			break;
		default:
			MISSING_CASE(wp->cpp);
			return -EINVAL;
		}
	} else {
		wp->y_min_scanlines = 4;
	}

	if (skl_needs_memory_bw_wa(dev_priv))
		wp->y_min_scanlines *= 2;

	wp->plane_bytes_per_line = wp->width * wp->cpp;
	if (wp->y_tiled) {
		interm_pbpl = DIV_ROUND_UP(wp->plane_bytes_per_line *
					   wp->y_min_scanlines,
					   wp->dbuf_block_size);
		interm_pbpl++;

		wp->plane_blocks_per_line = div_fixed16(interm_pbpl,
							wp->y_min_scanlines);
	} else {
		interm_pbpl = DIV_ROUND_UP(wp->plane_bytes_per_line,
					   wp->dbuf_block_size);

		if (!wp->x_tiled)
			interm_pbpl++;

		wp->plane_blocks_per_line = u32_to_fixed16(interm_pbpl);
	}

	wp->y_tile_minimum = mul_u32_fixed16(wp->y_min_scanlines,
					     wp->plane_blocks_per_line);

	wp->linetime_us = fixed16_to_u32_round_up(
					intel_get_linetime_us(crtc_state));

	return 0;
}

static int
skl_compute_plane_wm_params(const struct intel_crtc_state *crtc_state,
			    const struct intel_plane_state *plane_state,
			    struct skl_wm_params *wp, int color_plane)
{
	const struct drm_framebuffer *fb = plane_state->hw.fb;
	int width;

	/*
	 * Src coordinates are already rotated by 270 degrees for
	 * the 90/270 degree plane rotation cases (to match the
	 * GTT mapping), hence no need to account for rotation here.
	 */
	width = drm_rect_width(&plane_state->uapi.src) >> 16;

	return skl_compute_wm_params(crtc_state, width,
				     fb->format, fb->modifier,
				     plane_state->hw.rotation,
				     intel_plane_pixel_rate(crtc_state, plane_state),
				     wp, color_plane);
}

static bool skl_wm_has_lines(struct drm_i915_private *dev_priv, int level)
{
	return true;
}

static int skl_wm_max_lines(struct drm_i915_private *dev_priv)
{
	if (DISPLAY_VER(dev_priv) >= 13)
		return 255;
	else
		return 31;
}

static void skl_compute_plane_wm(const struct intel_crtc_state *crtc_state,
				 struct intel_plane *plane,
				 int level,
				 unsigned int latency,
				 const struct skl_wm_params *wp,
				 const struct skl_wm_level *result_prev,
				 struct skl_wm_level *result /* out */)
{
	struct drm_i915_private *dev_priv = to_i915(crtc_state->uapi.crtc->dev);
	uint_fixed_16_16_t method1, method2;
	uint_fixed_16_16_t selected_result;
	u32 blocks, lines, min_ddb_alloc = 0;

	if (latency == 0 ||
	    (use_minimal_wm0_only(crtc_state, plane) && level > 0)) {
		/* reject it */
		result->min_ddb_alloc = U16_MAX;
		return;
	}

	/*
	 * WaIncreaseLatencyIPCEnabled: kbl,cfl
	 * Display WA #1141: kbl,cfl
	 */
	if ((IS_KABYLAKE(dev_priv) ||
	     IS_COFFEELAKE(dev_priv) ||
	     IS_COMETLAKE(dev_priv)) &&
	    dev_priv->ipc_enabled)
		latency += 4;

	if (skl_needs_memory_bw_wa(dev_priv) && wp->x_tiled)
		latency += 15;

	method1 = skl_wm_method1(dev_priv, wp->plane_pixel_rate,
				 wp->cpp, latency, wp->dbuf_block_size);
	method2 = skl_wm_method2(wp->plane_pixel_rate,
				 crtc_state->hw.pipe_mode.crtc_htotal,
				 latency,
				 wp->plane_blocks_per_line);

	if (wp->y_tiled) {
		selected_result = max_fixed16(method2, wp->y_tile_minimum);
	} else {
		if ((wp->cpp * crtc_state->hw.pipe_mode.crtc_htotal /
		     wp->dbuf_block_size < 1) &&
		     (wp->plane_bytes_per_line / wp->dbuf_block_size < 1)) {
			selected_result = method2;
		} else if (latency >= wp->linetime_us) {
			selected_result = method2;
		} else {
			selected_result = method1;
		}
	}

	blocks = fixed16_to_u32_round_up(selected_result) + 1;
	/*
	 * Lets have blocks at minimum equivalent to plane_blocks_per_line
	 * as there will be at minimum one line for lines configuration. This
	 * is a work around for FIFO underruns observed with resolutions like
	 * 4k 60 Hz in single channel DRAM configurations.
	 *
	 * As per the Bspec 49325, if the ddb allocation can hold at least
	 * one plane_blocks_per_line, we should have selected method2 in
	 * the above logic. Assuming that modern versions have enough dbuf
	 * and method2 guarantees blocks equivalent to at least 1 line,
	 * select the blocks as plane_blocks_per_line.
	 *
	 * TODO: Revisit the logic when we have better understanding on DRAM
	 * channels' impact on the level 0 memory latency and the relevant
	 * wm calculations.
	 */
	if (skl_wm_has_lines(dev_priv, level))
		blocks = max(blocks,
			     fixed16_to_u32_round_up(wp->plane_blocks_per_line));
	lines = div_round_up_fixed16(selected_result,
				     wp->plane_blocks_per_line);

	if (wp->y_tiled) {
		int extra_lines;

		if (lines % wp->y_min_scanlines == 0)
			extra_lines = wp->y_min_scanlines;
		else
			extra_lines = wp->y_min_scanlines * 2 -
				lines % wp->y_min_scanlines;

		min_ddb_alloc = mul_round_up_u32_fixed16(lines + extra_lines,
							 wp->plane_blocks_per_line);
	} else {
		min_ddb_alloc = blocks + DIV_ROUND_UP(blocks, 10);
	}

	if (!skl_wm_has_lines(dev_priv, level))
		lines = 0;

	if (lines > skl_wm_max_lines(dev_priv)) {
		/* reject it */
		result->min_ddb_alloc = U16_MAX;
		return;
	}

	/*
	 * If lines is valid, assume we can use this watermark level
	 * for now.  We'll come back and disable it after we calculate the
	 * DDB allocation if it turns out we don't actually have enough
	 * blocks to satisfy it.
	 */
	result->blocks = blocks;
	result->lines = lines;
	/* Bspec says: value >= plane ddb allocation -> invalid, hence the +1 here */
	result->min_ddb_alloc = max(min_ddb_alloc, blocks) + 1;
	result->enable = true;
}

static void
skl_compute_wm_levels(const struct intel_crtc_state *crtc_state,
		      struct intel_plane *plane,
		      const struct skl_wm_params *wm_params,
		      struct skl_wm_level *levels)
{
	struct drm_i915_private *dev_priv = to_i915(crtc_state->uapi.crtc->dev);
	int level, max_level = ilk_wm_max_level(dev_priv);
	struct skl_wm_level *result_prev = &levels[0];

	for (level = 0; level <= max_level; level++) {
		struct skl_wm_level *result = &levels[level];
		unsigned int latency = dev_priv->wm.skl_latency[level];

		skl_compute_plane_wm(crtc_state, plane, level, latency,
				     wm_params, result_prev, result);

		result_prev = result;
	}
}

static void tgl_compute_sagv_wm(const struct intel_crtc_state *crtc_state,
				struct intel_plane *plane,
				const struct skl_wm_params *wm_params,
				struct skl_plane_wm *plane_wm)
{
	struct drm_i915_private *dev_priv = to_i915(crtc_state->uapi.crtc->dev);
	struct skl_wm_level *sagv_wm = &plane_wm->sagv.wm0;
	struct skl_wm_level *levels = plane_wm->wm;
	unsigned int latency = 0;

	if (dev_priv->sagv_block_time_us)
		latency = dev_priv->sagv_block_time_us + dev_priv->wm.skl_latency[0];

	skl_compute_plane_wm(crtc_state, plane, 0, latency,
			     wm_params, &levels[0],
			     sagv_wm);
}

static void skl_compute_transition_wm(struct drm_i915_private *dev_priv,
				      struct skl_wm_level *trans_wm,
				      const struct skl_wm_level *wm0,
				      const struct skl_wm_params *wp)
{
	u16 trans_min, trans_amount, trans_y_tile_min;
	u16 wm0_blocks, trans_offset, blocks;

	/* Transition WM don't make any sense if ipc is disabled */
	if (!dev_priv->ipc_enabled)
		return;

	/*
	 * WaDisableTWM:skl,kbl,cfl,bxt
	 * Transition WM are not recommended by HW team for GEN9
	 */
	trans_min = 4;
	trans_amount = 10; /* This is configurable amount */

	trans_offset = trans_min + trans_amount;

	/*
	 * The spec asks for Selected Result Blocks for wm0 (the real value),
	 * not Result Blocks (the integer value). Pay attention to the capital
	 * letters. The value wm_l0->blocks is actually Result Blocks, but
	 * since Result Blocks is the ceiling of Selected Result Blocks plus 1,
	 * and since we later will have to get the ceiling of the sum in the
	 * transition watermarks calculation, we can just pretend Selected
	 * Result Blocks is Result Blocks minus 1 and it should work for the
	 * current platforms.
	 */
	wm0_blocks = wm0->blocks - 1;

	if (wp->y_tiled) {
		trans_y_tile_min =
			(u16)mul_round_up_u32_fixed16(2, wp->y_tile_minimum);
		blocks = max(wm0_blocks, trans_y_tile_min) + trans_offset;
	} else {
		blocks = wm0_blocks + trans_offset;
	}
	blocks++;

	/*
	 * Just assume we can enable the transition watermark.  After
	 * computing the DDB we'll come back and disable it if that
	 * assumption turns out to be false.
	 */
	trans_wm->blocks = blocks;
	trans_wm->min_ddb_alloc = max_t(u16, wm0->min_ddb_alloc, blocks + 1);
	trans_wm->enable = true;
}

static int skl_build_plane_wm_single(struct intel_crtc_state *crtc_state,
				     const struct intel_plane_state *plane_state,
				     struct intel_plane *plane, int color_plane)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	struct skl_plane_wm *wm = &crtc_state->wm.skl.raw.planes[plane->id];
	struct skl_wm_params wm_params;
	int ret;

	ret = skl_compute_plane_wm_params(crtc_state, plane_state,
					  &wm_params, color_plane);
	if (ret)
		return ret;

	skl_compute_wm_levels(crtc_state, plane, &wm_params, wm->wm);

	skl_compute_transition_wm(dev_priv, &wm->trans_wm,
				  &wm->wm[0], &wm_params);

	tgl_compute_sagv_wm(crtc_state, plane, &wm_params, wm);

	skl_compute_transition_wm(dev_priv, &wm->sagv.trans_wm,
				  &wm->sagv.wm0, &wm_params);

	return 0;
}

static int icl_build_plane_wm(struct intel_crtc_state *crtc_state,
			      const struct intel_plane_state *plane_state)
{
	struct intel_plane *plane = to_intel_plane(plane_state->uapi.plane);
	struct drm_i915_private *dev_priv = to_i915(plane->base.dev);
	enum plane_id plane_id = plane->id;
	struct skl_plane_wm *wm = &crtc_state->wm.skl.raw.planes[plane_id];
	int ret;

	/* Watermarks calculated in master */
	if (plane_state->planar_slave)
		return 0;

	memset(wm, 0, sizeof(*wm));

	if (plane_state->planar_linked_plane) {
		const struct drm_framebuffer *fb = plane_state->hw.fb;

		drm_WARN_ON(&dev_priv->drm,
			    !intel_wm_plane_visible(crtc_state, plane_state));
		drm_WARN_ON(&dev_priv->drm, !fb->format->is_yuv ||
			    fb->format->num_planes == 1);

		ret = skl_build_plane_wm_single(crtc_state, plane_state,
						plane_state->planar_linked_plane, 0);
		if (ret)
			return ret;

		ret = skl_build_plane_wm_single(crtc_state, plane_state,
						plane, 1);
		if (ret)
			return ret;
	} else if (intel_wm_plane_visible(crtc_state, plane_state)) {
		ret = skl_build_plane_wm_single(crtc_state, plane_state,
						plane, 0);
		if (ret)
			return ret;
	}

	return 0;
}

static int skl_build_pipe_wm(struct intel_atomic_state *state,
			     struct intel_crtc *crtc)
{
	struct intel_crtc_state *crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	const struct intel_plane_state *plane_state;
	struct intel_plane *plane;
	int ret, i;

	for_each_new_intel_plane_in_state(state, plane, plane_state, i) {
		/*
		 * FIXME should perhaps check {old,new}_plane_crtc->hw.crtc
		 * instead but we don't populate that correctly for NV12 Y
		 * planes so for now hack this.
		 */
		if (plane->pipe != crtc->pipe)
			continue;

		ret = icl_build_plane_wm(crtc_state, plane_state);
		if (ret)
			return ret;
	}

	crtc_state->wm.skl.optimal = crtc_state->wm.skl.raw;

	return 0;
}

static void skl_ddb_entry_write(struct drm_i915_private *dev_priv,
				i915_reg_t reg,
				const struct skl_ddb_entry *entry)
{
	if (entry->end)
		intel_de_write_fw(dev_priv, reg,
				  PLANE_BUF_END(entry->end - 1) |
				  PLANE_BUF_START(entry->start));
	else
		intel_de_write_fw(dev_priv, reg, 0);
}

static void skl_write_wm_level(struct drm_i915_private *dev_priv,
			       i915_reg_t reg,
			       const struct skl_wm_level *level)
{
	u32 val = 0;

	if (level->enable)
		val |= PLANE_WM_EN;
	if (level->ignore_lines)
		val |= PLANE_WM_IGNORE_LINES;
	val |= REG_FIELD_PREP(PLANE_WM_BLOCKS_MASK, level->blocks);
	val |= REG_FIELD_PREP(PLANE_WM_LINES_MASK, level->lines);

	intel_de_write_fw(dev_priv, reg, val);
}

void skl_write_plane_wm(struct intel_plane *plane,
			const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(plane->base.dev);
	int level, max_level = ilk_wm_max_level(dev_priv);
	enum plane_id plane_id = plane->id;
	enum pipe pipe = plane->pipe;
	const struct skl_pipe_wm *pipe_wm = &crtc_state->wm.skl.optimal;
	const struct skl_ddb_entry *ddb =
		&crtc_state->wm.skl.plane_ddb[plane_id];

	for (level = 0; level <= max_level; level++)
		skl_write_wm_level(dev_priv, PLANE_WM(pipe, plane_id, level),
				   skl_plane_wm_level(pipe_wm, plane_id, level));

	skl_write_wm_level(dev_priv, PLANE_WM_TRANS(pipe, plane_id),
			   skl_plane_trans_wm(pipe_wm, plane_id));

	if (HAS_HW_SAGV_WM(dev_priv)) {
		const struct skl_plane_wm *wm = &pipe_wm->planes[plane_id];

		skl_write_wm_level(dev_priv, PLANE_WM_SAGV(pipe, plane_id),
				   &wm->sagv.wm0);
		skl_write_wm_level(dev_priv, PLANE_WM_SAGV_TRANS(pipe, plane_id),
				   &wm->sagv.trans_wm);
	}

	skl_ddb_entry_write(dev_priv,
			    PLANE_BUF_CFG(pipe, plane_id), ddb);
}

void skl_write_cursor_wm(struct intel_plane *plane,
			 const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(plane->base.dev);
	int level, max_level = ilk_wm_max_level(dev_priv);
	enum plane_id plane_id = plane->id;
	enum pipe pipe = plane->pipe;
	const struct skl_pipe_wm *pipe_wm = &crtc_state->wm.skl.optimal;
	const struct skl_ddb_entry *ddb =
		&crtc_state->wm.skl.plane_ddb[plane_id];

	for (level = 0; level <= max_level; level++)
		skl_write_wm_level(dev_priv, CUR_WM(pipe, level),
				   skl_plane_wm_level(pipe_wm, plane_id, level));

	skl_write_wm_level(dev_priv, CUR_WM_TRANS(pipe),
			   skl_plane_trans_wm(pipe_wm, plane_id));

	if (HAS_HW_SAGV_WM(dev_priv)) {
		const struct skl_plane_wm *wm = &pipe_wm->planes[plane_id];

		skl_write_wm_level(dev_priv, CUR_WM_SAGV(pipe),
				   &wm->sagv.wm0);
		skl_write_wm_level(dev_priv, CUR_WM_SAGV_TRANS(pipe),
				   &wm->sagv.trans_wm);
	}

	skl_ddb_entry_write(dev_priv, CUR_BUF_CFG(pipe), ddb);
}

static bool skl_wm_level_equals(const struct skl_wm_level *l1,
				const struct skl_wm_level *l2)
{
	return l1->enable == l2->enable &&
		l1->ignore_lines == l2->ignore_lines &&
		l1->lines == l2->lines &&
		l1->blocks == l2->blocks;
}

static bool skl_plane_wm_equals(struct drm_i915_private *dev_priv,
				const struct skl_plane_wm *wm1,
				const struct skl_plane_wm *wm2)
{
	int level, max_level = ilk_wm_max_level(dev_priv);

	for (level = 0; level <= max_level; level++) {
		/*
		 * We don't check uv_wm as the hardware doesn't actually
		 * use it. It only gets used for calculating the required
		 * ddb allocation.
		 */
		if (!skl_wm_level_equals(&wm1->wm[level], &wm2->wm[level]))
			return false;
	}

	return skl_wm_level_equals(&wm1->trans_wm, &wm2->trans_wm) &&
		skl_wm_level_equals(&wm1->sagv.wm0, &wm2->sagv.wm0) &&
		skl_wm_level_equals(&wm1->sagv.trans_wm, &wm2->sagv.trans_wm);
}

static bool skl_ddb_entries_overlap(const struct skl_ddb_entry *a,
				    const struct skl_ddb_entry *b)
{
	return a->start < b->end && b->start < a->end;
}

static void skl_ddb_entry_union(struct skl_ddb_entry *a,
				const struct skl_ddb_entry *b)
{
	if (a->end && b->end) {
		a->start = min(a->start, b->start);
		a->end = max(a->end, b->end);
	} else if (b->end) {
		a->start = b->start;
		a->end = b->end;
	}
}

bool skl_ddb_allocation_overlaps(const struct skl_ddb_entry *ddb,
				 const struct skl_ddb_entry *entries,
				 int num_entries, int ignore_idx)
{
	int i;

	for (i = 0; i < num_entries; i++) {
		if (i != ignore_idx &&
		    skl_ddb_entries_overlap(ddb, &entries[i]))
			return true;
	}

	return false;
}

static int
skl_ddb_add_affected_planes(const struct intel_crtc_state *old_crtc_state,
			    struct intel_crtc_state *new_crtc_state)
{
	struct intel_atomic_state *state = to_intel_atomic_state(new_crtc_state->uapi.state);
	struct intel_crtc *crtc = to_intel_crtc(new_crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	struct intel_plane *plane;

	for_each_intel_plane_on_crtc(&dev_priv->drm, crtc, plane) {
		struct intel_plane_state *plane_state;
		enum plane_id plane_id = plane->id;

		if (skl_ddb_entry_equal(&old_crtc_state->wm.skl.plane_ddb[plane_id],
					&new_crtc_state->wm.skl.plane_ddb[plane_id]) &&
		    skl_ddb_entry_equal(&old_crtc_state->wm.skl.plane_ddb_y[plane_id],
					&new_crtc_state->wm.skl.plane_ddb_y[plane_id]))
			continue;

		plane_state = intel_atomic_get_plane_state(state, plane);
		if (IS_ERR(plane_state))
			return PTR_ERR(plane_state);

		new_crtc_state->update_planes |= BIT(plane_id);
	}

	return 0;
}

static u8 intel_dbuf_enabled_slices(const struct intel_dbuf_state *dbuf_state)
{
	struct drm_i915_private *dev_priv = to_i915(dbuf_state->base.state->base.dev);
	u8 enabled_slices;
	enum pipe pipe;

	/*
	 * FIXME: For now we always enable slice S1 as per
	 * the Bspec display initialization sequence.
	 */
	enabled_slices = BIT(DBUF_S1);

	for_each_pipe(dev_priv, pipe)
		enabled_slices |= dbuf_state->slices[pipe];

	return enabled_slices;
}

static int
skl_compute_ddb(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	const struct intel_dbuf_state *old_dbuf_state;
	struct intel_dbuf_state *new_dbuf_state = NULL;
	const struct intel_crtc_state *old_crtc_state;
	struct intel_crtc_state *new_crtc_state;
	struct intel_crtc *crtc;
	int ret, i;

	for_each_new_intel_crtc_in_state(state, crtc, new_crtc_state, i) {
		new_dbuf_state = intel_atomic_get_dbuf_state(state);
		if (IS_ERR(new_dbuf_state))
			return PTR_ERR(new_dbuf_state);

		old_dbuf_state = intel_atomic_get_old_dbuf_state(state);
		break;
	}

	if (!new_dbuf_state)
		return 0;

	new_dbuf_state->active_pipes =
		intel_calc_active_pipes(state, old_dbuf_state->active_pipes);

	if (old_dbuf_state->active_pipes != new_dbuf_state->active_pipes) {
		ret = intel_atomic_lock_global_state(&new_dbuf_state->base);
		if (ret)
			return ret;
	}

	if (HAS_MBUS_JOINING(dev_priv))
		new_dbuf_state->joined_mbus =
			adlp_check_mbus_joined(new_dbuf_state->active_pipes);

	for_each_intel_crtc(&dev_priv->drm, crtc) {
		enum pipe pipe = crtc->pipe;

		new_dbuf_state->slices[pipe] =
			skl_compute_dbuf_slices(crtc, new_dbuf_state->active_pipes,
						new_dbuf_state->joined_mbus);

		if (old_dbuf_state->slices[pipe] == new_dbuf_state->slices[pipe])
			continue;

		ret = intel_atomic_lock_global_state(&new_dbuf_state->base);
		if (ret)
			return ret;
	}

	new_dbuf_state->enabled_slices = intel_dbuf_enabled_slices(new_dbuf_state);

	if (old_dbuf_state->enabled_slices != new_dbuf_state->enabled_slices ||
	    old_dbuf_state->joined_mbus != new_dbuf_state->joined_mbus) {
		ret = intel_atomic_serialize_global_state(&new_dbuf_state->base);
		if (ret)
			return ret;

		if (old_dbuf_state->joined_mbus != new_dbuf_state->joined_mbus) {
			/* TODO: Implement vblank synchronized MBUS joining changes */
			ret = intel_modeset_all_pipes(state);
			if (ret)
				return ret;
		}

		drm_dbg_kms(&dev_priv->drm,
			    "Enabled dbuf slices 0x%x -> 0x%x (total dbuf slices 0x%x), mbus joined? %s->%s\n",
			    old_dbuf_state->enabled_slices,
			    new_dbuf_state->enabled_slices,
			    INTEL_INFO(dev_priv)->display.dbuf.slice_mask,
			    str_yes_no(old_dbuf_state->joined_mbus),
			    str_yes_no(new_dbuf_state->joined_mbus));
	}

	for_each_new_intel_crtc_in_state(state, crtc, new_crtc_state, i) {
		enum pipe pipe = crtc->pipe;

		new_dbuf_state->weight[pipe] = intel_crtc_ddb_weight(new_crtc_state);

		if (old_dbuf_state->weight[pipe] == new_dbuf_state->weight[pipe])
			continue;

		ret = intel_atomic_lock_global_state(&new_dbuf_state->base);
		if (ret)
			return ret;
	}

	for_each_intel_crtc(&dev_priv->drm, crtc) {
		ret = skl_crtc_allocate_ddb(state, crtc);
		if (ret)
			return ret;
	}

	for_each_oldnew_intel_crtc_in_state(state, crtc, old_crtc_state,
					    new_crtc_state, i) {
		ret = skl_crtc_allocate_plane_ddb(state, crtc);
		if (ret)
			return ret;

		ret = skl_ddb_add_affected_planes(old_crtc_state,
						  new_crtc_state);
		if (ret)
			return ret;
	}

	return 0;
}

static char enast(bool enable)
{
	return enable ? '*' : ' ';
}

static void
skl_print_wm_changes(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	const struct intel_crtc_state *old_crtc_state;
	const struct intel_crtc_state *new_crtc_state;
	struct intel_plane *plane;
	struct intel_crtc *crtc;
	int i;

	if (!drm_debug_enabled(DRM_UT_KMS))
		return;

	for_each_oldnew_intel_crtc_in_state(state, crtc, old_crtc_state,
					    new_crtc_state, i) {
		const struct skl_pipe_wm *old_pipe_wm, *new_pipe_wm;

		old_pipe_wm = &old_crtc_state->wm.skl.optimal;
		new_pipe_wm = &new_crtc_state->wm.skl.optimal;

		for_each_intel_plane_on_crtc(&dev_priv->drm, crtc, plane) {
			enum plane_id plane_id = plane->id;
			const struct skl_ddb_entry *old, *new;

			old = &old_crtc_state->wm.skl.plane_ddb[plane_id];
			new = &new_crtc_state->wm.skl.plane_ddb[plane_id];

			if (skl_ddb_entry_equal(old, new))
				continue;

			drm_dbg_kms(&dev_priv->drm,
				    "[PLANE:%d:%s] ddb (%4d - %4d) -> (%4d - %4d), size %4d -> %4d\n",
				    plane->base.base.id, plane->base.name,
				    old->start, old->end, new->start, new->end,
				    skl_ddb_entry_size(old), skl_ddb_entry_size(new));
		}

		for_each_intel_plane_on_crtc(&dev_priv->drm, crtc, plane) {
			enum plane_id plane_id = plane->id;
			const struct skl_plane_wm *old_wm, *new_wm;

			old_wm = &old_pipe_wm->planes[plane_id];
			new_wm = &new_pipe_wm->planes[plane_id];

			if (skl_plane_wm_equals(dev_priv, old_wm, new_wm))
				continue;

			drm_dbg_kms(&dev_priv->drm,
				    "[PLANE:%d:%s]   level %cwm0,%cwm1,%cwm2,%cwm3,%cwm4,%cwm5,%cwm6,%cwm7,%ctwm,%cswm,%cstwm"
				    " -> %cwm0,%cwm1,%cwm2,%cwm3,%cwm4,%cwm5,%cwm6,%cwm7,%ctwm,%cswm,%cstwm\n",
				    plane->base.base.id, plane->base.name,
				    enast(old_wm->wm[0].enable), enast(old_wm->wm[1].enable),
				    enast(old_wm->wm[2].enable), enast(old_wm->wm[3].enable),
				    enast(old_wm->wm[4].enable), enast(old_wm->wm[5].enable),
				    enast(old_wm->wm[6].enable), enast(old_wm->wm[7].enable),
				    enast(old_wm->trans_wm.enable),
				    enast(old_wm->sagv.wm0.enable),
				    enast(old_wm->sagv.trans_wm.enable),
				    enast(new_wm->wm[0].enable), enast(new_wm->wm[1].enable),
				    enast(new_wm->wm[2].enable), enast(new_wm->wm[3].enable),
				    enast(new_wm->wm[4].enable), enast(new_wm->wm[5].enable),
				    enast(new_wm->wm[6].enable), enast(new_wm->wm[7].enable),
				    enast(new_wm->trans_wm.enable),
				    enast(new_wm->sagv.wm0.enable),
				    enast(new_wm->sagv.trans_wm.enable));

			drm_dbg_kms(&dev_priv->drm,
				    "[PLANE:%d:%s]   lines %c%3d,%c%3d,%c%3d,%c%3d,%c%3d,%c%3d,%c%3d,%c%3d,%c%3d,%c%3d,%c%4d"
				      " -> %c%3d,%c%3d,%c%3d,%c%3d,%c%3d,%c%3d,%c%3d,%c%3d,%c%3d,%c%3d,%c%4d\n",
				    plane->base.base.id, plane->base.name,
				    enast(old_wm->wm[0].ignore_lines), old_wm->wm[0].lines,
				    enast(old_wm->wm[1].ignore_lines), old_wm->wm[1].lines,
				    enast(old_wm->wm[2].ignore_lines), old_wm->wm[2].lines,
				    enast(old_wm->wm[3].ignore_lines), old_wm->wm[3].lines,
				    enast(old_wm->wm[4].ignore_lines), old_wm->wm[4].lines,
				    enast(old_wm->wm[5].ignore_lines), old_wm->wm[5].lines,
				    enast(old_wm->wm[6].ignore_lines), old_wm->wm[6].lines,
				    enast(old_wm->wm[7].ignore_lines), old_wm->wm[7].lines,
				    enast(old_wm->trans_wm.ignore_lines), old_wm->trans_wm.lines,
				    enast(old_wm->sagv.wm0.ignore_lines), old_wm->sagv.wm0.lines,
				    enast(old_wm->sagv.trans_wm.ignore_lines), old_wm->sagv.trans_wm.lines,
				    enast(new_wm->wm[0].ignore_lines), new_wm->wm[0].lines,
				    enast(new_wm->wm[1].ignore_lines), new_wm->wm[1].lines,
				    enast(new_wm->wm[2].ignore_lines), new_wm->wm[2].lines,
				    enast(new_wm->wm[3].ignore_lines), new_wm->wm[3].lines,
				    enast(new_wm->wm[4].ignore_lines), new_wm->wm[4].lines,
				    enast(new_wm->wm[5].ignore_lines), new_wm->wm[5].lines,
				    enast(new_wm->wm[6].ignore_lines), new_wm->wm[6].lines,
				    enast(new_wm->wm[7].ignore_lines), new_wm->wm[7].lines,
				    enast(new_wm->trans_wm.ignore_lines), new_wm->trans_wm.lines,
				    enast(new_wm->sagv.wm0.ignore_lines), new_wm->sagv.wm0.lines,
				    enast(new_wm->sagv.trans_wm.ignore_lines), new_wm->sagv.trans_wm.lines);

			drm_dbg_kms(&dev_priv->drm,
				    "[PLANE:%d:%s]  blocks %4d,%4d,%4d,%4d,%4d,%4d,%4d,%4d,%4d,%4d,%5d"
				    " -> %4d,%4d,%4d,%4d,%4d,%4d,%4d,%4d,%4d,%4d,%5d\n",
				    plane->base.base.id, plane->base.name,
				    old_wm->wm[0].blocks, old_wm->wm[1].blocks,
				    old_wm->wm[2].blocks, old_wm->wm[3].blocks,
				    old_wm->wm[4].blocks, old_wm->wm[5].blocks,
				    old_wm->wm[6].blocks, old_wm->wm[7].blocks,
				    old_wm->trans_wm.blocks,
				    old_wm->sagv.wm0.blocks,
				    old_wm->sagv.trans_wm.blocks,
				    new_wm->wm[0].blocks, new_wm->wm[1].blocks,
				    new_wm->wm[2].blocks, new_wm->wm[3].blocks,
				    new_wm->wm[4].blocks, new_wm->wm[5].blocks,
				    new_wm->wm[6].blocks, new_wm->wm[7].blocks,
				    new_wm->trans_wm.blocks,
				    new_wm->sagv.wm0.blocks,
				    new_wm->sagv.trans_wm.blocks);

			drm_dbg_kms(&dev_priv->drm,
				    "[PLANE:%d:%s] min_ddb %4d,%4d,%4d,%4d,%4d,%4d,%4d,%4d,%4d,%4d,%5d"
				    " -> %4d,%4d,%4d,%4d,%4d,%4d,%4d,%4d,%4d,%4d,%5d\n",
				    plane->base.base.id, plane->base.name,
				    old_wm->wm[0].min_ddb_alloc, old_wm->wm[1].min_ddb_alloc,
				    old_wm->wm[2].min_ddb_alloc, old_wm->wm[3].min_ddb_alloc,
				    old_wm->wm[4].min_ddb_alloc, old_wm->wm[5].min_ddb_alloc,
				    old_wm->wm[6].min_ddb_alloc, old_wm->wm[7].min_ddb_alloc,
				    old_wm->trans_wm.min_ddb_alloc,
				    old_wm->sagv.wm0.min_ddb_alloc,
				    old_wm->sagv.trans_wm.min_ddb_alloc,
				    new_wm->wm[0].min_ddb_alloc, new_wm->wm[1].min_ddb_alloc,
				    new_wm->wm[2].min_ddb_alloc, new_wm->wm[3].min_ddb_alloc,
				    new_wm->wm[4].min_ddb_alloc, new_wm->wm[5].min_ddb_alloc,
				    new_wm->wm[6].min_ddb_alloc, new_wm->wm[7].min_ddb_alloc,
				    new_wm->trans_wm.min_ddb_alloc,
				    new_wm->sagv.wm0.min_ddb_alloc,
				    new_wm->sagv.trans_wm.min_ddb_alloc);
		}
	}
}

static bool skl_plane_selected_wm_equals(struct intel_plane *plane,
					 const struct skl_pipe_wm *old_pipe_wm,
					 const struct skl_pipe_wm *new_pipe_wm)
{
	struct drm_i915_private *i915 = to_i915(plane->base.dev);
	int level, max_level = ilk_wm_max_level(i915);

	for (level = 0; level <= max_level; level++) {
		/*
		 * We don't check uv_wm as the hardware doesn't actually
		 * use it. It only gets used for calculating the required
		 * ddb allocation.
		 */
		if (!skl_wm_level_equals(skl_plane_wm_level(old_pipe_wm, plane->id, level),
					 skl_plane_wm_level(new_pipe_wm, plane->id, level)))
			return false;
	}

	if (HAS_HW_SAGV_WM(i915)) {
		const struct skl_plane_wm *old_wm = &old_pipe_wm->planes[plane->id];
		const struct skl_plane_wm *new_wm = &new_pipe_wm->planes[plane->id];

		if (!skl_wm_level_equals(&old_wm->sagv.wm0, &new_wm->sagv.wm0) ||
		    !skl_wm_level_equals(&old_wm->sagv.trans_wm, &new_wm->sagv.trans_wm))
			return false;
	}

	return skl_wm_level_equals(skl_plane_trans_wm(old_pipe_wm, plane->id),
				   skl_plane_trans_wm(new_pipe_wm, plane->id));
}

/*
 * To make sure the cursor watermark registers are always consistent
 * with our computed state the following scenario needs special
 * treatment:
 *
 * 1. enable cursor
 * 2. move cursor entirely offscreen
 * 3. disable cursor
 *
 * Step 2. does call .disable_plane() but does not zero the watermarks
 * (since we consider an offscreen cursor still active for the purposes
 * of watermarks). Step 3. would not normally call .disable_plane()
 * because the actual plane visibility isn't changing, and we don't
 * deallocate the cursor ddb until the pipe gets disabled. So we must
 * force step 3. to call .disable_plane() to update the watermark
 * registers properly.
 *
 * Other planes do not suffer from this issues as their watermarks are
 * calculated based on the actual plane visibility. The only time this
 * can trigger for the other planes is during the initial readout as the
 * default value of the watermarks registers is not zero.
 */
static int skl_wm_add_affected_planes(struct intel_atomic_state *state,
				      struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	const struct intel_crtc_state *old_crtc_state =
		intel_atomic_get_old_crtc_state(state, crtc);
	struct intel_crtc_state *new_crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	struct intel_plane *plane;

	for_each_intel_plane_on_crtc(&dev_priv->drm, crtc, plane) {
		struct intel_plane_state *plane_state;
		enum plane_id plane_id = plane->id;

		/*
		 * Force a full wm update for every plane on modeset.
		 * Required because the reset value of the wm registers
		 * is non-zero, whereas we want all disabled planes to
		 * have zero watermarks. So if we turn off the relevant
		 * power well the hardware state will go out of sync
		 * with the software state.
		 */
		if (!drm_atomic_crtc_needs_modeset(&new_crtc_state->uapi) &&
		    skl_plane_selected_wm_equals(plane,
						 &old_crtc_state->wm.skl.optimal,
						 &new_crtc_state->wm.skl.optimal))
			continue;

		plane_state = intel_atomic_get_plane_state(state, plane);
		if (IS_ERR(plane_state))
			return PTR_ERR(plane_state);

		new_crtc_state->update_planes |= BIT(plane_id);
	}

	return 0;
}

static int
skl_compute_wm(struct intel_atomic_state *state)
{
	struct intel_crtc *crtc;
	struct intel_crtc_state *new_crtc_state;
	int ret, i;

	for_each_new_intel_crtc_in_state(state, crtc, new_crtc_state, i) {
		ret = skl_build_pipe_wm(state, crtc);
		if (ret)
			return ret;
	}

	ret = skl_compute_ddb(state);
	if (ret)
		return ret;

	ret = intel_compute_sagv_mask(state);
	if (ret)
		return ret;

	/*
	 * skl_compute_ddb() will have adjusted the final watermarks
	 * based on how much ddb is available. Now we can actually
	 * check if the final watermarks changed.
	 */
	for_each_new_intel_crtc_in_state(state, crtc, new_crtc_state, i) {
		ret = skl_wm_add_affected_planes(state, crtc);
		if (ret)
			return ret;
	}

	skl_print_wm_changes(state);

	return 0;
}

static void skl_wm_level_from_reg_val(u32 val, struct skl_wm_level *level)
{
	level->enable = val & PLANE_WM_EN;
	level->ignore_lines = val & PLANE_WM_IGNORE_LINES;
	level->blocks = REG_FIELD_GET(PLANE_WM_BLOCKS_MASK, val);
	level->lines = REG_FIELD_GET(PLANE_WM_LINES_MASK, val);
}

static void skl_pipe_wm_get_hw_state(struct intel_crtc *crtc,
				     struct skl_pipe_wm *out)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum pipe pipe = crtc->pipe;
	int level, max_level;
	enum plane_id plane_id;
	u32 val;

	max_level = ilk_wm_max_level(dev_priv);

	for_each_plane_id_on_crtc(crtc, plane_id) {
		struct skl_plane_wm *wm = &out->planes[plane_id];

		for (level = 0; level <= max_level; level++) {
			if (plane_id != PLANE_CURSOR)
				val = intel_uncore_read(&dev_priv->uncore, PLANE_WM(pipe, plane_id, level));
			else
				val = intel_uncore_read(&dev_priv->uncore, CUR_WM(pipe, level));

			skl_wm_level_from_reg_val(val, &wm->wm[level]);
		}

		if (plane_id != PLANE_CURSOR)
			val = intel_uncore_read(&dev_priv->uncore, PLANE_WM_TRANS(pipe, plane_id));
		else
			val = intel_uncore_read(&dev_priv->uncore, CUR_WM_TRANS(pipe));

		skl_wm_level_from_reg_val(val, &wm->trans_wm);

		if (HAS_HW_SAGV_WM(dev_priv)) {
			if (plane_id != PLANE_CURSOR)
				val = intel_uncore_read(&dev_priv->uncore,
							PLANE_WM_SAGV(pipe, plane_id));
			else
				val = intel_uncore_read(&dev_priv->uncore,
							CUR_WM_SAGV(pipe));

			skl_wm_level_from_reg_val(val, &wm->sagv.wm0);

			if (plane_id != PLANE_CURSOR)
				val = intel_uncore_read(&dev_priv->uncore,
							PLANE_WM_SAGV_TRANS(pipe, plane_id));
			else
				val = intel_uncore_read(&dev_priv->uncore,
							CUR_WM_SAGV_TRANS(pipe));

			skl_wm_level_from_reg_val(val, &wm->sagv.trans_wm);
		} else {
			wm->sagv.wm0 = wm->wm[0];
			wm->sagv.trans_wm = wm->trans_wm;
		}
	}
}

void skl_wm_get_hw_state(struct drm_i915_private *dev_priv)
{
	struct intel_dbuf_state *dbuf_state =
		to_intel_dbuf_state(dev_priv->dbuf.obj.state);
	struct intel_crtc *crtc;

	if (HAS_MBUS_JOINING(dev_priv))
		dbuf_state->joined_mbus = intel_de_read(dev_priv, MBUS_CTL) & MBUS_JOIN;

	for_each_intel_crtc(&dev_priv->drm, crtc) {
		struct intel_crtc_state *crtc_state =
			to_intel_crtc_state(crtc->base.state);
		enum pipe pipe = crtc->pipe;
		unsigned int mbus_offset;
		enum plane_id plane_id;
		u8 slices;

		memset(&crtc_state->wm.skl.optimal, 0,
		       sizeof(crtc_state->wm.skl.optimal));
		if (crtc_state->hw.active)
			skl_pipe_wm_get_hw_state(crtc, &crtc_state->wm.skl.optimal);
		crtc_state->wm.skl.raw = crtc_state->wm.skl.optimal;

		memset(&dbuf_state->ddb[pipe], 0, sizeof(dbuf_state->ddb[pipe]));

		for_each_plane_id_on_crtc(crtc, plane_id) {
			struct skl_ddb_entry *ddb =
				&crtc_state->wm.skl.plane_ddb[plane_id];
			struct skl_ddb_entry *ddb_y =
				&crtc_state->wm.skl.plane_ddb_y[plane_id];

			if (!crtc_state->hw.active)
				continue;

			skl_ddb_get_hw_plane_state(dev_priv, crtc->pipe,
						   plane_id, ddb, ddb_y);

			skl_ddb_entry_union(&dbuf_state->ddb[pipe], ddb);
			skl_ddb_entry_union(&dbuf_state->ddb[pipe], ddb_y);
		}

		dbuf_state->weight[pipe] = intel_crtc_ddb_weight(crtc_state);

		/*
		 * Used for checking overlaps, so we need absolute
		 * offsets instead of MBUS relative offsets.
		 */
		slices = skl_compute_dbuf_slices(crtc, dbuf_state->active_pipes,
						 dbuf_state->joined_mbus);
		mbus_offset = mbus_ddb_offset(dev_priv, slices);
		crtc_state->wm.skl.ddb.start = mbus_offset + dbuf_state->ddb[pipe].start;
		crtc_state->wm.skl.ddb.end = mbus_offset + dbuf_state->ddb[pipe].end;

		/* The slices actually used by the planes on the pipe */
		dbuf_state->slices[pipe] =
			skl_ddb_dbuf_slice_mask(dev_priv, &crtc_state->wm.skl.ddb);

		drm_dbg_kms(&dev_priv->drm,
			    "[CRTC:%d:%s] dbuf slices 0x%x, ddb (%d - %d), active pipes 0x%x, mbus joined: %s\n",
			    crtc->base.base.id, crtc->base.name,
			    dbuf_state->slices[pipe], dbuf_state->ddb[pipe].start,
			    dbuf_state->ddb[pipe].end, dbuf_state->active_pipes,
			    str_yes_no(dbuf_state->joined_mbus));
	}

	dbuf_state->enabled_slices = dev_priv->dbuf.enabled_slices;
}

static bool skl_dbuf_is_misconfigured(struct drm_i915_private *i915)
{
	const struct intel_dbuf_state *dbuf_state =
		to_intel_dbuf_state(i915->dbuf.obj.state);
	struct skl_ddb_entry entries[I915_MAX_PIPES] = {};
	struct intel_crtc *crtc;

	for_each_intel_crtc(&i915->drm, crtc) {
		const struct intel_crtc_state *crtc_state =
			to_intel_crtc_state(crtc->base.state);

		entries[crtc->pipe] = crtc_state->wm.skl.ddb;
	}

	for_each_intel_crtc(&i915->drm, crtc) {
		const struct intel_crtc_state *crtc_state =
			to_intel_crtc_state(crtc->base.state);
		u8 slices;

		slices = skl_compute_dbuf_slices(crtc, dbuf_state->active_pipes,
						 dbuf_state->joined_mbus);
		if (dbuf_state->slices[crtc->pipe] & ~slices)
			return true;

		if (skl_ddb_allocation_overlaps(&crtc_state->wm.skl.ddb, entries,
						I915_MAX_PIPES, crtc->pipe))
			return true;
	}

	return false;
}

void skl_wm_sanitize(struct drm_i915_private *i915)
{
	struct intel_crtc *crtc;

	/*
	 * On TGL/RKL (at least) the BIOS likes to assign the planes
	 * to the wrong DBUF slices. This will cause an infinite loop
	 * in skl_commit_modeset_enables() as it can't find a way to
	 * transition between the old bogus DBUF layout to the new
	 * proper DBUF layout without DBUF allocation overlaps between
	 * the planes (which cannot be allowed or else the hardware
	 * may hang). If we detect a bogus DBUF layout just turn off
	 * all the planes so that skl_commit_modeset_enables() can
	 * simply ignore them.
	 */
	if (!skl_dbuf_is_misconfigured(i915))
		return;

	drm_dbg_kms(&i915->drm, "BIOS has misprogrammed the DBUF, disabling all planes\n");

	for_each_intel_crtc(&i915->drm, crtc) {
		struct intel_plane *plane = to_intel_plane(crtc->base.primary);
		const struct intel_plane_state *plane_state =
			to_intel_plane_state(plane->base.state);
		struct intel_crtc_state *crtc_state =
			to_intel_crtc_state(crtc->base.state);

		if (plane_state->uapi.visible)
			intel_plane_disable_noatomic(crtc, plane);

		drm_WARN_ON(&i915->drm, crtc_state->active_planes != 0);

		memset(&crtc_state->wm.skl.ddb, 0, sizeof(crtc_state->wm.skl.ddb));
	}
}

void intel_wm_state_verify(struct intel_crtc *crtc,
			   struct intel_crtc_state *new_crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	struct skl_hw_state {
		struct skl_ddb_entry ddb[I915_MAX_PLANES];
		struct skl_ddb_entry ddb_y[I915_MAX_PLANES];
		struct skl_pipe_wm wm;
	} *hw;
	const struct skl_pipe_wm *sw_wm = &new_crtc_state->wm.skl.optimal;
	int level, max_level = ilk_wm_max_level(dev_priv);
	struct intel_plane *plane;
	u8 hw_enabled_slices;

	if (!new_crtc_state->hw.active)
		return;

	hw = kzalloc(sizeof(*hw), GFP_KERNEL);
	if (!hw)
		return;

	skl_pipe_wm_get_hw_state(crtc, &hw->wm);

	skl_pipe_ddb_get_hw_state(crtc, hw->ddb, hw->ddb_y);

	hw_enabled_slices = intel_enabled_dbuf_slices_mask(dev_priv);

	if (hw_enabled_slices != dev_priv->dbuf.enabled_slices)
		drm_err(&dev_priv->drm,
			"mismatch in DBUF Slices (expected 0x%x, got 0x%x)\n",
			dev_priv->dbuf.enabled_slices,
			hw_enabled_slices);

	for_each_intel_plane_on_crtc(&dev_priv->drm, crtc, plane) {
		const struct skl_ddb_entry *hw_ddb_entry, *sw_ddb_entry;
		const struct skl_wm_level *hw_wm_level, *sw_wm_level;

		/* Watermarks */
		for (level = 0; level <= max_level; level++) {
			hw_wm_level = &hw->wm.planes[plane->id].wm[level];
			sw_wm_level = skl_plane_wm_level(sw_wm, plane->id, level);

			if (skl_wm_level_equals(hw_wm_level, sw_wm_level))
				continue;

			drm_err(&dev_priv->drm,
				"[PLANE:%d:%s] mismatch in WM%d (expected e=%d b=%u l=%u, got e=%d b=%u l=%u)\n",
				plane->base.base.id, plane->base.name, level,
				sw_wm_level->enable,
				sw_wm_level->blocks,
				sw_wm_level->lines,
				hw_wm_level->enable,
				hw_wm_level->blocks,
				hw_wm_level->lines);
		}

		hw_wm_level = &hw->wm.planes[plane->id].trans_wm;
		sw_wm_level = skl_plane_trans_wm(sw_wm, plane->id);

		if (!skl_wm_level_equals(hw_wm_level, sw_wm_level)) {
			drm_err(&dev_priv->drm,
				"[PLANE:%d:%s] mismatch in trans WM (expected e=%d b=%u l=%u, got e=%d b=%u l=%u)\n",
				plane->base.base.id, plane->base.name,
				sw_wm_level->enable,
				sw_wm_level->blocks,
				sw_wm_level->lines,
				hw_wm_level->enable,
				hw_wm_level->blocks,
				hw_wm_level->lines);
		}

		hw_wm_level = &hw->wm.planes[plane->id].sagv.wm0;
		sw_wm_level = &sw_wm->planes[plane->id].sagv.wm0;

		if (HAS_HW_SAGV_WM(dev_priv) &&
		    !skl_wm_level_equals(hw_wm_level, sw_wm_level)) {
			drm_err(&dev_priv->drm,
				"[PLANE:%d:%s] mismatch in SAGV WM (expected e=%d b=%u l=%u, got e=%d b=%u l=%u)\n",
				plane->base.base.id, plane->base.name,
				sw_wm_level->enable,
				sw_wm_level->blocks,
				sw_wm_level->lines,
				hw_wm_level->enable,
				hw_wm_level->blocks,
				hw_wm_level->lines);
		}

		hw_wm_level = &hw->wm.planes[plane->id].sagv.trans_wm;
		sw_wm_level = &sw_wm->planes[plane->id].sagv.trans_wm;

		if (HAS_HW_SAGV_WM(dev_priv) &&
		    !skl_wm_level_equals(hw_wm_level, sw_wm_level)) {
			drm_err(&dev_priv->drm,
				"[PLANE:%d:%s] mismatch in SAGV trans WM (expected e=%d b=%u l=%u, got e=%d b=%u l=%u)\n",
				plane->base.base.id, plane->base.name,
				sw_wm_level->enable,
				sw_wm_level->blocks,
				sw_wm_level->lines,
				hw_wm_level->enable,
				hw_wm_level->blocks,
				hw_wm_level->lines);
		}

		/* DDB */
		hw_ddb_entry = &hw->ddb[PLANE_CURSOR];
		sw_ddb_entry = &new_crtc_state->wm.skl.plane_ddb[PLANE_CURSOR];

		if (!skl_ddb_entry_equal(hw_ddb_entry, sw_ddb_entry)) {
			drm_err(&dev_priv->drm,
				"[PLANE:%d:%s] mismatch in DDB (expected (%u,%u), found (%u,%u))\n",
				plane->base.base.id, plane->base.name,
				sw_ddb_entry->start, sw_ddb_entry->end,
				hw_ddb_entry->start, hw_ddb_entry->end);
		}
	}

	kfree(hw);
}

static struct intel_global_state *intel_pmdemand_duplicate_state(struct intel_global_obj *obj)
{
	struct intel_pmdemand_state *pmdmnd_state;

	pmdmnd_state = kmemdup(obj->state, sizeof(*pmdmnd_state), GFP_KERNEL);
	if (!pmdmnd_state)
		return NULL;

	return &pmdmnd_state->base;
}

static void intel_pmdemand_destroy_state(struct intel_global_obj *obj,
					 struct intel_global_state *state)
{
	kfree(state);
}

static const struct intel_global_state_funcs intel_pmdemand_funcs = {
	.atomic_duplicate_state = intel_pmdemand_duplicate_state,
	.atomic_destroy_state = intel_pmdemand_destroy_state,
};

struct intel_pmdemand_state *
intel_atomic_get_pmdemand_state(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	struct intel_global_state *pmdemand_state;

	pmdemand_state = intel_atomic_get_global_obj_state(state, &dev_priv->pmdemand.obj);
	if (IS_ERR(pmdemand_state))
		return ERR_CAST(pmdemand_state);

	return to_intel_pmdemand_state(pmdemand_state);
}

int intel_pmdemand_init(struct drm_i915_private *dev_priv)
{
	struct intel_pmdemand_state *pmdemand_state;

	pmdemand_state = kzalloc(sizeof(*pmdemand_state), GFP_KERNEL);
	if (!pmdemand_state)
		return -ENOMEM;

	intel_atomic_global_obj_init(dev_priv, &dev_priv->pmdemand.obj,
				     &pmdemand_state->base, &intel_pmdemand_funcs);

	return 0;
}

void intel_init_pmdemand(struct drm_i915_private *dev_priv)
{
	mutex_init(&dev_priv->pmdemand.lock);
	init_waitqueue_head(&dev_priv->pmdemand.waitqueue);
}

int intel_pmdemand_atomic_check(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	struct intel_pmdemand_state *new_pmdemand_state = NULL;
	struct intel_crtc_state *old_crtc_state, *new_crtc_state;
	struct intel_crtc *crtc;
	struct intel_encoder *encoder;
	struct intel_bw_state *new_bw_state;
	const struct intel_dbuf_state *new_dbuf_state;
	const struct intel_cdclk_state *new_cdclk_state;
	int port_clock = 0;
	unsigned int data_rate;
	enum phy phy;
	int i, ret;

	if (DISPLAY_VER(dev_priv) < 14)
		return 0;

	new_pmdemand_state = intel_atomic_get_pmdemand_state(state);
	if (IS_ERR(new_pmdemand_state))
		return PTR_ERR(new_pmdemand_state);

	ret = intel_atomic_lock_global_state(&new_pmdemand_state->base);
	if (ret)
		return ret;

	/* Punit figures out the voltage index based on bandwidth*/
	new_bw_state = intel_atomic_get_bw_state(state);
	if (IS_ERR(new_bw_state))
		return PTR_ERR(new_bw_state);

	/* firmware will calculate the qclck_gc_index, requirement is set to 0 */
	new_pmdemand_state->qclk_gv_index = 0;

	data_rate = intel_bw_data_rate(dev_priv, new_bw_state);
	/* To MBs then to multiples of 100MBs */
	data_rate = DIV_ROUND_UP(data_rate, 1000);
	data_rate = DIV_ROUND_UP(data_rate, 100);
	new_pmdemand_state->qclk_gv_bw = data_rate;

	new_dbuf_state = intel_atomic_get_dbuf_state(state);
	if (IS_ERR(new_dbuf_state))
		return PTR_ERR(new_dbuf_state);

	i = hweight8(new_dbuf_state->active_pipes);
	new_pmdemand_state->active_pipes = min(i, 3);

	new_cdclk_state = intel_atomic_get_cdclk_state(state);
	if (IS_ERR(new_cdclk_state))
		return PTR_ERR(new_cdclk_state);

	new_pmdemand_state->voltage_index = new_cdclk_state->logical.voltage_level;
	/* KHz to MHz */
	new_pmdemand_state->cdclk_freq_mhz = DIV_ROUND_UP(new_cdclk_state->logical.cdclk, 1000);

	new_pmdemand_state->active_phys_plls_mask = 0;

	for_each_oldnew_intel_crtc_in_state(state, crtc, old_crtc_state, new_crtc_state, i) {
		if (!new_crtc_state->hw.active)
			continue;

		encoder = intel_get_crtc_new_encoder(state, new_crtc_state);
		if (!encoder)
			continue;

		phy = intel_port_to_phy(dev_priv, encoder->port);

		if (intel_is_c10phy(dev_priv, phy))
			new_pmdemand_state->active_phys_plls_mask |= BIT(phy);

		port_clock = max(port_clock, new_crtc_state->port_clock);
	}

	/* To MHz */
	new_pmdemand_state->ddiclk_freq_mhz = DIV_ROUND_UP(port_clock, 1000);

	/*
	 * Setting scalers to max as it can not be calculated during flips and
	 * fastsets without taking global states locks.
	 */
	new_pmdemand_state->scalers = 7;

	return 0;
}

static bool intel_pmdemand_check_prev_transaction(struct drm_i915_private *dev_priv)
{
	return !((intel_de_read(dev_priv, XELPDP_INITIATE_PMDEMAND_REQUEST(1)) & XELPDP_PMDEMAND_REQ_ENABLE) ||
		(intel_de_read(dev_priv, GEN12_DCPR_STATUS_1) & XELPDP_PMDEMAND_INFLIGHT_STATUS));
}

static bool intel_pmdemand_req_complete(struct drm_i915_private *dev_priv)
{
	return !(intel_de_read(dev_priv, XELPDP_INITIATE_PMDEMAND_REQUEST(1)) & XELPDP_PMDEMAND_REQ_ENABLE);
}

static int intel_pmdemand_wait(struct drm_i915_private *dev_priv)
{
	DEFINE_WAIT(wait);
	int ret;
	const unsigned int timeout_ms = 10;

	add_wait_queue(&dev_priv->pmdemand.waitqueue, &wait);

	ret = wait_event_timeout(dev_priv->pmdemand.waitqueue,
				 intel_pmdemand_req_complete(dev_priv),
				 msecs_to_jiffies_timeout(timeout_ms));
	if (ret < 0)
		drm_err(&dev_priv->drm,
			"timed out waiting for Punit PM Demand Response\n");

	remove_wait_queue(&dev_priv->pmdemand.waitqueue, &wait);

	return ret;
}

/* Required to be programmed during Display Init Sequences. */
void intel_program_dbuf_pmdemand(struct drm_i915_private *dev_priv,
				 u8 dbuf_slices)
{
	mutex_lock(&dev_priv->pmdemand.lock);
	if (drm_WARN_ON(&dev_priv->drm,
			!intel_pmdemand_check_prev_transaction(dev_priv)))
		goto unlock;

	intel_de_rmw(dev_priv, XELPDP_INITIATE_PMDEMAND_REQUEST(0),
		     XELPDP_PMDEMAND_DBUFS_MASK,
		     XELPDP_PMDEMAND_DBUFS(hweight32(dbuf_slices)));
	intel_de_rmw(dev_priv, XELPDP_INITIATE_PMDEMAND_REQUEST(1), 0,
		     XELPDP_PMDEMAND_REQ_ENABLE);

	intel_pmdemand_wait(dev_priv);
unlock:
	mutex_unlock(&dev_priv->pmdemand.lock);
}

static void intel_program_pmdemand(struct drm_i915_private *dev_priv,
				   const struct intel_pmdemand_state *new,
				   const struct intel_pmdemand_state *old)
{
	u32 val, tmp;

#define UPDATE_PMDEMAND_VAL(val, F, f) do {            \
	val &= (~(XELPDP_PMDEMAND_##F##_MASK));         \
	val |= (XELPDP_PMDEMAND_##F((u32)(old ? max(old->f, new->f) : new->f))); \
} while (0)

	mutex_lock(&dev_priv->pmdemand.lock);
	if (drm_WARN_ON(&dev_priv->drm,
			!intel_pmdemand_check_prev_transaction(dev_priv)))
		goto unlock;

	/*
	 * TODO: Update programming PM Demand for
	 * PHYS, PLLS, DDI_CLKFREQ, SCALARS
	 */
	val = intel_de_read(dev_priv, XELPDP_INITIATE_PMDEMAND_REQUEST(0));
	UPDATE_PMDEMAND_VAL(val, QCLK_GV_INDEX, qclk_gv_index);
	UPDATE_PMDEMAND_VAL(val, QCLK_GV_BW, qclk_gv_bw);
	UPDATE_PMDEMAND_VAL(val, VOLTAGE_INDEX, voltage_index);
	UPDATE_PMDEMAND_VAL(val, PIPES, active_pipes);
	UPDATE_PMDEMAND_VAL(val, DBUFS, dbufs);
	tmp = hweight32(new->active_phys_plls_mask);
	if (old)
		tmp = max(tmp, hweight32(old->active_phys_plls_mask));
	val |= XELPDP_PMDEMAND_PHYS(tmp);

	intel_de_write(dev_priv, XELPDP_INITIATE_PMDEMAND_REQUEST(0), val);

	val = intel_de_read(dev_priv, XELPDP_INITIATE_PMDEMAND_REQUEST(1));
	UPDATE_PMDEMAND_VAL(val, CDCLK_FREQ, cdclk_freq_mhz);
	UPDATE_PMDEMAND_VAL(val, DDICLK_FREQ, ddiclk_freq_mhz);
	UPDATE_PMDEMAND_VAL(val, SCALERS, scalers);
	/*
	 * Active_PLLs starts with 1 because of CDCLK PLL.
	 * TODO: Missing to account genlock filter when it gets used.
	 */
	val |= XELPDP_PMDEMAND_PLLS(tmp + 1);

	intel_de_write(dev_priv, XELPDP_INITIATE_PMDEMAND_REQUEST(1), val);

#undef UPDATE_PM_DEMAND_VAL

	intel_de_rmw(dev_priv, XELPDP_INITIATE_PMDEMAND_REQUEST(1), 0, XELPDP_PMDEMAND_REQ_ENABLE);

	intel_pmdemand_wait(dev_priv);
unlock:
	mutex_unlock(&dev_priv->pmdemand.lock);
}

void intel_pmdemand_pre_plane_update(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	const struct intel_pmdemand_state *new_pmdmnd_state =
		intel_atomic_get_new_pmdemand_state(state);
	const struct intel_pmdemand_state *old_pmdmnd_state =
		intel_atomic_get_old_pmdemand_state(state);

	if (DISPLAY_VER(dev_priv) < 14)
		return;

	if (!new_pmdmnd_state ||
	    memcmp(new_pmdmnd_state, old_pmdmnd_state, sizeof(*new_pmdmnd_state)) == 0)
		return;

	intel_program_pmdemand(dev_priv, new_pmdmnd_state, old_pmdmnd_state);
}

void intel_pmdemand_post_plane_update(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	const struct intel_pmdemand_state *new_pmdmnd_state =
		intel_atomic_get_new_pmdemand_state(state);
	const struct intel_pmdemand_state *old_pmdmnd_state =
		intel_atomic_get_old_pmdemand_state(state);

	if (DISPLAY_VER(dev_priv) < 14)
		return;

	if (!new_pmdmnd_state ||
	    memcmp(new_pmdmnd_state, old_pmdmnd_state, sizeof(*new_pmdmnd_state)) == 0)
		return;

	intel_program_pmdemand(dev_priv, new_pmdmnd_state, NULL);
}
#endif

void intel_enable_ipc(struct drm_i915_private *dev_priv)
{
	u32 val;

	if (!HAS_IPC(dev_priv))
		return;

	val = intel_uncore_read(&dev_priv->uncore, DISP_ARB_CTL2);

	if (dev_priv->ipc_enabled)
		val |= DISP_IPC_ENABLE;
	else
		val &= ~DISP_IPC_ENABLE;

	intel_uncore_write(&dev_priv->uncore, DISP_ARB_CTL2, val);
}

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
static bool intel_can_enable_ipc(struct drm_i915_private *dev_priv)
{
	return true;
}
#else
static bool intel_can_enable_ipc(struct drm_i915_private *dev_priv) { return false; }
#endif

void intel_init_ipc(struct drm_i915_private *dev_priv)
{
	if (!HAS_IPC(dev_priv))
		return;

	dev_priv->ipc_enabled = intel_can_enable_ipc(dev_priv);

	intel_enable_ipc(dev_priv);
}

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
static void gen12lp_init_clock_gating(struct drm_i915_private *dev_priv)
{
	/* Wa_1409120013 */
	if (DISPLAY_VER(dev_priv) == 12)
		intel_uncore_write(&dev_priv->uncore, ILK_DPFC_CHICKEN(INTEL_FBC_A),
				   DPFC_CHICKEN_COMP_DUMMY_PIXEL);

	/* Wa_14013723622:tgl,rkl,dg1,adl-s */
	if (DISPLAY_VER(dev_priv) == 12)
		intel_uncore_rmw(&dev_priv->uncore, CLKREQ_POLICY,
				 CLKREQ_POLICY_MEM_UP_OVRD, 0);
}
#else
static void gen12lp_init_clock_gating(struct drm_i915_private *dev_priv) {}
#endif

static void adlp_init_clock_gating(struct drm_i915_private *dev_priv)
{
	gen12lp_init_clock_gating(dev_priv);

	/* Wa_22011091694:adlp */
	intel_de_rmw(dev_priv, GEN9_CLKGATE_DIS_5, 0, DPCE_GATING_DIS);

	/* Bspec/49189 Initialize Sequence */
	intel_de_rmw(dev_priv, GEN8_CHICKEN_DCPR_1, DDI_CLOCK_REG_ACCESS, 0);
}

static void dg2_init_clock_gating(struct drm_i915_private *i915)
{
	/* Wa_22010954014:dg2 */
	intel_uncore_rmw(&i915->uncore, XEHP_CLOCK_GATE_DIS, 0,
			 SGSI_SIDECLK_DIS);

	/*
	 * Wa_14010733611:dg2_g10
	 * Wa_22010146351:dg2_g10
	 */
	if (IS_DG2_GRAPHICS_STEP(i915, G10, STEP_A0, STEP_B0))
		intel_uncore_rmw(&i915->uncore, XEHP_CLOCK_GATE_DIS, 0,
				 SGR_DIS | SGGI_DIS);
}

static void pvc_init_clock_gating(struct drm_i915_private *dev_priv)
{
	/* Wa_14012385139:pvc */
	if (IS_PVC_BD_STEP(dev_priv, STEP_A0, STEP_B0))
		intel_uncore_rmw(&dev_priv->uncore, XEHP_CLOCK_GATE_DIS, 0, SGR_DIS);

	/* Wa_22010954014:pvc */
	if (IS_PVC_BD_STEP(dev_priv, STEP_A0, STEP_B0))
		intel_uncore_rmw(&dev_priv->uncore, XEHP_CLOCK_GATE_DIS, 0, SGSI_SIDECLK_DIS);
}

void intel_init_clock_gating(struct drm_i915_private *dev_priv)
{
	dev_priv->clock_gating_funcs->init_clock_gating(dev_priv);
}

void intel_suspend_hw(struct drm_i915_private *dev_priv)
{
}

static void nop_init_clock_gating(struct drm_i915_private *dev_priv)
{
	drm_dbg_kms(&dev_priv->drm,
		    "No clock gating settings or workarounds applied.\n");
}

#define CG_FUNCS(platform)						\
static const struct drm_i915_clock_gating_funcs platform##_clock_gating_funcs = { \
	.init_clock_gating = platform##_init_clock_gating,		\
}

CG_FUNCS(pvc);
CG_FUNCS(dg2);
CG_FUNCS(adlp);
CG_FUNCS(gen12lp);
CG_FUNCS(nop);
#undef CG_FUNCS

/**
 * intel_init_clock_gating_hooks - setup the clock gating hooks
 * @dev_priv: device private
 *
 * Setup the hooks that configure which clocks of a given platform can be
 * gated and also apply various GT and display specific workarounds for these
 * platforms. Note that some GT specific workarounds are applied separately
 * when GPU contexts or batchbuffers start their execution.
 */
void intel_init_clock_gating_hooks(struct drm_i915_private *dev_priv)
{
	if (IS_SRIOV_VF(dev_priv))
		dev_priv->clock_gating_funcs = &nop_clock_gating_funcs;
	else if (IS_PONTEVECCHIO(dev_priv))
		dev_priv->clock_gating_funcs = &pvc_clock_gating_funcs;
	else if (IS_DG2(dev_priv))
		dev_priv->clock_gating_funcs = &dg2_clock_gating_funcs;
	else if (IS_ALDERLAKE_P(dev_priv))
		dev_priv->clock_gating_funcs = &adlp_clock_gating_funcs;
	else if (GRAPHICS_VER(dev_priv) == 12)
		dev_priv->clock_gating_funcs = &gen12lp_clock_gating_funcs;
	else {
		MISSING_CASE(INTEL_DEVID(dev_priv));
		dev_priv->clock_gating_funcs = &nop_clock_gating_funcs;
	}
}

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
static const struct drm_i915_wm_disp_funcs skl_wm_funcs = {
	.compute_global_watermarks = skl_compute_wm,
};

static void display_intel_init_pm(struct drm_i915_private *dev_priv)
{
	/* For FIFO watermark updates */
	skl_setup_wm_latency(dev_priv);
	dev_priv->wm_disp = &skl_wm_funcs;
}
#else
static void display_intel_init_pm(struct drm_i915_private *dev_priv) {}
#endif

/* Set up chip specific power management-related functions */
void intel_init_pm(struct drm_i915_private *dev_priv)
{
#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
	if (IS_SRIOV_VF(dev_priv)) {
		/* XXX */
		dev_priv->wm_disp = &skl_wm_funcs;
		return;
	}
#endif

	intel_sagv_init(dev_priv);

	display_intel_init_pm(dev_priv);
}

void intel_pm_setup(struct drm_i915_private *dev_priv)
{
	dev_priv->runtime_pm.suspended = false;
	atomic_set(&dev_priv->runtime_pm.wakeref_count, 0);
}

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
static struct intel_global_state *intel_dbuf_duplicate_state(struct intel_global_obj *obj)
{
	struct intel_dbuf_state *dbuf_state;

	dbuf_state = kmemdup(obj->state, sizeof(*dbuf_state), GFP_KERNEL);
	if (!dbuf_state)
		return NULL;

	return &dbuf_state->base;
}

static void intel_dbuf_destroy_state(struct intel_global_obj *obj,
				     struct intel_global_state *state)
{
	kfree(state);
}

static const struct intel_global_state_funcs intel_dbuf_funcs = {
	.atomic_duplicate_state = intel_dbuf_duplicate_state,
	.atomic_destroy_state = intel_dbuf_destroy_state,
};

struct intel_dbuf_state *
intel_atomic_get_dbuf_state(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	struct intel_global_state *dbuf_state;

	dbuf_state = intel_atomic_get_global_obj_state(state, &dev_priv->dbuf.obj);
	if (IS_ERR(dbuf_state))
		return ERR_CAST(dbuf_state);

	return to_intel_dbuf_state(dbuf_state);
}

int intel_dbuf_init(struct drm_i915_private *dev_priv)
{
	struct intel_dbuf_state *dbuf_state;

	dbuf_state = kzalloc(sizeof(*dbuf_state), GFP_KERNEL);
	if (!dbuf_state)
		return -ENOMEM;

	intel_atomic_global_obj_init(dev_priv, &dev_priv->dbuf.obj,
				     &dbuf_state->base, &intel_dbuf_funcs);

	return 0;
}

/*
 * Configure MBUS_CTL and all DBUF_CTL_S of each slice to join_mbus state before
 * update the request state of all DBUS slices.
 */
static void update_mbus_pre_enable(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	u32 mbus_ctl, dbuf_min_tracker_val;
	enum dbuf_slice slice;
	const struct intel_dbuf_state *dbuf_state =
		intel_atomic_get_new_dbuf_state(state);

	if (!HAS_MBUS_JOINING(dev_priv))
		return;

	/*
	 * TODO: Implement vblank synchronized MBUS joining changes.
	 * Must be properly coordinated with dbuf reprogramming.
	 */
	if (dbuf_state->joined_mbus) {
		mbus_ctl = MBUS_HASHING_MODE_1x4 | MBUS_JOIN |
			MBUS_JOIN_PIPE_SELECT_NONE;
		dbuf_min_tracker_val = DBUF_MIN_TRACKER_STATE_SERVICE(3);
	} else {
		mbus_ctl = MBUS_HASHING_MODE_2x2 |
			MBUS_JOIN_PIPE_SELECT_NONE;
		dbuf_min_tracker_val = DBUF_MIN_TRACKER_STATE_SERVICE(1);
	}

	intel_de_rmw(dev_priv, MBUS_CTL,
		     MBUS_HASHING_MODE_MASK | MBUS_JOIN |
		     MBUS_JOIN_PIPE_SELECT_MASK, mbus_ctl);

	for_each_dbuf_slice(dev_priv, slice)
		intel_de_rmw(dev_priv, DBUF_CTL_S(slice),
			     DBUF_MIN_TRACKER_STATE_SERVICE_MASK,
			     dbuf_min_tracker_val);
}

void intel_dbuf_pre_plane_update(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	const struct intel_dbuf_state *new_dbuf_state =
		intel_atomic_get_new_dbuf_state(state);
	const struct intel_dbuf_state *old_dbuf_state =
		intel_atomic_get_old_dbuf_state(state);

	if (!new_dbuf_state ||
	    ((new_dbuf_state->enabled_slices == old_dbuf_state->enabled_slices)
	    && (new_dbuf_state->joined_mbus == old_dbuf_state->joined_mbus)))
		return;

	WARN_ON(!new_dbuf_state->base.changed);

	update_mbus_pre_enable(state);
	gen9_dbuf_slices_update(dev_priv,
				old_dbuf_state->enabled_slices |
				new_dbuf_state->enabled_slices);
}

void intel_dbuf_post_plane_update(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	const struct intel_dbuf_state *new_dbuf_state =
		intel_atomic_get_new_dbuf_state(state);
	const struct intel_dbuf_state *old_dbuf_state =
		intel_atomic_get_old_dbuf_state(state);

	if (!new_dbuf_state ||
	    ((new_dbuf_state->enabled_slices == old_dbuf_state->enabled_slices)
	    && (new_dbuf_state->joined_mbus == old_dbuf_state->joined_mbus)))
		return;

	WARN_ON(!new_dbuf_state->base.changed);

	gen9_dbuf_slices_update(dev_priv,
				new_dbuf_state->enabled_slices);
}

static bool xelpdp_is_one_pipe_per_dbuf_bank(enum pipe pipe, u8 active_pipes)
{
	switch (pipe) {
	case PIPE_A:
	case PIPE_D:
		if (is_power_of_2(active_pipes & (BIT(PIPE_A) | BIT(PIPE_D))))
			return true;
		break;
	case PIPE_B:
	case PIPE_C:
		if (is_power_of_2(active_pipes & (BIT(PIPE_B) | BIT(PIPE_C))))
			return true;
		break;
	default: /* to suppress compiler warning */
		MISSING_CASE(pipe);
		break;
	}

	return false;
}

void intel_mbus_dbox_update(struct intel_atomic_state *state)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	const struct intel_dbuf_state *new_dbuf_state, *old_dbuf_state;
	const struct intel_crtc_state *new_crtc_state;
	const struct intel_crtc *crtc;
	u32 val = 0;
	int i;

	new_dbuf_state = intel_atomic_get_new_dbuf_state(state);
	old_dbuf_state = intel_atomic_get_old_dbuf_state(state);
	if (!new_dbuf_state ||
	    (new_dbuf_state->joined_mbus == old_dbuf_state->joined_mbus &&
	     new_dbuf_state->active_pipes == old_dbuf_state->active_pipes))
		return;

	if (DISPLAY_VER(i915) >= 14)
		val |= MBUS_DBOX_I_CREDIT(2);

	if (DISPLAY_VER(i915) >= 12) {
		val |= MBUS_DBOX_B2B_TRANSACTIONS_MAX(16);
		val |= MBUS_DBOX_B2B_TRANSACTIONS_DELAY(1);
		val |= MBUS_DBOX_REGULATE_B2B_TRANSACTIONS_EN;
	}

	if (DISPLAY_VER(i915) >= 14)
		val |= new_dbuf_state->joined_mbus ? MBUS_DBOX_A_CREDIT(12) :
						     MBUS_DBOX_A_CREDIT(8);
	else if (IS_ALDERLAKE_P(i915))
		/* Wa_22010947358:adl-p */
		val |= new_dbuf_state->joined_mbus ? MBUS_DBOX_A_CREDIT(6) :
						     MBUS_DBOX_A_CREDIT(4);
	else
		val |= MBUS_DBOX_A_CREDIT(2);

	if (DISPLAY_VER(i915) >= 14) {
		val |= MBUS_DBOX_B_CREDIT(0xA);
	} else if (IS_ALDERLAKE_P(i915)) {
		val |= MBUS_DBOX_BW_CREDIT(2);
		val |= MBUS_DBOX_B_CREDIT(8);
	} else if (DISPLAY_VER(i915) >= 12) {
		val |= MBUS_DBOX_BW_CREDIT(2);
		val |= MBUS_DBOX_B_CREDIT(12);
	} else {
		val |= MBUS_DBOX_BW_CREDIT(1);
		val |= MBUS_DBOX_B_CREDIT(8);
	}

	for_each_new_intel_crtc_in_state(state, crtc, new_crtc_state, i) {
		u32 pipe_val = val;

		if (!new_crtc_state->hw.active ||
		    !intel_crtc_needs_modeset(new_crtc_state))
			continue;

		if (DISPLAY_VER(i915) >= 14) {
			if (xelpdp_is_one_pipe_per_dbuf_bank(crtc->pipe,
							     new_dbuf_state->active_pipes))
				pipe_val |= MBUS_DBOX_BW_CREDIT(MBUS_DBOX_BW_8CREDITS_MTL);
			else
				pipe_val |= MBUS_DBOX_BW_CREDIT(MBUS_DBOX_BW_4CREDITS_MTL);
		}

		intel_de_write(i915, PIPE_MBUS_DBOX_CTL(crtc->pipe), pipe_val);
	}
}
#endif
