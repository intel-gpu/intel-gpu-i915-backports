/*
 * Copyright © 2015 Intel Corporation
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
 */

#ifndef _I915_PARAMS_H_
#define _I915_PARAMS_H_

#include <linux/bitops.h>
#include <linux/cache.h> /* for __read_mostly */

struct drm_printer;

#define ENABLE_GUC_SUBMISSION		BIT(0)
#define ENABLE_GUC_LOAD_HUC		BIT(1)
#define ENABLE_GUC_DO_NOT_LOAD_GUC	BIT(7)
#define ENABLE_GUC_MASK			GENMASK(1, 0)

/*
 * Invoke param, a function-like macro, for each i915 param, with arguments:
 *
 * param(type, name, value, mode)
 *
 * type: parameter type, one of {bool, int, unsigned int, unsigned long, char *}
 * name: name of the parameter
 * value: initial/default value of the parameter
 * mode: debugfs file permissions, one of {0400, 0600, 0}, use 0 to not create
 *       debugfs file
 */
#define I915_PARAMS_FOR_EACH(param) \
	param(char *, vbt_firmware, NULL, 0400) \
	param(int, modeset, -1, 0400) \
	param(int, panel_use_ssc, -1, 0600) \
	param(int, vbt_sdvo_panel_type, -1, 0400) \
	param(int, enable_dc, -1, 0400) \
	param(int, enable_fbc, -1, 0600) \
	param(int, enable_psr, -1, 0600) \
	param(int, enable_softpg, -1, 0400) \
	param(bool, psr_safest_params, false, 0400) \
	param(bool, enable_psr2_sel_fetch, true, 0400) \
	param(int, disable_power_well, -1, 0400) \
	param(int, invert_brightness, 0, 0600) \
	param(int, enable_guc, -1, 0400) \
	param(unsigned int, guc_feature_flags, 0, 0400) \
	param(int, guc_log_level, -1, 0400) \
	param(int, guc_log_size_crash, -1, 0400) \
	param(int, guc_log_size_debug, -1, 0400) \
	param(int, guc_log_size_capture, -1, 0400) \
	param(char *, guc_firmware_path, NULL, 0400) \
	param(char *, huc_firmware_path, NULL, 0400) \
	param(char *, dmc_firmware_path, NULL, 0400) \
	param(char *, gsc_firmware_path, NULL, 0400) \
	param(bool, memtest, false, 0400) \
	param(int, mmio_debug, -IS_ENABLED(CPTCFG_DRM_I915_DEBUG_MMIO), 0600) \
	param(char *, mocs_table_path, NULL, IS_ENABLED(CPTCFG_DRM_I915_DEBUG_MOCS) ? 0400 : 0) \
	param(int, edp_vswing, 0, 0400) \
	param(unsigned int, reset, 3, 0600) \
	param(int, inject_probe_failure, 0, 0) \
	param(unsigned int, debug_eu, 0, 0400) \
	param(unsigned int, debugger_timeout_ms, 3000, 0400) \
	param(int, debugger_log_level, -1, 0600) \
	param(unsigned int, ppgtt_size, 57, 0400) \
	param(int, fastboot, -1, 0600) \
	param(int, enable_dpcd_backlight, -1, 0600) \
	param(unsigned int, lmem_size, 0, 0400) \
	param(unsigned int, max_vfs, 0, 0400) \
	param(unsigned long, vfs_flr_mask, ~0, IS_ENABLED(CPTCFG_DRM_I915_DEBUG_IOV) ? 0600 : 0) \
	param(int, force_alloc_contig, 0, 0400) \
	param(unsigned int, page_sz_mask, 0, 0600) \
	param(unsigned int, debug_pages, 0, 0400) \
	param(unsigned int, prelim_override_p2p_dist, 0, 0400)	\
	param(unsigned int, guc_log_destination, 0, 0400) \
	param(unsigned int, ring_mask, (unsigned int)~0, 0400) \
	param(int, max_tiles, -1, 0400) \
	param(unsigned int, pvc_fw_put_delay_ms, CPTCFG_DRM_I915_PVC_FORCEWAKE_DELAY_MS, 0600) \
	/* leave bools at the end to not create holes */ \
	param(bool, enable_busy_v2, false, 0400) \
	param(bool, allow_non_persist_without_reset, false, 0400) \
	param(bool, enable_fake_int_wa, true, 0400) \
	param(bool, enable_full_ps64, true, 0400) \
	param(bool, enable_iaf, true, 0400) \
	param(bool, address_translation_services, false, IS_ENABLED(CONFIG_DRM_I915_ATS) ? 0400 : 0) \
	param(bool, enable_secure_batch, false, 0400) \
	param(bool, enable_rc6, true, 0400) \
	param(bool, enable_rps, true, 0400) \
	param(bool, force_host_pm, false, 0400) \
	param(bool, rc6_ignore_steppings, false, 0400) \
	param(bool, enable_hangcheck, true, 0600) \
	param(bool, error_capture, true, IS_ENABLED(CPTCFG_DRM_I915_CAPTURE_ERROR) ? 0600 : 0) \
	param(bool, disable_display, IS_ENABLED(CPTCFG_DRM_I915_DISPLAY) ? false : true, 0400) \
	param(int, force_driver_flr, -1, 0400) \
	param(bool, disable_bo_chunking, false, 0600) \
	param(bool, enable_force_miss_ftlb, true, 0600) \
	param(bool, engine_mocs_uncacheable, false, 0600) \
	param(bool, enable_resizeable_bar, true, 0400) \
	param(bool, enable_gsc, true, 0400) \
	param(bool, enable_pcode_handshake, true, 0400) \
	param(bool, enable_256B, true, 0400) \
	param(bool, enable_gt_reset, true, 0400) \
	param(bool, enable_spi, true, 0400)

#define MEMBER(T, member, ...) T member;
struct i915_params {
	I915_PARAMS_FOR_EACH(MEMBER);
};
#undef MEMBER

extern struct i915_params i915_modparams __read_mostly;

void i915_params_dump(const struct i915_params *params, struct drm_printer *p);
void i915_params_copy(struct i915_params *dest, const struct i915_params *src);
void i915_params_free(struct i915_params *params);

#endif
