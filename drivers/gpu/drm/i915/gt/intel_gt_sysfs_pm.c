// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

#include <drm/drm_device.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/printk.h>

#include "i915_drv.h"
#include "intel_gt.h"
#include "intel_gt_regs.h"
#include "intel_pcode.h"
#include "intel_rc6.h"
#include "intel_rps.h"
#include "intel_gt_sysfs.h"
#include "intel_gt_sysfs_pm.h"

/*
 * Scaling for multipliers (aka frequency factors).
 * The format of the value in the register is u8.8.
 *
 * The presentation to userspace is inspired by the perf event framework.
 * See file
 *   Documentation/ABI/testing/sysfs-bus-event_source-devices-events
 * for the description of:
 *   /sys/bus/event_source/devices/<pmu>/events/<event>.scale
 *
 * Summary: Expose two sysfs files for each multiplier.
 *
 * 1. File <attr> contains a raw hardware value.
 * 2. File <attr>.scale contains the multiplicative scale factor to be
 *    used by userspace to compute the actual value.
 *
 * So userspace knows that to get the frequency_factor it multiples the
 * provided value by the specified scale factor and vice-versa.
 *
 * That way there is no precision loss in the kernel interface and API
 * is future proof should one day the hardware register change to u16.u16,
 * on some platform.  (Or any other fixed point representation.)
 *
 * Example:
 * File <attr> contains the value 2.5, represented as u8.8 0x0280, which
 * is comprised of:
 * - an integer part of 2
 * - a fractional part of 0x80 (representing 0x80 / 2^8 == 0x80 / 256).
 * File <attr>.scale contains a string representation of floating point
 * value 0.00390625 (which is (1 / 256)).
 * (Optional scientific notation: 3.90625e-3)
 * Userspace computes the actual value:
 *   0x0280 * 0.00390625 -> 2.5
 * or converts an actual value to the value to be written into <attr>:
 *   2.5 / 0.00390625 -> 0x0280
 */

static ssize_t
i915_sysfs_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t
i915_sysfs_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count);

typedef ssize_t (*show)(struct device *dev, struct device_attribute *attr,
		char *buf);
typedef ssize_t (*store)(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count);

struct i915_ext_attr {
	struct device_attribute attr;
	show i915_show;
	store i915_store;
};

#define I915_DEVICE_ATTR_RO(_name, _show) \
	struct i915_ext_attr dev_attr_##_name = \
	{ __ATTR(_name, 0444, i915_sysfs_show, NULL), _show, NULL}

#define I915_DEVICE_ATTR_WO(_name, _store) \
	struct i915_ext_attr dev_attr_##_name = \
	{ __ATTR(_name, 0200, NULL, i915_sysfs_store), NULL, _store}

#define I915_DEVICE_ATTR_RW(_name, _mode, _show, _store) \
	struct i915_ext_attr dev_attr_##_name = \
	{ __ATTR(_name, _mode, i915_sysfs_show, i915_sysfs_store),  _show, _store}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
/*Introduced kobj attributes to adopt to access sysnode under <dev>/gt/gt<i>/ */

static ssize_t
i915_sysfs_show_kobj(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf);

static ssize_t
i915_sysfs_store_kobj(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count);
typedef ssize_t (*show_kobj)(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf);
typedef ssize_t (*store_kobj)(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count);

struct i915_ext_attr_kobj {
	struct kobj_attribute attr;
	show_kobj i915_show_kobj;
	store_kobj i915_store_kobj;
};
#endif

static ssize_t
i915_kobj_sysfs_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf);

typedef ssize_t (*kobj_show)(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf);
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
typedef ssize_t (*kobj_store)(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count);
#else
typedef ssize_t (*kobj_store)(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf, size_t count);
#endif

struct i915_kobj_ext_attr {
	struct kobj_attribute attr;
	kobj_show i915_kobj_show;
	kobj_store i915_kobj_store;
};

#define U8_8_VAL_MASK           0xffff
#define U8_8_SCALE_TO_VALUE     "0.00390625"


#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static u32 _with_pm_intel_dev_read(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   i915_reg_t rgadr)
#else
static u32 _with_pm_intel_dev_read(struct device *dev,
                                  struct device_attribute *attr,
				   i915_reg_t rgadr)
#endif
{
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	struct device *dev = kobj_to_dev(kobj);
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	struct intel_uncore *uncore = gt->uncore;
	intel_wakeref_t wakeref;
	u32 regval;

	with_intel_runtime_pm(uncore->rpm, wakeref)
		regval = intel_uncore_read(uncore, rgadr);

	return regval;
}

#if IS_ENABLED(CONFIG_PM)
static u32 get_residency(struct intel_gt *gt, i915_reg_t reg)
{
	intel_wakeref_t wakeref;
	u64 res = 0;

	with_intel_runtime_pm(gt->uncore->rpm, wakeref)
		res = intel_rc6_residency_us(&gt->rc6, reg);

	return DIV_ROUND_CLOSEST_ULL(res, 1000);
}

static ssize_t rc6_enable_store(struct device *dev,
				struct device_attribute *attr,
				const char *buff, size_t count)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	intel_wakeref_t wakeref;
	ssize_t ret;
	u32 val;

	ret = kstrtou32(buff, 0, &val);
	if (ret)
		return ret;

	wakeref = intel_runtime_pm_get(gt->uncore->rpm);

	if (val) {
		if (gt->rc6.enabled)
			goto unlock;

		if (!gt->rc6.wakeref)
			intel_rc6_rpm_get(&gt->rc6);

		intel_rc6_enable(&gt->rc6);
		intel_rc6_unpark(&gt->rc6);
	} else {
		intel_rc6_disable(&gt->rc6);

		if (gt->rc6.wakeref)
			intel_rc6_rpm_put(&gt->rc6);
	}

unlock:
	intel_runtime_pm_put(gt->uncore->rpm, wakeref);

	return count;
}

static ssize_t rc6_enable_show(struct device *dev,
			       struct device_attribute *attr,
			       char *buff)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);

	return scnprintf(buff, PAGE_SIZE, "%d\n", gt->rc6.enabled);
}

static ssize_t rc6_residency_ms_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buff)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 rc6_residency;
	i915_reg_t reg;

	if (gt->type == GT_MEDIA)
		reg = MTL_MEDIA_MC6;
	else
		reg = GEN6_GT_GFX_RC6;

	rc6_residency = get_residency(gt, reg);

	return scnprintf(buff, PAGE_SIZE, "%u\n", rc6_residency);
}

static ssize_t rc6p_residency_ms_show(struct device *dev,
				      struct device_attribute *attr,
				      char *buff)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 rc6p_residency = get_residency(gt, GEN6_GT_GFX_RC6p);

	return scnprintf(buff, PAGE_SIZE, "%u\n", rc6p_residency);
}

static ssize_t rc6pp_residency_ms_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buff)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 rc6pp_residency = get_residency(gt, GEN6_GT_GFX_RC6pp);

	return scnprintf(buff, PAGE_SIZE, "%u\n", rc6pp_residency);
}

static ssize_t media_rc6_residency_ms_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buff)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 rc6_residency = get_residency(gt, VLV_GT_MEDIA_RC6);

	return scnprintf(buff, PAGE_SIZE, "%u\n", rc6_residency);
}

/* sysfs dual-location rc6 files under directories <dev>/power/ and <dev>/gt/gt<i>/ */

static I915_DEVICE_ATTR_RW(rc6_enable, 0644, rc6_enable_show, rc6_enable_store);
static I915_DEVICE_ATTR_RO(rc6_residency_ms, rc6_residency_ms_show);
static I915_DEVICE_ATTR_RO(rc6p_residency_ms, rc6p_residency_ms_show);
static I915_DEVICE_ATTR_RO(rc6pp_residency_ms, rc6pp_residency_ms_show);
static I915_DEVICE_ATTR_RO(media_rc6_residency_ms, media_rc6_residency_ms_show);

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t gt_rc6_enable_store(struct kobject *kobj, struct kobj_attribute *attr,
				const char *buff, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);

	return rc6_enable_store(dev, (struct device_attribute *)attr, buff, count);
}

