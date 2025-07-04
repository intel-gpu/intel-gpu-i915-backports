// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "intel_gt.h"
#include "intel_gt_ccs_mode.h"
#include "intel_gt_debugfs.h"
#include "intel_gt_mcr.h"
#include "intel_gt_pm.h"
#include "intel_gt_print.h"
#include "intel_gt_regs.h"
#include "intel_gt_requests.h"
#include "intel_gt_sysfs.h"
#include "intel_gt_sysfs_pm.h"

#define PVC_NUM_CSLICES_PER_TILE 4

#define ALL_CCS(gt) (CCS_MASK(gt) << CCS0)
#define ONE_CCS(gt) FIRST_ENGINE_MASK((gt), CCS)

static bool needs_fixed_ccs_mode(struct intel_gt *gt)
{
	if (!(IS_PONTEVECCHIO(gt->i915) || IS_DG2(gt->i915)))
		return false;

	/*
	 * Only the PF knows the entire system state and can deduce the
	 * appropriate engine:slice mapping. However, the PF doesn't know
	 * the user's requested configuration and so cannot allocate
	 * precise mappings ahead of time. Instead we opt to apply a static
	 * mapping between single CCS engine and all compute slices -- we never
	 * need to reconfigure.
	 */
	if (IS_SRIOV(gt->i915))
		return false;

	return true;
}

void intel_gt_init_ccs_mode(struct intel_gt *gt)
{
	mutex_init(&gt->ccs.mutex);
	gt->ccs.fixed = needs_fixed_ccs_mode(gt);
	gt->ccs.dynamic = gt->ccs.fixed;
}

__maybe_unused static bool assert_compute_idle(struct intel_gt *gt)
{
	i915_mcr_reg_t instdone;
	int subslice;
	int slice;
	int iter;

	instdone = XEHPC_ROW_INSTDONE;
	if (GRAPHICS_VER_FULL(gt->i915) < IP_VER(12, 60))
		instdone = GEN8_ROW_INSTDONE;

	/*
	 * Check IC done on all DSS. IC done indicates EU is
	 * done executing WL.
	 */
	for_each_ss_steering(iter, gt, slice, subslice) {
		if ((intel_gt_mcr_read(gt, instdone, slice, subslice) & XEHPC_IC_DONE) == 0)
			return false;
	}

	return true;
}

static void __intel_gt_apply_ccs_mode(struct intel_gt *gt, intel_engine_mask_t config)
{
	u32 mode = XEHP_CCS_MODE_CSLICE_0_3_MASK; /* disable all by default */
	struct intel_engine_cs *engine;
	int num_slices, num_engines;
	intel_engine_mask_t tmp;
	int width, cslice;

	lockdep_assert_held(&gt->ccs.mutex);
	GEM_BUG_ON(GRAPHICS_VER_FULL(gt->i915) < IP_VER(12, 50));
	GEM_BUG_ON(!config);

	/* Ignore uneven numbers of slices/engines: bad config */
	num_slices = hweight32(CCS_MASK(gt));
	num_engines = hweight32(config);
	if (num_slices / num_engines * num_engines != num_slices) {
		/* Let all users have the same allotment of slices/engine */
		config = ALL_CCS(gt);
		num_engines = hweight32(config);
	}
	GEM_BUG_ON(num_engines > num_slices);
	GEM_BUG_ON(num_slices > PVC_NUM_CSLICES_PER_TILE);

	/*
	 * Loop over all available slices and assign each a user
	 * engine.
	 *
	 * With 1 engine (ccs0):
	 *   slice 0, 1, 2, 3: ccs0
	 *
	 * With 2 engines (ccs0, ccs1):
	 *   slice 0, 2: ccs0
	 *   slice 1, 3: ccs1
	 *
	 * With 4 engines (ccs0, ccs1, ccs2, ccs3):
	 *   slice 0: ccs0
	 *   slice 1: ccs1
	 *   slice 2: ccs2
	 *   slice 3: ccs3
	 *
	 * Since the number of slices and the number of engines is
	 * known, and we ensure that there is an exact multiple of
	 * engines for slices, the double loop becomes a loop over each
	 * slice.
	 */
	for (width = num_slices / num_engines, cslice = 0; width--;) {
		for_each_engine_masked(engine, gt, config, tmp) {
			/* If a slice is fused off, leave disabled */
			while ((CCS_MASK(gt) & BIT(cslice)) == 0) {
				mode |= XEHP_CCS_MODE_CSLICE(cslice, 0xf);
				cslice++;
			}

			mode &= ~XEHP_CCS_MODE_CSLICE(cslice, XEHP_CCS_MODE_CSLICE_MASK);
			mode |= XEHP_CCS_MODE_CSLICE(cslice, engine->instance);
			cslice++;
		}
	}
	GEM_BUG_ON(cslice > PVC_NUM_CSLICES_PER_TILE);

	if (mode != gt->ccs.mode) {
		GT_TRACE(gt,
			 "CCS_MODE=%x for config:%08x, num_engines:%d, num_slices:%d\n",
			 mode, config, num_engines, num_slices);

		GEM_BUG_ON(!assert_compute_idle(gt));
		intel_uncore_write(gt->uncore, XEHP_CCS_MODE, mode);
		gt->ccs.mode = mode;
	}

	gt->ccs.config = config;
}

