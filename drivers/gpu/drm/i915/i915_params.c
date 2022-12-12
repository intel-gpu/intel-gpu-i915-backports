/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <linux/string_helpers.h>

#include <drm/drm_print.h>

#include "i915_params.h"
#include "i915_drv.h"

#define i915_param_named(name, T, perm, desc) \
	module_param_named(name, i915_modparams.name, T, perm); \
	MODULE_PARM_DESC(name, desc)
#define i915_param_named_unsafe(name, T, perm, desc) \
	module_param_named_unsafe(name, i915_modparams.name, T, perm); \
	MODULE_PARM_DESC(name, desc)

struct i915_params i915_modparams __read_mostly = {
#define MEMBER(T, member, value, ...) .member = (value),
	I915_PARAMS_FOR_EACH(MEMBER)
#undef MEMBER
};

/*
 * Note: As a rule, keep module parameter sysfs permissions read-only
 * 0400. Runtime changes are only supported through i915 debugfs.
 *
 * For any exceptions requiring write access and runtime changes through module
 * parameter sysfs, prevent debugfs file creation by setting the parameter's
 * debugfs mode to 0.
 */

i915_param_named(modeset, int, 0400,
	"Use kernel modesetting [KMS] (0=disable, "
	"1=on, -1=force vga console preference [default])");

i915_param_named_unsafe(force_pch, int, 0400,
	"Force PCH type on boot (-1=auto detected by PCI ID, "
	"0=PCH_NONE, see enum intel_pch for additional values)");

i915_param_named_unsafe(enable_dc, int, 0400,
	"Enable power-saving display C-states. "
	"(-1=auto [default]; 0=disable; 1=up to DC5; 2=up to DC6; "
	"3=up to DC5 with DC3CO; 4=up to DC6 with DC3CO)");

i915_param_named_unsafe(enable_fbc, int, 0400,
	"Enable frame buffer compression for power savings "
	"(default: -1 (use per-chip default))");

i915_param_named_unsafe(lvds_channel_mode, int, 0400,
	 "Specify LVDS channel mode "
	 "(0=probe BIOS [default], 1=single-channel, 2=dual-channel)");

i915_param_named_unsafe(panel_use_ssc, int, 0400,
	"Use Spread Spectrum Clock with panels [LVDS/eDP] "
	"(default: auto from VBT)");

i915_param_named_unsafe(vbt_sdvo_panel_type, int, 0400,
	"Override/Ignore selection of SDVO panel mode in the VBT "
	"(-2=ignore, -1=auto [default], index in VBT BIOS table)");

i915_param_named_unsafe(reset, uint, 0400,
	"Attempt GPU resets (0=disabled, 1=full gpu reset, 2=engine reset [default])");

i915_param_named_unsafe(allow_non_persist_without_reset, bool, 0400,
	"Allow non-persistent contexts even if reset is disabled (default: false)");

i915_param_named_unsafe(vbt_firmware, charp, 0400,
	"Load VBT from specified file under /lib/firmware");

#if IS_ENABLED(CPTCFG_DRM_I915_CAPTURE_ERROR)
i915_param_named(error_capture, bool, 0400,
	"Record the GPU state following a hang. "
	"This information in /sys/class/drm/card<N>/error is vital for "
	"triaging and debugging hangs.");
#endif

i915_param_named(async_vm_unbind, bool, 0600,
	"Enable asynchronous vm_unbind. (default: false)");

i915_param_named_unsafe(enable_hangcheck, bool, 0400,
	"Periodically check GPU activity for detecting hangs. "
	"WARNING: Disabling this can cause system wide hangs. "
	"(default: true)");

i915_param_named_unsafe(enable_psr, int, 0400,
	"Enable PSR "
	"(0=disabled, 1=enable up to PSR1, 2=enable up to PSR2) "
	"Default: -1 (use per-chip default)");

i915_param_named(psr_safest_params, bool, 0400,
	"Replace PSR VBT parameters by the safest and not optimal ones. This "
	"is helpful to detect if PSR issues are related to bad values set in "
	" VBT. (0=use VBT parameters, 1=use safest parameters)");

i915_param_named_unsafe(enable_psr2_sel_fetch, bool, 0400,
	"Enable PSR2 selective fetch "
	"(0=disabled, 1=enabled) "
	"Default: 0");

