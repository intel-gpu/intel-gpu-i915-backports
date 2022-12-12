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
	param(int, force_pch, -1, 0400) \
	param(int, lvds_channel_mode, 0, 0400) \
	param(int, panel_use_ssc, -1, 0600) \
	param(int, vbt_sdvo_panel_type, -1, 0400) \
	param(int, enable_dc, -1, 0400) \
	param(int, enable_fbc, -1, 0600) \
	param(int, enable_psr, -1, 0600) \
	param(bool, psr_safest_params, false, 0400) \
	param(bool, enable_psr2_sel_fetch, true, 0400) \
	param(int, disable_power_well, -1, 0400) \
	param(int, enable_ips, 1, 0600) \
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
	param(int, edp_vswing, 0, 0400) \
	param(unsigned int, reset, 3, 0600) \
	param(unsigned int, inject_probe_failure, 0, 0) \
	param(unsigned int, debug_eu, 0, 0400) \
	param(unsigned int, debugger_timeout_ms, 3000, 0400) \
	param(int, debugger_log_level, -1, 0600) \
	param(int, fastboot, -1, 0600) \
	param(int, enable_dpcd_backlight, -1, 0600) \
	param(char *, force_probe, CPTCFG_DRM_I915_FORCE_PROBE, 0400) \
	param(unsigned int, request_timeout_ms, CPTCFG_DRM_I915_REQUEST_TIMEOUT, CPTCFG_DRM_I915_REQUEST_TIMEOUT ? 0600 : 0) \
	param(unsigned int, lmem_size, 0, 0400) \
	param(unsigned int, enable_eviction, 3, 0600) \
	param(unsigned int, max_vfs, 0, 0400) \
	param(unsigned long, vfs_flr_mask, ~0, IS_ENABLED(CPTCFG_DRM_I915_DEBUG_IOV) ? 0600 : 0) \
	param(int, force_alloc_contig, 0, 0400) \
	param(int, smem_access_control, I915_SMEM_ACCESS_CONTROL_DEFAULT, 0600) \
	param(unsigned int, page_sz_mask, 0, 0600) \
	param(unsigned int, debug_pages, 0, 0400) \
	param(unsigned int, prelim_override_p2p_dist, 0, 0400)	\
	/* leave bools at the end to not create holes */ \
	param(bool, allow_non_persist_without_reset, false, 0400) \
	param(bool, enable_fake_int_wa, true, 0400) \
	param(bool, enable_pagefault, false, 0600) \
	param(bool, enable_iaf, true, 0400) \
	param(bool, enable_secure_batch, false, 0400) \
	param(bool, enable_hw_throttle_blt, false, 0400) \
	param(bool, enable_rc6, true, 0400) \
	param(bool, enable_stateless_mc, false, 0400) \
	param(bool, rc6_ignore_steppings, false, 0400) \
	param(bool, enable_hangcheck, true, 0600) \
	param(bool, load_detect_test, false, 0600) \
	param(bool, force_reset_modeset_test, false, 0600) \
	param(bool, error_capture, true, IS_ENABLED(CPTCFG_DRM_I915_CAPTURE_ERROR) ? 0600 : 0) \
	param(bool, async_vm_unbind, false, 0600) \
	param(bool, disable_display, false, 0400) \
	param(bool, verbose_state_checks, true, 0) \
	param(bool, nuclear_pageflip, false, 0400) \
	param(bool, enable_dp_mst, true, 0600) \
	param(bool, enable_gvt, false, IS_ENABLED(CPTCFG_DRM_I915_GVT) ? 0400 : 0) \
	param(bool, enable_non_private_objects, false, 0400) \
	param(bool, enable_mem_fence, false, 0400) \
	param(bool, ulls_bcs0_pm_wa, true, 0600) \
	param(int, force_driver_flr, -1, 0400)

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