static ssize_t gt_rc6_enable_show(struct kobject *kobj, struct kobj_attribute *attr,
			       char *buff)
{
	struct device *dev = kobj_to_dev(kobj);

	return rc6_enable_show(dev, (struct device_attribute *)attr, buff);
}

static ssize_t gt_rc6_residency_ms_show(struct kobject *kobj, struct kobj_attribute *attr,
				     char *buff)
{
	struct device *dev = kobj_to_dev(kobj);

	return rc6_residency_ms_show(dev, (struct device_attribute *)attr, buff);
}

static ssize_t gt_rc6p_residency_ms_show(struct kobject *kobj, struct kobj_attribute *attr,
				      char *buff)
{
	struct device *dev = kobj_to_dev(kobj);

	return rc6p_residency_ms_show(dev, (struct device_attribute *)attr, buff);
}

static ssize_t gt_rc6pp_residency_ms_show(struct kobject *kobj, struct kobj_attribute *attr,
				       char *buff)
{
	struct device *dev = kobj_to_dev(kobj);

	return rc6pp_residency_ms_show(dev, (struct device_attribute *)attr, buff);
}

static ssize_t gt_media_rc6_residency_ms_show(struct kobject *kobj, struct kobj_attribute *attr,
					   char *buff)
{
	struct device *dev = kobj_to_dev(kobj);

	return media_rc6_residency_ms_show(dev, (struct device_attribute *)attr, buff);
}

#define INTEL_KOBJ_GT_ATTR(_name, __mode, __show, __store, i915_show_kobj, i915_store_kobj) \
	static struct i915_ext_attr_kobj dev_attr_gt_##_name =    \
	{__ATTR(_name, __mode, __show, __store), i915_show_kobj, i915_store_kobj};    \

#define INTEL_KOBJ_GT_ATTR_RO(_name)                               \
	INTEL_KOBJ_GT_ATTR(_name, 0444, i915_sysfs_show_kobj, NULL,\
		gt_##_name##_show, NULL)
#define INTEL_KOBJ_GT_ATTR_RW(_name)                               \
	INTEL_KOBJ_GT_ATTR(_name, 0644, i915_sysfs_show_kobj,   \
		i915_sysfs_store_kobj, gt_##_name##_show, \
		gt_##_name##_store)

INTEL_KOBJ_GT_ATTR_RW(rc6_enable);
INTEL_KOBJ_GT_ATTR_RO(rc6_residency_ms);
INTEL_KOBJ_GT_ATTR_RO(rc6p_residency_ms);
INTEL_KOBJ_GT_ATTR_RO(rc6pp_residency_ms);
INTEL_KOBJ_GT_ATTR_RO(media_rc6_residency_ms);

static struct attribute *gt_rc6_attrs[] = {
	&dev_attr_gt_rc6_enable.attr.attr,
	&dev_attr_gt_rc6_residency_ms.attr.attr,
	NULL
};

static struct attribute *gt_rc6p_attrs[] = {
	&dev_attr_gt_rc6p_residency_ms.attr.attr,
	&dev_attr_gt_rc6pp_residency_ms.attr.attr,
	NULL
};

static struct attribute *gt_media_rc6_attrs[] = {
	&dev_attr_gt_media_rc6_residency_ms.attr.attr,
	NULL
};
#endif /* BPM_DEVICE_ATTR_NOT_PRESENT */

static struct attribute *rc6_attrs[] = {
	&dev_attr_rc6_enable.attr.attr,
	&dev_attr_rc6_residency_ms.attr.attr,
	NULL
};

static struct attribute *rc6p_attrs[] = {
	&dev_attr_rc6p_residency_ms.attr.attr,
	&dev_attr_rc6pp_residency_ms.attr.attr,
	NULL
};

static struct attribute *media_rc6_attrs[] = {
	&dev_attr_media_rc6_residency_ms.attr.attr,
	NULL
};

static const struct attribute_group rc6_attr_group[] = {
	{ .name = power_group_name, .attrs = rc6_attrs },
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	{ .attrs = gt_rc6_attrs }
#else
	{ .attrs = rc6_attrs }
#endif
};

static const struct attribute_group rc6p_attr_group[] = {
	{ .name = power_group_name, .attrs = rc6p_attrs },
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	{ .attrs = gt_rc6p_attrs }
#else
	{ .attrs = rc6p_attrs }
#endif

};

static const struct attribute_group media_rc6_attr_group[] = {
	{ .name = power_group_name, .attrs = media_rc6_attrs },
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	{ .attrs = gt_media_rc6_attrs }
#else
	{ .attrs = media_rc6_attrs }

#endif
};

static int __intel_gt_sysfs_create_group(struct kobject *kobj,
					 const struct attribute_group *grp)
{
	/* is_object_gt() returns 0 if parent device or 1 if gt/gt<i>. */
	int i = is_object_gt(kobj);

	/*
	 * For gt/gt<i>, sysfs_create_group() from grp[1] - group name = "".
	 * For <parent>, sysfs_merge_group()  from grp[0] - group name = "power"
	 * which must already exist.
	 */
	return i ? sysfs_create_group(kobj, &grp[i]) :
		   sysfs_merge_group(kobj, &grp[i]);
}

/*
 * intel_sysfs_rc6_init()
 * @gt: The gt being processed.
 * @kobj: The kobj in sysfs to which created files will be attached.
 *
 * Called unconditionally from intel_gt_sysfs_pm_init:
 * - Once with kobj specifying directory of parent_device (and gt specifying gt0).
 *   Places files under <dev>/power
 * - Once per gt, with kobj specifying directory gt/gt<i>
 *   Places files under <dev>/gt/gt<i>.
 */
static void intel_sysfs_rc6_init(struct intel_gt *gt, struct kobject *kobj)
{
	int ret;

	if (!HAS_RC6(gt->i915))
		return;

	ret = __intel_gt_sysfs_create_group(kobj, rc6_attr_group);
	if (ret)
		drm_err(&gt->i915->drm,
			"failed to create gt%u RC6 sysfs files\n", gt->info.id);

	if (HAS_RC6p(gt->i915)) {
		ret = __intel_gt_sysfs_create_group(kobj, rc6p_attr_group);
		if (ret)
			drm_err(&gt->i915->drm,
				"failed to create gt%u RC6p sysfs files\n",
				gt->info.id);
	}

	if (IS_VALLEYVIEW(gt->i915) || IS_CHERRYVIEW(gt->i915)) {
		ret = __intel_gt_sysfs_create_group(kobj, media_rc6_attr_group);
		if (ret)
			drm_err(&gt->i915->drm,
				"failed to create media %u RC6 sysfs files\n",
				gt->info.id);
	}
}

static ssize_t vlv_rpe_freq_mhz_show(struct device *dev,
				     struct device_attribute *attr, char *buff)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	struct intel_rps *rps = &gt->rps;

	return scnprintf(buff, PAGE_SIZE, "%d\n",
			intel_gpu_freq(rps, rps->efficient_freq));
}
#else
static void intel_sysfs_rc6_init(struct intel_gt *gt, struct kobject *kobj)
{
}
#endif /* CONFIG_PM */

static ssize_t act_freq_mhz_show(struct device *dev,
				 struct device_attribute *attr, char *buff)
{
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	struct kobject *kobj = &dev->kobj;
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);

	/*
	 * For PVC show chiplet freq which is the "base" frequency, all other
	 * gt/rps frequency attributes also apply to the chiplet.
	 * intel_rps_read_actual_frequency is used in base_act_freq_mhz_show
	 */
	if (IS_PONTEVECCHIO(gt->i915)) {
		return sysfs_emit(buff, "%d\n",
				  intel_rps_read_chiplet_frequency(&gt->rps));
	} else {
		return sysfs_emit(buff, "%d\n",
				  intel_rps_read_actual_frequency(&gt->rps));
	}
}

static ssize_t cur_freq_mhz_show(struct device *dev,
				 struct device_attribute *attr, char *buff)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	struct intel_rps *rps = &gt->rps;

	return scnprintf(buff, PAGE_SIZE, "%d\n",
				intel_rps_get_requested_frequency(rps));
}

static ssize_t boost_freq_mhz_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buff)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	struct intel_rps *rps = &gt->rps;

	return scnprintf(buff, PAGE_SIZE, "%d\n",
			intel_rps_get_boost_frequency(rps));
}