i915_param_named_unsafe(force_probe, charp, 0400,
	"Force probe the driver for specified devices. "
	"See CPTCFG_DRM_I915_FORCE_PROBE for details.");

i915_param_named_unsafe(enable_secure_batch, bool, 0400,
	"Enable for legacy tests I915_EXEC_SECURE. (default: 0)");

i915_param_named_unsafe(disable_power_well, int, 0400,
	"Disable display power wells when possible "
	"(-1=auto [default], 0=power wells always on, 1=power wells disabled when possible)");

i915_param_named_unsafe(enable_ips, int, 0400, "Enable IPS (default: true)");

i915_param_named(fastboot, int, 0400,
	"Try to skip unnecessary mode sets at boot time "
	"(0=disabled, 1=enabled) "
	"Default: -1 (use per-chip default)");

i915_param_named_unsafe(load_detect_test, bool, 0400,
	"Force-enable the VGA load detect code for testing (default:false). "
	"For developers only.");

i915_param_named_unsafe(force_reset_modeset_test, bool, 0400,
	"Force a modeset during gpu reset for testing (default:false). "
	"For developers only.");

i915_param_named_unsafe(invert_brightness, int, 0400,
	"Invert backlight brightness "
	"(-1 force normal, 0 machine defaults, 1 force inversion), please "
	"contact your Intel support representative, if your machine needs it. "
	"It will then be included in an upcoming module version.");

i915_param_named(disable_display, bool, 0400,
	"Disable display (default: false)");

i915_param_named(memtest, bool, 0400,
	"Perform a read/write test of all device memory on module load (default: off)");

i915_param_named(mmio_debug, int, 0400,
	"Enable the MMIO debug code for the first N failures (default: off). "
	"This may negatively affect performance.");

/* Special case writable file */
i915_param_named(verbose_state_checks, bool, 0600,
	"Enable verbose logs (ie. WARN_ON()) in case of unexpected hw state conditions.");

i915_param_named_unsafe(nuclear_pageflip, bool, 0400,
	"Force enable atomic functionality on platforms that don't have full support yet.");

/* WA to get away with the default setting in VBT for early platforms.Will be removed */
i915_param_named_unsafe(edp_vswing, int, 0400,
	"Ignore/Override vswing pre-emph table selection from VBT "
	"(0=use value from vbt [default], 1=low power swing(200mV),"
	"2=default swing(400mV))");

i915_param_named_unsafe(enable_guc, int, 0400,
	"Enable GuC load for GuC submission and/or HuC load. "
	"Required functionality can be selected using bitmask values. "
	"(-1=auto [default], 0=disable, 1=GuC submission, 2=HuC load)");

i915_param_named_unsafe(guc_feature_flags, uint, 0400,
	"GuC feature flags. Requires GuC to be loaded. (0=none [default])");

i915_param_named(guc_log_level, int, 0400,
	"GuC firmware logging level. Requires GuC to be loaded. "
	"(-1=auto [default], 0=disable, 1..4=enable with verbosity min..max)");

i915_param_named(guc_log_size_crash, int, 0400,
	"GuC firmware logging buffer size for crash dumps (in MB)"
	"(-1=auto [default], NB: max = 4, other restrictions apply)");

i915_param_named(guc_log_size_debug, int, 0400,
	"GuC firmware logging buffer size for debug logs (in MB)"
	"(-1=auto [default], NB: max = 16, other restrictions apply)");

i915_param_named(guc_log_size_capture, int, 0400,
	"GuC error capture register dump buffer size (in MB)"
	"(-1=auto [default], NB: max = 4, other restrictions apply)");

i915_param_named_unsafe(guc_firmware_path, charp, 0400,
	"GuC firmware path to use instead of the default one");

i915_param_named_unsafe(huc_firmware_path, charp, 0400,
	"HuC firmware path to use instead of the default one");

i915_param_named_unsafe(dmc_firmware_path, charp, 0400,
	"DMC firmware path to use instead of the default one");

i915_param_named_unsafe(gsc_firmware_path, charp, 0400,
	"GSC firmware path to use instead of the default one");

i915_param_named_unsafe(enable_dp_mst, bool, 0400,
	"Enable multi-stream transport (MST) for new DisplayPort sinks. (default: true)");