void intel_gt_apply_ccs_mode(struct intel_gt *gt)
{
	intel_engine_mask_t config = 0;

	mutex_lock(&gt->ccs.mutex);

	/*
	 * on GT reset we might have ccs.active non zero.
	 * Example: context running might get GPU hang hence
	 * end up in GT reset. But we never called gt_park thus
	 * any active engines before reset will be reconfigured
	 */
	if (gt->ccs.active)
		config = gt->ccs.config;

	/*
	 * SRIOV PF will always use ccs-1 mode from init/reset onwards
	 * as we have asked GUC to restore CCS_MODE for engine resets
	 */
	if (IS_SRIOV_PF(gt->i915))
		config = ONE_CCS(gt);

	gt->ccs.mode = -1;
	if (config)
		__intel_gt_apply_ccs_mode(gt, config);

	mutex_unlock(&gt->ccs.mutex);
}

int intel_gt_configure_ccs_mode(struct intel_gt *gt,
				intel_engine_mask_t config,
				const struct intel_engine_cs *engine)
{
	int err = 0;

	/*
	 * Allow dynamic reconfiguration of the compute engines depending on
	 * application load (narrow/fast vs wide/slow).
	 *
	 * During context creation, the application will set how many compute
	 * engines they will be using. Then during execbuf, we inspect the
	 * context's compute config and configure the system accordingly.
	 *
	 * If the system is idle or does not have any configuration yet, the
	 * application's configuration is applied immediately and will stay in
	 * effect until that set of engines is idle.
	 *
	 * If the application only wishes to use a subset of engines already
	 * configured, the system configuration will remain in effect, so the
	 * application will run in a slower mode while competing for system
	 * resources with the second application.
	 *
	 * If the application wishes to use a wider mode than currently
	 * configured, it will be rejected with -EBUSY if the system is still
	 * active - we can only change compute configuration while they are
	 * idle.
	 */

	config &= ALL_CCS(gt);
	if (likely((READ_ONCE(gt->ccs.active) & config) == config))
		return 0;

	if ((engine->mask & config) == 0 || !gt->ccs.dynamic)
		return 0;

	if (gt->ccs.active) /* clear ccs_active if possible */
		intel_gt_retire_requests(gt);

	mutex_lock(&gt->ccs.mutex);
	if (config & ~gt->ccs.config) {
		if (!gt->ccs.active)
			__intel_gt_apply_ccs_mode(gt, config);
		else
			err = -EBUSY;
	}
	if (err == 0)
		gt->ccs.active = gt->ccs.config;
	mutex_unlock(&gt->ccs.mutex);

	return err;
}