static ssize_t boost_freq_mhz_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buff, size_t count)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	struct intel_rps *rps = &gt->rps;
	ssize_t ret;
	u32 val;

	ret = kstrtou32(buff, 0, &val);
	if (ret)
		return ret;

	ret = intel_rps_set_boost_frequency(rps, val);

	return ret ?: count;
}

static ssize_t max_freq_mhz_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buff)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	struct intel_rps *rps = &gt->rps;

	return scnprintf(buff, PAGE_SIZE, "%d\n", intel_rps_get_max_frequency(rps));
}

static ssize_t max_freq_mhz_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buff, size_t count)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	struct intel_rps *rps = &gt->rps;
	ssize_t ret;
	u32 val;

	ret = kstrtou32(buff, 0, &val);
	if (ret)
		return ret;

	ret = intel_rps_set_max_frequency(rps, val);

	return ret ?: count;
}

static ssize_t min_freq_mhz_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buff)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	struct intel_rps *rps = &gt->rps;

	return scnprintf(buff, PAGE_SIZE, "%d\n",
			intel_rps_get_min_frequency(rps));
}

static ssize_t min_freq_mhz_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buff, size_t count)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	struct intel_rps *rps = &gt->rps;
	ssize_t ret;
	u32 val;

	ret = kstrtou32(buff, 0, &val);
	if (ret)
		return ret;

	ret = intel_rps_set_min_frequency(rps, val);

	return ret ?: count;
}

static ssize_t RP0_freq_mhz_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buff)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	struct intel_rps *rps = &gt->rps;
	struct intel_guc_slpc *slpc = &gt->uc.guc.slpc;
	u32 val;

	if (intel_uc_uses_guc_slpc(&gt->uc))
		val = slpc->rp0_freq;
	else
		val = intel_gpu_freq(rps, rps->rp0_freq);

	return scnprintf(buff, PAGE_SIZE, "%d\n", val);
}

static ssize_t RP1_freq_mhz_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buff)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	struct intel_rps *rps = &gt->rps;
	struct intel_guc_slpc *slpc = &gt->uc.guc.slpc;
	u32 val;

	if (intel_uc_uses_guc_slpc(&gt->uc))
		val = slpc->rp1_freq;
	else
		val = intel_gpu_freq(rps, rps->rp1_freq);

	return scnprintf(buff, PAGE_SIZE, "%d\n", val);
}

static ssize_t RPn_freq_mhz_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buff)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	struct intel_rps *rps = &gt->rps;
	struct intel_guc_slpc *slpc = &gt->uc.guc.slpc;
	u32 val;

	if (intel_uc_uses_guc_slpc(&gt->uc))
		val = slpc->min_freq;
	else
		val = intel_gpu_freq(rps, rps->min_freq);

	return scnprintf(buff, PAGE_SIZE, "%d\n", val);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t rps_act_freq_mhz_show(struct kobject *kobj, struct kobj_attribute *attr,
					char *buff)
{
	struct device *dev = kobj_to_dev(kobj);

	return act_freq_mhz_show(dev, (struct device_attribute *)attr, buff);
}
static ssize_t rps_cur_freq_mhz_show(struct kobject *kobj, struct kobj_attribute *attr,
					char *buff)
{
	struct device *dev = kobj_to_dev(kobj);

	return cur_freq_mhz_show(dev, (struct device_attribute *)attr, buff);
}

static ssize_t rps_boost_freq_mhz_show(struct kobject *kobj, struct kobj_attribute *attr,
					char *buff)
{
	struct device *dev = kobj_to_dev(kobj);

	return boost_freq_mhz_show(dev, (struct device_attribute *)attr, buff);
}

static ssize_t rps_boost_freq_mhz_store(struct kobject *kobj, struct kobj_attribute *attr,
					const char *buff, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);

	return boost_freq_mhz_store(dev, (struct device_attribute *)attr, buff, count);
}

static ssize_t rps_max_freq_mhz_show(struct kobject *kobj, struct kobj_attribute *attr,
					char *buff)
{
	struct device *dev = kobj_to_dev(kobj);

	return max_freq_mhz_show(dev, (struct device_attribute *)attr, buff);
}

static ssize_t rps_max_freq_mhz_store(struct kobject *kobj, struct kobj_attribute *attr,
					const char *buff, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);

	return max_freq_mhz_store(dev, (struct device_attribute *)attr, buff, count);
}

static ssize_t rps_min_freq_mhz_show(struct kobject *kobj, struct kobj_attribute *attr,
					char *buff)
{
	struct device *dev = kobj_to_dev(kobj);

	return min_freq_mhz_show(dev, (struct device_attribute *)attr, buff);
}

static ssize_t rps_min_freq_mhz_store(struct kobject *kobj, struct kobj_attribute *attr,
					const char *buff, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);

	return min_freq_mhz_store(dev, (struct device_attribute *)attr, buff, count);
}

static ssize_t rps_RP0_freq_mhz_show(struct kobject *kobj, struct kobj_attribute *attr,
					char *buff)
{
	struct device *dev = kobj_to_dev(kobj);

	return RP0_freq_mhz_show(dev, (struct device_attribute *)attr, buff);
}

static ssize_t rps_RP1_freq_mhz_show(struct kobject *kobj, struct kobj_attribute *attr,
					char *buff)
{
	struct device *dev = kobj_to_dev(kobj);

	return RP1_freq_mhz_show(dev, (struct device_attribute *)attr, buff);
}

static ssize_t rps_RPn_freq_mhz_show(struct kobject *kobj, struct kobj_attribute *attr,
					char *buff)
{
	struct device *dev = kobj_to_dev(kobj);

	return RPn_freq_mhz_show(dev, (struct device_attribute *)attr, buff);
}
#endif

/*
 * sysfs dual-location files <dev>/vlv_rpe_freq_mhz and
 * <dev>/gt/gt0/vlv_rpe_freq_mhz
 */
#if IS_ENABLED(CONFIG_PM)
static I915_DEVICE_ATTR_RO(vlv_rpe_freq_mhz, vlv_rpe_freq_mhz_show);
#endif