#if IS_ENABLED(CPTCFG_DRM_I915_DEBUG)
i915_param_named_unsafe(inject_probe_failure, uint, 0400,
	"Force an error after a number of failure check points (0:disabled (default), N:force failure at the Nth failure check point)");
#endif

i915_param_named(enable_dpcd_backlight, int, 0400,
	"Enable support for DPCD backlight control"
	"(-1=use per-VBT LFP backlight type setting [default], 0=disabled, 1=enable, 2=force VESA interface, 3=force Intel interface)");

i915_param_named_unsafe(enable_rc6, bool, 0400,
	"Enable power-saving render C-state 6. (default: true)");
i915_param_named_unsafe(rc6_ignore_steppings, bool, 0400,
	"Allow RC6 to be enabled on steppings where it would be disabled. (default: false)");

i915_param_named_unsafe(enable_pagefault, bool, 0600,
	"Enable device page fault. (default: false)");

#if IS_ENABLED(CPTCFG_DRM_I915_GVT)
i915_param_named(enable_gvt, bool, 0400,
	"Enable support for Intel GVT-g graphics virtualization host support(default:false)");
#endif

#if CPTCFG_DRM_I915_REQUEST_TIMEOUT
i915_param_named_unsafe(request_timeout_ms, uint, 0600,
			"Default request/fence/batch buffer expiration timeout.");
#endif

i915_param_named_unsafe(lmem_size, uint, 0400,
			"Set the lmem size(in MiB) for each region. (default: 0, all memory)");

i915_param_named_unsafe(enable_eviction, uint, 0600,
	"Enable eviction which does not rely on DMA resv refactoring "
	"0=disabled, 1=memcpy based only, 2=blt based only, "
	"3=blt based but fallsback to memcpy based [default])");

/*
 * In execbuff path, we should iterate over all non-private (shared) objects of
 * the VM to take the dma_resv lock. But this causes a performance degradation
 * as execbuff latency will be O(n) where 'n' is the number of non_private
 * objects. Hence adding this enable_non_private_objects module param (default
 * fasle) to control this feature.
 *
 * Having this module param and disabling it by default is an ugly performance
 * hack so that UMDs can continue with same performance as before, until they
 * measure (and improve) the performance with proper handling of non-private
 * objects is enabled.
 */
i915_param_named_unsafe(enable_non_private_objects, bool, 0400,
			"Enable non-private objects handling in execbuff path");

i915_param_named(max_vfs, uint, 0400,
	"Limit number of virtual functions to allocate. "
	"(0 = no VFs [default]; N = allow up to N VFs)");

#if IS_ENABLED(CPTCFG_DRM_I915_DEBUG_IOV)
i915_param_named_unsafe(vfs_flr_mask, ulong, 0600,
	"Bitmask to enable (1) or disable (0) cleaning by PF VF's resources "
	"(GGTT and LMEM) after FLR (default: ~0 - cleaning enable for all VFs) "
	"Bit number indicates VF number, e.g. bit 1 indicates VF1");
#endif

i915_param_named_unsafe(debug_eu, int, 0400,
	"Enable EU debug capabilities (default: 0)");

i915_param_named_unsafe(debugger_timeout_ms, uint, 0400,
	"Setup debugger disconnect timeout in milliseconds (default: 3000, 0 never)");

i915_param_named_unsafe(debugger_log_level, int, 0600,
	"EU debugger log level (-1 = default, 0=none, 1=err, 2=warn, 3=info, 4=verbose)");

i915_param_named_unsafe(enable_hw_throttle_blt, bool, 0400,
	"Enable hardware throttling BLT on XEHPSDV A0. (default: yes)");

i915_param_named_unsafe(enable_fake_int_wa, bool, 0400,
			"Enable fake interrupts via polling timer w/a for multi-tile platforms. (default: true)");

#if IS_ENABLED(CPTCFG_DRM_I915_DEBUG_CONTIGUOUS)
i915_param_named_unsafe(force_alloc_contig, int, 0400,
	"Force allocation of LMEM and SMEM objects from physically contiguous pages. "
	"0=disabled [default], 1=SMEM only, 2=LMEM only, 3=both");
#endif

i915_param_named_unsafe(prelim_override_p2p_dist, uint, 0400,
			"Flags to determine P2P behavior: "
			"Use kernel configured behavior (default: 0), "
			"Override distance check (1), "
			"Fabric path only (2)");