void intel_gt_park_ccs_mode(struct intel_gt *gt, struct intel_engine_cs *engine)
{
	if (!gt->ccs.active)
		return;

	mutex_lock(&gt->ccs.mutex);

	/*
	 * As we park (idle) individual engines, they are then safe
	 * to be reassigned in CCS_MODE. After we park the last engine, and
	 * the whole GT becomes idle
	 */
	if (engine)
		gt->ccs.active &= ~engine->mask;
	else
		gt->ccs.active = 0;

	/*
	 * When compute is idle again, we can consider reconfiguring CCS_MODE
	 * to allow fast/narrow for new clients.
	 *
	 * If we never reset the config, we will only allow CCS_MODE to widen
	 * and eventually be locked into a 4-ccs mode even if the system
	 * thenceforth only ever uses a single ccs.
	 *
	 * By resetting the config, we allow ourselves to switch back into a
	 * fast/narrow mode, at the cost of preventing the next wide client
	 * from executing until idle again.
	 *
	 * The other choice where we may consider releasing the lock on the
	 * config is upon context closure. In that case a config will be locked
	 * from the moment a context is created or first activated, until it
	 * the last concurrent matching context is idle. This has one advantage
	 * of reducing the risk of that first context losing a race to a second
	 * context, at the cost of preventing the second context from running
	 * in its optimal configuration. A key consideration here though is
	 * that we can forcibly idle the system by issuing a GPU reset; but we
	 * would have to extend the context banning to release the config lock
	 * as we cannot otherwise force release the context. It is easier to
	 * create a context and leave it hidden in the background (keeping the
	 * config locked) compared to an active compute task which is easily
	 * monitored by existing hang detection and userspace tooling.
	 */
	if (!gt->ccs.active)
		gt->ccs.config = 0;

	mutex_unlock(&gt->ccs.mutex);
}

static int ccs_debug_show(struct seq_file *m, void *data)
{
	struct intel_gt *gt = m->private;
	intel_engine_mask_t config, active, tmp;
	struct intel_engine_cs *engine;
	const char *prefix;
	intel_wakeref_t wf;
	u32 rcu_debug;
	u32 rcu_mode;
	u32 ccs_mode;
	int i;

	with_intel_runtime_pm(gt->uncore->rpm, wf) {
		mutex_lock(&gt->ccs.mutex);
		active = gt->ccs.active;
		config = gt->ccs.config;
		ccs_mode = intel_uncore_read(gt->uncore, XEHP_CCS_MODE);
		rcu_mode = intel_uncore_read(gt->uncore, GEN12_RCU_MODE);
		rcu_debug = intel_uncore_read(gt->uncore, GEN12_RCU_DEBUG_1);
		mutex_unlock(&gt->ccs.mutex);
	}

	if (rcu_mode & XEHP_RCU_MODE_FIXED_SLICE_CCS_MODE)
		seq_printf(m, "strategy: fixed\n");
	else
		seq_printf(m, "strategy: dynamic\n");

	seq_printf(m, "user: %s\n", str_yes_no(!gt->ccs.dynamic));

	prefix = "";
	seq_printf(m, "config: %08x [", config);
	for_each_engine_masked(engine, gt, config, tmp) {
		seq_printf(m, "%s%s", prefix, engine->name);
		prefix = ", ";
	}
	seq_printf(m, "]\n");

	prefix = "";
	seq_printf(m, "active: %08x [", active);
	for_each_engine_masked(engine, gt, active, tmp) {
		seq_printf(m, "%s%s", prefix, engine->name);
		prefix = ", ";
	}
	seq_printf(m, "]\n");

	prefix = "";
	seq_printf(m, "CCS_MODE: %08x [", ccs_mode);
	for (i = 0; i < PVC_NUM_CSLICES_PER_TILE; i++) {
		int inst = (ccs_mode >> (XEHP_CCS_MODE_CSLICE_WIDTH * i)) & XEHP_CCS_MODE_CSLICE_MASK;
		const char *name;

		if (inst == 0x7) {
			name = "disabled";
		} else {
			engine = gt->engine_class[COMPUTE_CLASS][inst];
			if (engine)
				name = engine->name;
			else
				name = "invalid";
		}
		seq_printf(m, "%s%s", prefix, name);
		prefix = ", ";
	}
	seq_printf(m, "]\n");

	seq_printf(m, "RCU_MODE: %08x\n", rcu_mode);

	prefix = "";
	seq_printf(m, "RCU_DEBUG_1: %08x [", rcu_debug);
	for (i = 0; i < PVC_NUM_CSLICES_PER_TILE; i++) {
		if (rcu_debug & BIT(10 + 3 * i)) {
			seq_printf(m, "%sslice%d", prefix, i);
			prefix = ", ";
		}
	}
	seq_printf(m, "]\n");

	return 0;
}
DEFINE_INTEL_GT_DEBUGFS_ATTRIBUTE(ccs_debug);

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t num_cslices_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buf)
#else
static ssize_t num_cslices_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
#endif
{
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	struct intel_gt *gt = kobj_to_gt(kobj);
#else
	struct intel_gt *gt = kobj_to_gt(&dev->kobj);
#endif

	return sysfs_emit(buf, "%u\n", hweight32(CCS_MASK(gt)));
}
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static KOBJ_ATTR_RO(num_cslices);
#else
static DEVICE_ATTR_RO(num_cslices);
#endif

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t ccs_mode_show(struct kobject *kobj,
			     struct kobj_attribute *attr,
			     char *buf)