/* sysfs dual-location files <dev>/gt_* and <dev>/gt/gt<i>/rps_* */

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
#define INTEL_GT_RPS_SYSFS_ATTR(_name, __mode, __show, __store, i915_show, i915_store) \
	static struct i915_ext_attr dev_attr_gt_##_name =    \
		{__ATTR(gt_##_name, __mode, __show, __store), i915_show, i915_store}
#else
#define INTEL_GT_RPS_SYSFS_ATTR(_name, __mode, __show, __store, i915_show, i915_store) \
	static struct i915_ext_attr dev_attr_gt_##_name =    \
		{__ATTR(gt_##_name, __mode, __show, __store), i915_show, i915_store};    \
	static struct i915_ext_attr dev_attr_rps_##_name =   \
		{__ATTR(rps_##_name, __mode, __show, __store), i915_show, i915_store}
#endif
/* Note: rps_ and gt_ share common show and store functions. */
#define INTEL_GT_RPS_SYSFS_ATTR_RO(_name)				\
		INTEL_GT_RPS_SYSFS_ATTR(_name, 0444, i915_sysfs_show, NULL,\
					_name##_show, NULL)
#define INTEL_GT_RPS_SYSFS_ATTR_RW(_name)				\
		INTEL_GT_RPS_SYSFS_ATTR(_name, 0644, i915_sysfs_show,	\
					i915_sysfs_store, _name##_show,	\
					_name##_store)

INTEL_GT_RPS_SYSFS_ATTR_RO(act_freq_mhz);
INTEL_GT_RPS_SYSFS_ATTR_RO(cur_freq_mhz);
INTEL_GT_RPS_SYSFS_ATTR_RW(boost_freq_mhz);
INTEL_GT_RPS_SYSFS_ATTR_RW(max_freq_mhz);
INTEL_GT_RPS_SYSFS_ATTR_RW(min_freq_mhz);
INTEL_GT_RPS_SYSFS_ATTR_RO(RP0_freq_mhz);
INTEL_GT_RPS_SYSFS_ATTR_RO(RP1_freq_mhz);
INTEL_GT_RPS_SYSFS_ATTR_RO(RPn_freq_mhz);


#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
#define INTEL_RPS_SYSFS_ATTR(_name, __mode, __show, __store, i915_show_kobj, \
		i915_store_kobj) \
	static struct i915_ext_attr_kobj dev_attr_rps_##_name =    \
	{__ATTR(rps_##_name, __mode, __show, __store), i915_show_kobj, \
		i915_store_kobj};    \

/* Note: rps_ and gt_ share common show and store functions. */
#define INTEL_RPS_SYSFS_ATTR_RO(_name)                               \
	INTEL_RPS_SYSFS_ATTR(_name, 0444, i915_sysfs_show_kobj, NULL,\
			rps_##_name##_show, NULL)
#define INTEL_RPS_SYSFS_ATTR_RW(_name)                               \
	INTEL_RPS_SYSFS_ATTR(_name, 0644, i915_sysfs_show_kobj,   \
			i915_sysfs_store_kobj, rps_##_name##_show, \
			rps_##_name##_store)

INTEL_RPS_SYSFS_ATTR_RO(act_freq_mhz);
INTEL_RPS_SYSFS_ATTR_RO(cur_freq_mhz);
INTEL_RPS_SYSFS_ATTR_RW(boost_freq_mhz);
INTEL_RPS_SYSFS_ATTR_RW(max_freq_mhz);
INTEL_RPS_SYSFS_ATTR_RW(min_freq_mhz);
INTEL_RPS_SYSFS_ATTR_RO(RP0_freq_mhz);
INTEL_RPS_SYSFS_ATTR_RO(RP1_freq_mhz);
INTEL_RPS_SYSFS_ATTR_RO(RPn_freq_mhz);

#endif

#define GEN6_ATTR(s) { \
		&dev_attr_##s##_act_freq_mhz.attr.attr, \
		&dev_attr_##s##_cur_freq_mhz.attr.attr, \
		&dev_attr_##s##_boost_freq_mhz.attr.attr, \
		&dev_attr_##s##_max_freq_mhz.attr.attr, \
		&dev_attr_##s##_min_freq_mhz.attr.attr, \
		&dev_attr_##s##_RP0_freq_mhz.attr.attr, \
		&dev_attr_##s##_RP1_freq_mhz.attr.attr, \
		&dev_attr_##s##_RPn_freq_mhz.attr.attr, \
		NULL, \
	}

/* sysfs files <dev>/gt_* */
static const struct attribute * const gen6_rps_attrs[] = GEN6_ATTR(rps);

/* sysfs files <dev>/gt/gt<i>/rps_* */
static const struct attribute * const gen6_gt_attrs[]  = GEN6_ATTR(gt);

#if IS_ENABLED(CONFIG_PM)
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t rapl_PL1_freq_mhz_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buff)
#else
static ssize_t rapl_PL1_freq_mhz_show(struct device *dev,
                                     struct device_attribute *attr,
                                     char *buff)
#endif
{
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	struct device *dev = kobj_to_dev(kobj);
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 rapl_pl1 = intel_rps_read_rapl_pl1_frequency(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%d\n", rapl_pl1);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t punit_req_freq_mhz_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buff)
#else
static ssize_t punit_req_freq_mhz_show(struct device *dev,
                                      struct device_attribute *attr,
                                      char *buff)
#endif
{
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	struct device *dev = kobj_to_dev(kobj);
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 swreq = intel_rps_get_requested_frequency(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%d\n", swreq);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t throttle_reason_status_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buff)
#else
static ssize_t throttle_reason_status_show(struct device *dev,
                                          struct device_attribute *attr,
                                          char *buff)
#endif
{
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	struct device *dev = kobj_to_dev(kobj);
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	bool status = !!intel_rps_read_throttle_reason_status(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%u\n", status);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t throttle_reason_pl1_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buff)
#else
static ssize_t throttle_reason_pl1_show(struct device *dev,
                                       struct device_attribute *attr,
					char *buff)
#endif
{
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	struct device *dev = kobj_to_dev(kobj);
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	bool pl1 = !!intel_rps_read_throttle_reason_pl1(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%u\n", pl1);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t throttle_reason_pl2_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buff)
#else
static ssize_t throttle_reason_pl2_show(struct device *dev,
					struct device_attribute *attr,
					char *buff)
#endif
{
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	struct device *dev = kobj_to_dev(kobj);
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	bool pl2 = !!intel_rps_read_throttle_reason_pl2(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%u\n", pl2);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t throttle_reason_pl4_show(struct kobject *kobj,
                                       struct kobj_attribute *attr,
                                        char *buff)
#else
static ssize_t throttle_reason_pl4_show(struct device *dev,
                                       struct device_attribute *attr,
					char *buff)
#endif
{
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	struct device *dev = kobj_to_dev(kobj);
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	bool pl4 = !!intel_rps_read_throttle_reason_pl4(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%u\n", pl4);
}


#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t throttle_reason_thermal_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buff)
#else
static ssize_t throttle_reason_thermal_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buff)
#endif
{
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	struct device *dev = kobj_to_dev(kobj);
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	bool thermal = !!intel_rps_read_throttle_reason_thermal(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%u\n", thermal);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t throttle_reason_prochot_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buff)
#else
static ssize_t throttle_reason_prochot_show(struct device *dev,
                                           struct device_attribute *attr,
                                           char *buff)
#endif
{
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	struct device *dev = kobj_to_dev(kobj);
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	bool prochot = !!intel_rps_read_throttle_reason_prochot(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%u\n", prochot);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t throttle_reason_ratl_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buff)
#else
static ssize_t throttle_reason_ratl_show(struct device *dev,
                                        struct device_attribute *attr,
                                        char *buff)
#endif
{
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	struct device *dev = kobj_to_dev(kobj);
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	bool ratl = !!intel_rps_read_throttle_reason_ratl(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%u\n", ratl);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t throttle_reason_vr_thermalert_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buff)
#else
static ssize_t throttle_reason_vr_thermalert_show(struct device *dev,
                                                 struct device_attribute *attr,
                                                 char *buff)

#endif
{
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	struct device *dev = kobj_to_dev(kobj);
#endif

	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	bool thermalert = !!intel_rps_read_throttle_reason_vr_thermalert(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%u\n", thermalert);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t throttle_reason_vr_tdc_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buff)
#else
static ssize_t throttle_reason_vr_tdc_show(struct device *dev,
                                          struct device_attribute *attr,
                                          char *buff)
#endif
{
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	struct device *dev = kobj_to_dev(kobj);
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	bool tdc = !!intel_rps_read_throttle_reason_vr_tdc(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%u\n", tdc);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
#define INTEL_KOBJ_ATTR_RO(_name, _show) \
	struct i915_ext_attr_kobj dev_attr_##_name = \
	{ __ATTR(_name, 0444, i915_sysfs_show_kobj, NULL), _show, NULL}

#define INTEL_KOBJ_ATTR_WO(_name, _store) \
	struct i915_ext_attr_kobj dev_attr_##_name = \
	{ __ATTR(_name, 0200, NULL, i915_sysfs_store_kobj), NULL, _store}


#define INTEL_KOBJ_ATTR_RW(_name, _mode, _show, _store) \
	struct i915_ext_attr_kobj dev_attr_##_name = \
	{ __ATTR(_name, _mode, i915_sysfs_show_kobj, i915_sysfs_store_kobj), _show, _store}
#endif
/* dgfx sysfs files under directory <dev>/gt/gt<i>/ */
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static INTEL_KOBJ_ATTR_RO(rapl_PL1_freq_mhz, rapl_PL1_freq_mhz_show);
static INTEL_KOBJ_ATTR_RO(punit_req_freq_mhz, punit_req_freq_mhz_show);
static INTEL_KOBJ_ATTR_RO(throttle_reason_status, throttle_reason_status_show);
static INTEL_KOBJ_ATTR_RO(throttle_reason_pl1, throttle_reason_pl1_show);
static INTEL_KOBJ_ATTR_RO(throttle_reason_pl2, throttle_reason_pl2_show);
static INTEL_KOBJ_ATTR_RO(throttle_reason_pl4, throttle_reason_pl4_show);
static INTEL_KOBJ_ATTR_RO(throttle_reason_thermal, throttle_reason_thermal_show);
static INTEL_KOBJ_ATTR_RO(throttle_reason_prochot, throttle_reason_prochot_show);
static INTEL_KOBJ_ATTR_RO(throttle_reason_ratl, throttle_reason_ratl_show);
static INTEL_KOBJ_ATTR_RO(throttle_reason_vr_thermalert, throttle_reason_vr_thermalert_show);
static INTEL_KOBJ_ATTR_RO(throttle_reason_vr_tdc, throttle_reason_vr_tdc_show);
#else
/* gen12+ sysfs files under directory <dev>/gt/gt<i>/ */
static I915_DEVICE_ATTR_RO(rapl_PL1_freq_mhz, rapl_PL1_freq_mhz_show);
static I915_DEVICE_ATTR_RO(punit_req_freq_mhz, punit_req_freq_mhz_show);
static I915_DEVICE_ATTR_RO(throttle_reason_status, throttle_reason_status_show);
static I915_DEVICE_ATTR_RO(throttle_reason_pl1, throttle_reason_pl1_show);
static I915_DEVICE_ATTR_RO(throttle_reason_pl2, throttle_reason_pl2_show);
static I915_DEVICE_ATTR_RO(throttle_reason_pl4, throttle_reason_pl4_show);
static I915_DEVICE_ATTR_RO(throttle_reason_thermal, throttle_reason_thermal_show);
static I915_DEVICE_ATTR_RO(throttle_reason_prochot, throttle_reason_prochot_show);
static I915_DEVICE_ATTR_RO(throttle_reason_ratl, throttle_reason_ratl_show);
static I915_DEVICE_ATTR_RO(throttle_reason_vr_thermalert, throttle_reason_vr_thermalert_show);
static I915_DEVICE_ATTR_RO(throttle_reason_vr_tdc, throttle_reason_vr_tdc_show);
#endif

static const struct attribute *freq_attrs[] = {
	&dev_attr_punit_req_freq_mhz.attr.attr,
	&dev_attr_throttle_reason_status.attr.attr,
	&dev_attr_throttle_reason_pl1.attr.attr,
	&dev_attr_throttle_reason_pl2.attr.attr,
	&dev_attr_throttle_reason_pl4.attr.attr,
	&dev_attr_throttle_reason_thermal.attr.attr,
	&dev_attr_throttle_reason_prochot.attr.attr,
	&dev_attr_throttle_reason_ratl.attr.attr,
	&dev_attr_throttle_reason_vr_thermalert.attr.attr,
	&dev_attr_throttle_reason_vr_tdc.attr.attr,
	NULL
};

/*
 * Mem Frequency query interface -
 * sysfs files under directory <dev>/gt/gt<i>/
 */

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t mem_RP0_freq_mhz_show(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      char *buff)
#else
static ssize_t mem_RP0_freq_mhz_show(struct device *dev,
                                     struct device_attribute *attr,
				      char *buff)
#endif
{
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	struct device *dev = kobj_to_dev(kobj);
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 val;
	int err;

	err = snb_pcode_read_p(gt->uncore, XEHPSDV_PCODE_FREQUENCY_CONFIG,
			       PCODE_MBOX_FC_SC_READ_FUSED_P0,
			       PCODE_MBOX_DOMAIN_HBM, &val);
	if (err)
		return err;

	/* data_out - Fused P0 for domain ID in units of 50 MHz */
	val *= GT_FREQUENCY_MULTIPLIER;

	return sysfs_emit(buff, "%u\n", val);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t mem_RPn_freq_mhz_show(struct kobject *kobj,
				       struct kobj_attribute *attr,
				      char *buff)
#else
static ssize_t mem_RPn_freq_mhz_show(struct device *dev,
                                     struct device_attribute *attr,
				      char *buff)
#endif
{
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	struct device *dev = kobj_to_dev(kobj);
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 val;
	int err;

	err = snb_pcode_read_p(gt->uncore, XEHPSDV_PCODE_FREQUENCY_CONFIG,
			       PCODE_MBOX_FC_SC_READ_FUSED_PN,
			       PCODE_MBOX_DOMAIN_HBM, &val);
	if (err)
		return err;

	/* data_out - Fused P0 for domain ID in units of 50 MHz */
	val *= GT_FREQUENCY_MULTIPLIER;

	return sysfs_emit(buff, "%u\n", val);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static INTEL_KOBJ_ATTR_RO(mem_RP0_freq_mhz, mem_RP0_freq_mhz_show);
static INTEL_KOBJ_ATTR_RO(mem_RPn_freq_mhz, mem_RPn_freq_mhz_show);
#else
static I915_DEVICE_ATTR_RO(mem_RP0_freq_mhz, mem_RP0_freq_mhz_show);
static I915_DEVICE_ATTR_RO(mem_RPn_freq_mhz, mem_RPn_freq_mhz_show);
#endif

static const struct attribute *mem_freq_attrs[] = {
	&dev_attr_mem_RP0_freq_mhz.attr.attr,
	&dev_attr_mem_RPn_freq_mhz.attr.attr,
	NULL
};

/*
 * PVC Performance control/query interface -
 * sysfs files under directory <dev>/gt/gt<i>/
 */
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t freq_factor_scale_show(struct kobject *kobj,
				      struct kobj_attribute *attr,
				       char *buff)
#else
static ssize_t freq_factor_scale_show(struct device *dev,
                                     struct device_attribute *attr,
                                     char *buff)

#endif
{
	return sysfs_emit(buff, "%s\n", U8_8_SCALE_TO_VALUE);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t base_freq_factor_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
#else
static ssize_t base_freq_factor_show(struct device *dev,
                                    struct device_attribute *attr,
                                    char *buff)
{
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 val;
	int err;

	err = snb_pcode_read_p(gt->uncore, PVC_PCODE_QOS_MULTIPLIER_GET,
			       PCODE_MBOX_DOMAIN_CHIPLET,
			       PCODE_MBOX_DOMAIN_BASE, &val);
	if (err)
		return err;

	val &= U8_8_VAL_MASK;

	return sysfs_emit(buff, "%u\n", val);
}
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t base_freq_factor_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buff, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
#else
static ssize_t base_freq_factor_store(struct device *dev,
                                     struct device_attribute *attr,
                                     const char *buff, size_t count)
{
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 val;
	int err;

	err = kstrtou32(buff, 0, &val);
	if (err)
		return err;

	if (val > U8_8_VAL_MASK)
		return -EINVAL;

	err = snb_pcode_write_p(gt->uncore, PVC_PCODE_QOS_MULTIPLIER_SET,
			      PCODE_MBOX_DOMAIN_CHIPLET,
			      PCODE_MBOX_DOMAIN_BASE, val);
	if (err)
		return err;

	return count;
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t base_RP0_freq_mhz_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
#else
static ssize_t base_RP0_freq_mhz_show(struct device *dev,
                                     struct device_attribute *attr,
                                     char *buff)
{
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 val;
	int err;

	err = snb_pcode_read_p(gt->uncore, XEHPSDV_PCODE_FREQUENCY_CONFIG,
			       PCODE_MBOX_FC_SC_READ_FUSED_P0,
			       PCODE_MBOX_DOMAIN_BASE, &val);
	if (err)
		return err;

	/* data_out - Fused P0 for domain ID in units of 50 MHz */
	val *= GT_FREQUENCY_MULTIPLIER;

	return sysfs_emit(buff, "%u\n", val);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t base_RPn_freq_mhz_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
#else
static ssize_t base_RPn_freq_mhz_show(struct device *dev,
                                    struct device_attribute *attr,
                                    char *buff)
{
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 val;
	int err;

	err = snb_pcode_read_p(gt->uncore, XEHPSDV_PCODE_FREQUENCY_CONFIG,
			       PCODE_MBOX_FC_SC_READ_FUSED_PN,
			       PCODE_MBOX_DOMAIN_BASE, &val);
	if (err)
		return err;

	/* data_out - Fused Pn for domain ID in units of 50 MHz */
	val *= GT_FREQUENCY_MULTIPLIER;

	return sysfs_emit(buff, "%u\n", val);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t base_act_freq_mhz_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
#else
static ssize_t base_act_freq_mhz_show(struct device *dev,
                                     struct device_attribute *attr,
                                     char *buff)
{
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);

	/* On PVC this returns the base die frequency */
	return sysfs_emit(buff, "%d\n",
			  intel_rps_read_actual_frequency(&gt->rps));
}

static u32 media_ratio_mode_to_factor(u32 mode)
{
	const u32 factor[] = {
		[SLPC_MEDIA_RATIO_MODE_DYNAMIC_CONTROL] = 0x0,
		[SLPC_MEDIA_RATIO_MODE_FIXED_ONE_TO_ONE] = 0x100,
		[SLPC_MEDIA_RATIO_MODE_FIXED_ONE_TO_TWO] = 0x80
	};

	return factor[mode];
}
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t media_freq_factor_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
#else
static ssize_t media_freq_factor_show(struct device *dev,
                                     struct device_attribute *attr,
                                     char *buff)
{
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	struct intel_guc_slpc *slpc = &gt->uc.guc.slpc;
	u32 mode;

	if (IS_XEHPSDV(gt->i915) &&
	    slpc->media_ratio_mode == SLPC_MEDIA_RATIO_MODE_DYNAMIC_CONTROL) {
		/*
		 * For PVC/XEHPSDV dynamic mode 0xA008:13 does not contain the
		 * actual media ratio, just return the cached media ratio
		*/
		mode = slpc->media_ratio_mode;
	} else {
		/* 0xA008:13 value 0 represents 1:2 and 1 represents 1:1 */
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
		mode = _with_pm_intel_dev_read(kobj, attr, GEN6_RPNSWREQ);
#else
		mode = _with_pm_intel_dev_read(dev, attr, GEN6_RPNSWREQ);
#endif
		mode = REG_FIELD_GET(GEN12_MEDIA_FREQ_RATIO, mode) ?
			SLPC_MEDIA_RATIO_MODE_FIXED_ONE_TO_ONE :
			SLPC_MEDIA_RATIO_MODE_FIXED_ONE_TO_TWO;
	}

	return sysfs_emit(buff, "%u\n", media_ratio_mode_to_factor(mode));
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t media_freq_factor_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buff, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
#else
static ssize_t media_freq_factor_store(struct device *dev,
                                      struct device_attribute *attr,
                                      const char *buff, size_t count)
{
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	struct intel_guc_slpc *slpc = &gt->uc.guc.slpc;
	u32 val, mode;
	int err;

	err = kstrtou32(buff, 0, &val);
	if (err)
		return err;

	switch (val) {
	case 0x0:
		/* SLPC_MEDIA_RATIO_MODE_DYNAMIC_CONTROL is not supported on PVC */
		if (IS_PONTEVECCHIO(gt->i915))
			return -EINVAL;
		mode = SLPC_MEDIA_RATIO_MODE_DYNAMIC_CONTROL;
		break;
	case 0x80:
		mode = SLPC_MEDIA_RATIO_MODE_FIXED_ONE_TO_TWO;
		break;
	case 0x100:
		mode = SLPC_MEDIA_RATIO_MODE_FIXED_ONE_TO_ONE;
		break;
	default:
		return -EINVAL;
	}

	err = intel_guc_slpc_set_media_ratio_mode(slpc, mode);
	if (!err) {
		slpc->media_ratio_mode = mode;
		DRM_DEBUG("Set slpc->media_ratio_mode to %d", mode);
	}
	return err ?: count;
}
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t media_RP0_freq_mhz_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
#else
static ssize_t media_RP0_freq_mhz_show(struct device *dev,
                                      struct device_attribute *attr,
                                      char *buff)
{
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 val;
	int err;

	err = snb_pcode_read_p(gt->uncore, XEHPSDV_PCODE_FREQUENCY_CONFIG,
			       PCODE_MBOX_FC_SC_READ_FUSED_P0,
			       PCODE_MBOX_DOMAIN_MEDIAFF, &val);

	if (err)
		return err;

	/* data_out - Fused P0 for domain ID in units of 50 MHz */
	val *= GT_FREQUENCY_MULTIPLIER;

	return sysfs_emit(buff, "%u\n", val);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t media_RPn_freq_mhz_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
#else
static ssize_t media_RPn_freq_mhz_show(struct device *dev,
                                      struct device_attribute *attr,
                                      char *buff)
{
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 val;
	int err;

	err = snb_pcode_read_p(gt->uncore, XEHPSDV_PCODE_FREQUENCY_CONFIG,
			       PCODE_MBOX_FC_SC_READ_FUSED_PN,
			       PCODE_MBOX_DOMAIN_MEDIAFF, &val);

	if (err)
		return err;

	/* data_out - Fused P0 for domain ID in units of 50 MHz */
	val *= GT_FREQUENCY_MULTIPLIER;

	return sysfs_emit(buff, "%u\n", val);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t media_act_freq_mhz_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
#else
static ssize_t media_act_freq_mhz_show(struct device *dev,
                                      struct device_attribute *attr,
                                      char *buff)
{
#endif
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	struct intel_rps *rps = &gt->rps;
	i915_reg_t rgadr = PVC_MEDIA_PERF_STATUS;

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	u32 val = _with_pm_intel_dev_read(kobj, attr, rgadr);
#else
	u32 val = _with_pm_intel_dev_read(dev, attr, rgadr);
#endif
	/* Available from PVC B-step */
	val = REG_FIELD_GET(PVC_MEDIA_PERF_MEDIA_RATIO, val);
	val = intel_gpu_freq(rps, val);

	return sysfs_emit(buff, "%u\n", val);
}

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static INTEL_KOBJ_ATTR_RW(base_freq_factor, 0644, base_freq_factor_show,
		base_freq_factor_store);
static struct i915_ext_attr_kobj dev_attr_base_freq_factor_scale = {
	__ATTR(base_freq_factor.scale, 0444, i915_sysfs_show_kobj, NULL),
	freq_factor_scale_show, NULL
	};
static INTEL_KOBJ_ATTR_RO(base_RP0_freq_mhz, base_RP0_freq_mhz_show);
static INTEL_KOBJ_ATTR_RO(base_RPn_freq_mhz, base_RPn_freq_mhz_show);
static INTEL_KOBJ_ATTR_RO(base_act_freq_mhz, base_act_freq_mhz_show);

static INTEL_KOBJ_ATTR_RW(media_freq_factor, 0644, media_freq_factor_show,
		media_freq_factor_store);
static struct i915_ext_attr_kobj dev_attr_media_freq_factor_scale = {
	__ATTR(media_freq_factor.scale, 0444, i915_sysfs_show_kobj, NULL),
	freq_factor_scale_show, NULL
	};
static INTEL_KOBJ_ATTR_RO(media_RP0_freq_mhz, media_RP0_freq_mhz_show);
static INTEL_KOBJ_ATTR_RO(media_RPn_freq_mhz, media_RPn_freq_mhz_show);
static INTEL_KOBJ_ATTR_RO(media_act_freq_mhz, media_act_freq_mhz_show);
#else
static I915_DEVICE_ATTR_RW(base_freq_factor, 0644, base_freq_factor_show, base_freq_factor_store);
static struct i915_ext_attr dev_attr_base_freq_factor_scale =
       {__ATTR(base_freq_factor.scale, 0444, i915_sysfs_show, NULL), freq_factor_scale_show};
static I915_DEVICE_ATTR_RO(base_RP0_freq_mhz, base_RP0_freq_mhz_show);
static I915_DEVICE_ATTR_RO(base_RPn_freq_mhz, base_RPn_freq_mhz_show);
static I915_DEVICE_ATTR_RO(base_act_freq_mhz, base_act_freq_mhz_show);

static I915_DEVICE_ATTR_RW(media_freq_factor, 0644, media_freq_factor_show, media_freq_factor_store);
static struct i915_ext_attr dev_attr_media_freq_factor_scale =
       {__ATTR(media_freq_factor.scale, 0444, i915_sysfs_show, NULL), freq_factor_scale_show};
static I915_DEVICE_ATTR_RO(media_RP0_freq_mhz, media_RP0_freq_mhz_show);
static I915_DEVICE_ATTR_RO(media_RPn_freq_mhz, media_RPn_freq_mhz_show);
static I915_DEVICE_ATTR_RO(media_act_freq_mhz, media_act_freq_mhz_show);
#endif

static const struct attribute *pvc_perf_power_attrs[] = {
	&dev_attr_base_freq_factor.attr.attr,
	&dev_attr_base_freq_factor_scale.attr.attr,
	&dev_attr_base_RP0_freq_mhz.attr.attr,
	&dev_attr_base_RPn_freq_mhz.attr.attr,
	&dev_attr_base_act_freq_mhz.attr.attr,
	NULL
};

static const struct attribute *media_perf_power_attrs[] = {
	&dev_attr_media_freq_factor.attr.attr,
	&dev_attr_media_freq_factor_scale.attr.attr,
	&dev_attr_media_RP0_freq_mhz.attr.attr,
	&dev_attr_media_RPn_freq_mhz.attr.attr,
	NULL
};

static ssize_t throttle_reason_thermal_swing_show(struct device *dev,
						  struct device_attribute *attr,
						  char *buff)
{
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	struct kobject *kobj = &dev->kobj;
#endif

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
	u32 en8 = _with_pm_intel_dev_read(kobj, (struct kobj_attribute *)attr, PVC_CR_RMID_ENERGY_8);
	u32 en9 = _with_pm_intel_dev_read(kobj, (struct kobj_attribute *)attr, PVC_CR_RMID_ENERGY_9);
#else
	u32 en8 = _with_pm_intel_dev_read(dev, attr, PVC_CR_RMID_ENERGY_8);
	u32 en9 = _with_pm_intel_dev_read(dev, attr, PVC_CR_RMID_ENERGY_9);
#endif

	/*
	 * Whenever these counters are out of sync, thermal swing throttling
	 * is active
	 */
	bool thermal_swing = en8 - en9;

	return scnprintf(buff, PAGE_SIZE, "%u\n", thermal_swing);
}

static I915_DEVICE_ATTR_RO(throttle_reason_thermal_swing,
			   throttle_reason_thermal_swing_show);

static const struct attribute *pvc_thermal_attrs[] = {
	&dev_attr_throttle_reason_thermal_swing.attr.attr,
	NULL
};

static ssize_t sys_pwr_balance_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	i915_reg_t rgadr = PVC_GT0_PACKAGE_SYS_PWR_BAL_FACTOR;
	intel_wakeref_t wakeref;
	ssize_t ret;
	u32 val;

	ret = kstrtou32(buf, 0, &val);
	if (ret)
		return ret;

	val = REG_FIELD_GET(PVC_SYS_PWR_BAL_FACTOR_MASK, val);

	with_intel_runtime_pm(gt->uncore->rpm, wakeref)
		intel_uncore_rmw(gt->uncore, rgadr,
				 PVC_SYS_PWR_BAL_FACTOR_MASK, val);
	return count;
}
static ssize_t sys_pwr_balance_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
       struct kobject *kobj = &dev->kobj;
#endif

       i915_reg_t rgadr = PVC_GT0_PACKAGE_SYS_PWR_BAL_FACTOR;
#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
       u32 val = _with_pm_intel_dev_read(kobj, (struct kobj_attribute *)attr, rgadr);
#else
	u32 val = _with_pm_intel_dev_read(dev, attr, rgadr);
#endif

	val = REG_FIELD_GET(PVC_SYS_PWR_BAL_FACTOR_MASK, val);

	return scnprintf(buf, PAGE_SIZE, "%d\n", val );
}
static I915_DEVICE_ATTR_RW(sys_pwr_balance, 0644, sys_pwr_balance_show, sys_pwr_balance_store);

/* sysfs file <dev>/sys_pwr_balance */
static const struct attribute * const sys_pwr_balance_attrs[] = {
	&dev_attr_sys_pwr_balance.attr.attr,
	NULL
};
#endif

static ssize_t
default_min_freq_mhz_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_gt *gt = kobj_to_gt(kobj->parent);

	return sysfs_emit(buf, "%d\n", gt->rps_defaults.min_freq);
}

static struct i915_kobj_ext_attr default_min_freq_mhz = {
	__ATTR(rps_min_freq_mhz, 0444, i915_kobj_sysfs_show, NULL),
	default_min_freq_mhz_show, NULL};

static ssize_t
default_max_freq_mhz_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_gt *gt = kobj_to_gt(kobj->parent);

	return sysfs_emit(buf, "%d\n", gt->rps_defaults.max_freq);
}

static struct i915_kobj_ext_attr default_max_freq_mhz = {
	__ATTR(rps_max_freq_mhz, 0444, i915_kobj_sysfs_show, NULL),
	default_max_freq_mhz_show, NULL};

static ssize_t
default_boost_freq_mhz_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_gt *gt = kobj_to_gt(kobj->parent);

	return sysfs_emit(buf, "%d\n", gt->rps_defaults.boost_freq);
}

static struct i915_kobj_ext_attr default_boost_freq_mhz = {
	__ATTR(rps_boost_freq_mhz, 0444, i915_kobj_sysfs_show, NULL),
	default_boost_freq_mhz_show, NULL};

#if IS_ENABLED(CONFIG_PM)
static ssize_t
default_media_freq_factor_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_gt *gt = kobj_to_gt(kobj->parent);

	return sysfs_emit(buf, "%d\n",
			  media_ratio_mode_to_factor(gt->rps_defaults.media_ratio_mode));
}

static struct i915_kobj_ext_attr default_media_freq_factor = {
	__ATTR(media_freq_factor, 0444, i915_kobj_sysfs_show, NULL),
	default_media_freq_factor_show, NULL};
#endif

static ssize_t
default_base_freq_factor_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_gt *gt = kobj_to_gt(kobj->parent);

	return sysfs_emit(buf, "%d\n", gt->rps_defaults.base_freq_factor);
}

static struct i915_kobj_ext_attr default_base_freq_factor = {
	__ATTR(base_freq_factor, 0444, i915_kobj_sysfs_show, NULL),
	default_base_freq_factor_show, NULL};

static const struct attribute * const rps_defaults_attrs[] = {
	&default_min_freq_mhz.attr.attr,
	&default_max_freq_mhz.attr.attr,
	&default_boost_freq_mhz.attr.attr,
	NULL
};

static ssize_t
i915_sysfs_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t value;
	struct i915_ext_attr *ea = container_of(attr, struct i915_ext_attr, attr);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);

	/* Wa_16015476723 & Wa_16015666671 */
	pvc_wa_disallow_rc6(gt->i915);

	value = ea->i915_show(dev, attr, buf);

	pvc_wa_allow_rc6(gt->i915);

	return value;
}

static ssize_t
i915_sysfs_store(struct device *dev, struct device_attribute *attr, const char
		 *buf, size_t count)
{
	struct i915_ext_attr *ea = container_of(attr, struct i915_ext_attr, attr);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);

	/* Wa_16015476723 & Wa_16015666671 */
	pvc_wa_disallow_rc6(gt->i915);

	count = ea->i915_store(dev, attr, buf, count);

	pvc_wa_allow_rc6(gt->i915);

	return count;
}


#ifdef BPM_DEVICE_ATTR_NOT_PRESENT
static ssize_t
i915_sysfs_show_kobj(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	ssize_t value;
	struct device *dev = kobj_to_dev(kobj);
	struct i915_ext_attr_kobj *ea = container_of(attr, struct i915_ext_attr_kobj, attr);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);

	/* Wa_16015476723 & Wa_16015666671 */
	pvc_wa_disallow_rc6(gt->i915);

	value = ea->i915_show_kobj(kobj, attr, buf);

	pvc_wa_allow_rc6(gt->i915);

	return value;
}

static ssize_t
i915_sysfs_store_kobj(struct kobject *kobj, struct kobj_attribute *attr, const char
		 *buf, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct i915_ext_attr_kobj *ea = container_of(attr, struct i915_ext_attr_kobj, attr);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);

	/* Wa_16015476723 & Wa_16015666671 */
	pvc_wa_disallow_rc6(gt->i915);

	count = ea->i915_store_kobj(kobj, attr, buf, count);

	pvc_wa_allow_rc6(gt->i915);

	return count;
}
#endif

static ssize_t
i915_kobj_sysfs_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	ssize_t value;
	struct i915_kobj_ext_attr *ea = container_of(attr, struct
						     i915_kobj_ext_attr, attr);
	struct intel_gt *gt = kobj_to_gt(kobj->parent);

	/* Wa_16015476723 & Wa_16015666671 */
	pvc_wa_disallow_rc6(gt->i915);

	value = ea->i915_kobj_show(kobj, attr, buf);

	pvc_wa_allow_rc6(gt->i915);

	return value;
}

static int add_rps_defaults(struct intel_gt *gt)
{
	return sysfs_create_files(gt->sysfs_defaults, rps_defaults_attrs);
}

static void set_default_base_freq_factor(struct intel_gt *gt)
{
	/* 0x100 corresponds to a factor value of 1.0 */
	gt->rps_defaults.base_freq_factor = 0x100;
}

static int intel_sysfs_rps_init_gt(struct intel_gt *gt, struct kobject *kobj)
{
	int ret;

	if (GRAPHICS_VER(gt->i915) >= 12) {
#if IS_ENABLED(CONFIG_PM)
		ret = sysfs_create_files(kobj, freq_attrs);
		if (ret)
			return ret;
#endif
	}

	if (IS_PONTEVECCHIO(gt->i915)) {
#if IS_ENABLED(CONFIG_PM)
		ret = sysfs_create_files(kobj, pvc_thermal_attrs);
		if (ret)
			return ret;

		ret = sysfs_create_files(kobj, pvc_perf_power_attrs);
		if (ret)
			return ret;
#endif

		set_default_base_freq_factor(gt);
		ret = sysfs_create_file(gt->sysfs_defaults, &default_base_freq_factor.attr.attr);
		if (ret)
			return ret;
	}

#if IS_ENABLED(CONFIG_PM)
	if (IS_PVC_BD_STEP(gt->i915, STEP_B0, STEP_FOREVER)) {
		ret = sysfs_create_file(kobj, &dev_attr_media_act_freq_mhz.attr.attr);
		if (ret)
			return ret;
	}

	if (IS_DGFX(gt->i915)) {
		ret = sysfs_create_file(kobj, &dev_attr_rapl_PL1_freq_mhz.attr.attr);
		if (ret)
			return ret;
	}

	if (IS_DGFX(gt->i915) && !IS_DG1(gt->i915) && !IS_DG2(gt->i915)) {
		ret = sysfs_create_files(kobj, mem_freq_attrs);
		if (ret)
			return ret;
	}

	if (HAS_MEDIA_RATIO_MODE(gt->i915) && intel_uc_uses_guc_slpc(&gt->uc)) {
		ret = sysfs_create_files(kobj, media_perf_power_attrs);
		if (ret)
			return ret;

		ret = sysfs_create_file(gt->sysfs_defaults, &default_media_freq_factor.attr.attr);
		if (ret)
			return ret;
	}

#endif
	return add_rps_defaults(gt);
}

#if IS_ENABLED(CONFIG_PM)
/* seconds */
#define POWER_STATE_PW_DELAY_MIN 5

static int iaf_gt_set_power_state(struct device *dev, bool enable)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, "no-name");
	u32 pcode_cmd = enable ?
		PCODE_MBOX_CD_TRIGGER_SHUTDOWN_DATA_REENABLE :
		PCODE_MBOX_CD_TRIGGER_SHUTDOWN_DATA_SHUTDOWN;
	u32 status = enable ?
		PCODE_MBOX_CD_STATUS_DATA_ONLINE :
		PCODE_MBOX_CD_STATUS_DATA_SHUTDOWN;
	u32 iaf_status;
	int retry = 0;
	int ret;

	/* enable/disable the IAF device */
	ret = snb_pcode_write_p(gt->uncore, PCODE_MBOX_CD, PCODE_MBOX_CD_TRIGGER_SHUTDOWN,
				0, pcode_cmd);
	if (ret)
		return ret;

	ret = snb_pcode_read_p(gt->uncore, PCODE_MBOX_CD, PCODE_MBOX_CD_STATUS, 0,
			       &iaf_status);

	/*
	 * Power on can be on the order of 10s of seconds.  Try to be
	 * optimistic with 5.
	 */
	while (!ret && iaf_status != status && retry < 10) {
		ssleep(POWER_STATE_PW_DELAY_MIN);
		ret = snb_pcode_read_p(gt->uncore, PCODE_MBOX_CD, PCODE_MBOX_CD_STATUS, 0,
				       &iaf_status);
		retry++;
	}

	if (retry == 10)
		ret = -EIO;

	return ret;
}

static ssize_t iaf_power_enable_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	bool enable;
	int ret;

	/* This should not be possble, makesure of it */
	GEM_BUG_ON(IS_PVC_BD_STEP(gt->i915, STEP_A0, STEP_B0));

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;

	if (gt->i915->intel_iaf.power_enabled == enable)
		return count;

	/*
	 * If the driver is still present, do not allow the disable.
	 * The driver MUST be unbound first
	 */
	mutex_lock(&gt->i915->intel_iaf.power_mutex);
	if (gt->i915->intel_iaf.handle && !enable) {
		ret = -EBUSY;
		goto unlock;
	}

	ret = iaf_gt_set_power_state(dev, enable);
	if (ret)
		goto unlock;

	/* remember the new state */
	gt->i915->intel_iaf.power_enabled = enable;

unlock:
	mutex_unlock(&gt->i915->intel_iaf.power_mutex);

	return ret ?: count;
}

static ssize_t iaf_power_enable_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);

	return sysfs_emit(buf, "%d\n", gt->i915->intel_iaf.power_enabled);
}

static I915_DEVICE_ATTR_RW(iaf_power_enable, 0644, iaf_power_enable_show, iaf_power_enable_store);

static const struct attribute * const iaf_attrs[] = {
	&dev_attr_iaf_power_enable.attr.attr,
	NULL
};
#endif
static int intel_sysfs_rps_init(struct intel_gt *gt, struct kobject *kobj)
{
	const struct attribute * const *attrs;
	int ret;

	if (IS_SRIOV_VF(gt->i915))
		return 0;

	if (is_object_gt(kobj))
		attrs = gen6_rps_attrs;
	else
		attrs = gen6_gt_attrs;
	ret = sysfs_create_files(kobj, attrs);
	if (ret)
		return ret;

#if IS_ENABLED(CONFIG_PM)
	if (IS_VALLEYVIEW(gt->i915) || IS_CHERRYVIEW(gt->i915)) {
		ret = sysfs_create_file(kobj, &dev_attr_vlv_rpe_freq_mhz.attr.attr);
		if (ret)
			return ret;
	}
#endif
	if (is_object_gt(kobj)) {
		/* attributes for only directory gt/gt<i> */
		ret = intel_sysfs_rps_init_gt(gt, kobj);
		if (ret)
			return ret;
	} else if (IS_PONTEVECCHIO(gt->i915)) {

#if IS_ENABLED(CONFIG_PM)
		ret = sysfs_create_files(kobj, sys_pwr_balance_attrs);
		if (ret)
			return ret;

		if (IS_PVC_BD_STEP(gt->i915, STEP_B0, STEP_FOREVER) &&
		    HAS_IAF(gt->i915)) {
			ret = sysfs_create_files(kobj, iaf_attrs);
			if (ret)
				return ret;
		}
#endif
	}

	return 0;
}

/*
 * intel_gt_sysfs_pm_init()
 * @gt: The gt being processed.
 * @kobj: The kobj in sysfs to which created files will be attached.
 *
 * Called twice:
 * - Once with kobj == the device parent directory and gt == gt0.
 *   Populates those things whose parent directory is kobj.
 * - Once per gt, with kobj == that gt's kobject = gt/gt<i>
 *   Populates those things whose parent directory is gt/gt<i>.
 */
void intel_gt_sysfs_pm_init(struct intel_gt *gt, struct kobject *kobj)
{
	int ret;

	intel_sysfs_rc6_init(gt, kobj);

	if (GRAPHICS_VER(gt->i915) >= 6) {
		ret = intel_sysfs_rps_init(gt, kobj);
		if (ret) {
			drm_err(&gt->i915->drm,
				"failed to create gt%u RPS sysfs files",
				gt->info.id);
		}
	}
}