i915_param_named_unsafe(smem_access_control, int, 0600,
			"Bitmask to indicate WA enabled for pcie deadlock, bits 1 and 2 are mutually exclusive"
			"bit-0 if set LRC, hwsp and guc objects in smem, "
			"bit-1 stall gpu before ppgtt updates, "
			"bit-2 Update ppgtt and ggtt using blitter commands");

i915_param_named_unsafe(page_sz_mask, uint, 0600,
			"mask to force the huge page sizes\n"
			"bit0 4K page, bit1 64K page bit2 2M page, bit3 1G page size");

i915_param_named_unsafe(ulls_bcs0_pm_wa, bool, 0600,
	"Workaround for VLK-20104 which disables bcs0 PM (default: true)");

i915_param_named_unsafe(debug_pages, uint, 0400,
			"Extra pages allocated for debug (default=0, Bit 31 indicates LMEM)");

i915_param_named_unsafe(enable_stateless_mc, bool, 0400,
	"Set default for PVC stateless memory compression (0=disabled [default],  1=enabled)");

i915_param_named_unsafe(enable_mem_fence, bool, 0400,
			"Set this true to enable MEM_FENCE workaround (default: false");

i915_param_named_unsafe(force_driver_flr, int, 0400,
			"Set this to enforce doing or skipping a driver-FLR at MMIO init and fini"
			"-1=driver decides[default], 0=skip driver flr, 1=trigger driver flr");

/*
 * This module parameter is needed because SRIOV PF and IAF are mutually
 * exclusive (see HSDES #14014623804).  Until this is fixed, the driver
 * needs to be able to enable/disable the IAF infrastructure (specifically
 * Device Physical Addressing).  Since there will be no enable/disable for the
 * SRIOV PF path, this parameter is needed to explicitly disable IAF when
 * SRIOV PF is required.
 */
i915_param_named(enable_iaf, bool, 0400, "Enable IAF feature (default: true)");

static __always_inline void _print_param(struct drm_printer *p,
					 const char *name,
					 const char *type,
					 const void *x)
{
	if (!__builtin_strcmp(type, "bool"))
		drm_printf(p, "i915.%s=%s\n", name,
			   str_yes_no(*(const bool *)x));
	else if (!__builtin_strcmp(type, "int"))
		drm_printf(p, "i915.%s=%d\n", name, *(const int *)x);
	else if (!__builtin_strcmp(type, "unsigned int"))
		drm_printf(p, "i915.%s=%u\n", name, *(const unsigned int *)x);
	else if (!__builtin_strcmp(type, "unsigned long"))
		drm_printf(p, "i915.%s=%lu\n", name, *(const unsigned long *)x);
	else if (!__builtin_strcmp(type, "char *"))
		drm_printf(p, "i915.%s=%s\n", name, *(const char **)x);
	else
		WARN_ONCE(1, "no printer defined for param type %s (i915.%s)\n",
			  type, name);
}

/**
 * i915_params_dump - dump i915 modparams
 * @params: i915 modparams
 * @p: the &drm_printer
 *
 * Pretty printer for i915 modparams.
 */
void i915_params_dump(const struct i915_params *params, struct drm_printer *p)
{
#define PRINT(T, x, ...) _print_param(p, #x, #T, &params->x);
	I915_PARAMS_FOR_EACH(PRINT);
#undef PRINT
}

static __always_inline void dup_param(const char *type, void *x)
{
	if (!__builtin_strcmp(type, "char *"))
		*(void **)x = kstrdup(*(void **)x, GFP_ATOMIC);
}

void i915_params_copy(struct i915_params *dest, const struct i915_params *src)
{
	*dest = *src;
#define DUP(T, x, ...) dup_param(#T, &dest->x);
	I915_PARAMS_FOR_EACH(DUP);
#undef DUP
}

static __always_inline void free_param(const char *type, void *x)
{
	if (!__builtin_strcmp(type, "char *")) {
		kfree(*(void **)x);
		*(void **)x = NULL;
	}
}

/* free the allocated members, *not* the passed in params itself */
void i915_params_free(struct i915_params *params)
{
#define FREE(T, x, ...) free_param(#T, &params->x);
	I915_PARAMS_FOR_EACH(FREE);
#undef FREE
}