#else
static ssize_t ccs_mode_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
#endif
{
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	struct intel_gt *gt = kobj_to_gt(kobj);
#else
	struct intel_gt *gt = kobj_to_gt(&dev->kobj);
#endif
	return sysfs_emit(buf, "%u\n", gt->ccs.width);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t ccs_mode_store(struct kobject *kobj,
			      struct kobj_attribute *attr,
			      const char *buf,
			      size_t count)
#else
static ssize_t ccs_mode_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf,
			      size_t count)
#endif
{
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	struct intel_gt *gt = kobj_to_gt(kobj);
#else
	struct intel_gt *gt = kobj_to_gt(&dev->kobj);
#endif
	int num_cslices = hweight32(CCS_MASK(gt));
	ssize_t ret;
	u32 val = 0;

	if (kstrtou32(buf, 0, &val))
		/* fallback to interpreting the input as binary */
		memcpy(&val, buf, min(count, sizeof(val)));

	if (val == gt->ccs.width)
		return count;

	if (val > num_cslices || (val && num_cslices % val))
		return -EINVAL;

	if (intel_gt_wait_for_idle(gt, 5 * HZ))
		return -EBUSY;

	if (intel_gt_pm_wait_for_idle(gt, HZ))
		return -EBUSY;

	ret = -EBUSY;
	mutex_lock(&gt->wakeref.mutex);
	if (!intel_gt_pm_is_awake(gt)) {
		mutex_lock(&gt->ccs.mutex);
		GEM_BUG_ON(gt->ccs.active);
		gt->ccs.dynamic = !val;
		gt->ccs.width = val;
		if (val) {
			val = GENMASK(val - 1, 0) << CCS0;
			gt->uabi_engines &= ~ALL_CCS(gt);
			gt->uabi_engines |= val;
			__intel_gt_apply_ccs_mode(gt, val);
		} else {
			gt->uabi_engines |= ALL_CCS(gt);
			gt->ccs.config = 0;
		}
		mutex_unlock(&gt->ccs.mutex);

		ret = count;
	}
	mutex_unlock(&gt->wakeref.mutex);

	return ret;
}
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static KOBJ_ATTR_RW(ccs_mode);
#else
static DEVICE_ATTR_RW(ccs_mode);
#endif

void intel_gt_sysfs_register_ccs(struct intel_gt *gt, struct kobject *parent)
{
	if (sysfs_create_file(parent, &dev_attr_num_cslices.attr))
		goto err;

	if (!gt->ccs.fixed)
		return;

	if (sysfs_create_file(parent, &dev_attr_ccs_mode.attr))
		goto err;

	return;
err:
	gt_warn(gt, "Failed to create sysfs entries for fixed ccs_mode\n");
}

void intel_gt_debugfs_register_ccs_mode(struct intel_gt *gt, struct dentry *root)
{
	static const struct intel_gt_debugfs_file files[] = {
		{ "ccs_mode", &ccs_debug_fops, NULL },
	};

	if (gt->ccs.fixed)
		intel_gt_debugfs_register_files(root, files, ARRAY_SIZE(files), gt);
}

void intel_gt_fini_ccs_mode(struct intel_gt *gt)
{
	mutex_destroy(&gt->ccs.mutex);
}
#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftest_gt_ccs_mode.c"
#endif
