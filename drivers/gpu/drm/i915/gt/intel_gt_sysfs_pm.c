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

#define U8_8_VAL_MASK           0xffff
#define U8_8_SCALE_TO_VALUE     "0.00390625"

static u32 _with_pm_intel_dev_read(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   i915_reg_t rgadr)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	struct intel_uncore *uncore = gt->uncore;
	intel_wakeref_t wakeref;
	u32 regval;

	with_intel_runtime_pm(uncore->rpm, wakeref)
		regval = intel_uncore_read(uncore, rgadr);

	return regval;
}

#ifdef CONFIG_PM
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
	u32 rc6_residency = get_residency(gt, GEN6_GT_GFX_RC6);

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

static DEVICE_ATTR_RW(rc6_enable);
static DEVICE_ATTR_RO(rc6_residency_ms);
static DEVICE_ATTR_RO(rc6p_residency_ms);
static DEVICE_ATTR_RO(rc6pp_residency_ms);
static DEVICE_ATTR_RO(media_rc6_residency_ms);

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
#define INTEL_KOBJ_GT_ATTR(_name, __mode, __show, __store) \
struct kobj_attribute dev_attr_gt_##_name =   \
		__ATTR(_name, __mode, gt_##__show, __store)

#define INTEL_KOBJ_GT_ATTR_RO(_name)				\
		INTEL_KOBJ_GT_ATTR(_name, 0444, _name##_show, NULL)
#define INTEL_KOBJ_GT_ATTR_RW(_name)				\
		INTEL_KOBJ_GT_ATTR(_name, 0644, _name##_show, gt_##_name##_store)
static INTEL_KOBJ_GT_ATTR_RW(rc6_enable);
static INTEL_KOBJ_GT_ATTR_RO(rc6_residency_ms);
static INTEL_KOBJ_GT_ATTR_RO(rc6p_residency_ms);
static INTEL_KOBJ_GT_ATTR_RO(rc6pp_residency_ms);
static INTEL_KOBJ_GT_ATTR_RO(media_rc6_residency_ms);

static struct attribute *gt_rc6_attrs[] = {
	&dev_attr_gt_rc6_enable.attr,
	&dev_attr_gt_rc6_residency_ms.attr,
	NULL
};

static struct attribute *gt_rc6p_attrs[] = {
	&dev_attr_gt_rc6p_residency_ms.attr,
	&dev_attr_gt_rc6pp_residency_ms.attr,
	NULL
};

static struct attribute *gt_media_rc6_attrs[] = {
	&dev_attr_gt_media_rc6_residency_ms.attr,
	NULL
};

static struct attribute *rc6_attrs[] = {
	&dev_attr_rc6_enable.attr,
	&dev_attr_rc6_residency_ms.attr,
	NULL
};

static struct attribute *rc6p_attrs[] = {
	&dev_attr_rc6p_residency_ms.attr,
	&dev_attr_rc6pp_residency_ms.attr,
	NULL
};

static struct attribute *media_rc6_attrs[] = {
	&dev_attr_media_rc6_residency_ms.attr,
	NULL
};

static const struct attribute_group rc6_attr_group[] = {
	{ .name = power_group_name, .attrs = rc6_attrs },
	{ .attrs = gt_rc6_attrs }
};

static const struct attribute_group rc6p_attr_group[] = {
	{ .name = power_group_name, .attrs = rc6p_attrs },
	{ .attrs = gt_rc6p_attrs }
};

static const struct attribute_group media_rc6_attr_group[] = {
	{ .name = power_group_name, .attrs = media_rc6_attrs },
	{ .attrs = gt_media_rc6_attrs }
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
#else
static void intel_sysfs_rc6_init(struct intel_gt *gt, struct kobject *kobj)
{
}
#endif /* CONFIG_PM */

static ssize_t vlv_rpe_freq_mhz_show(struct device *dev,
					struct device_attribute *attr, char *buff)
{
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	struct intel_rps *rps = &gt->rps;

	return scnprintf(buff, PAGE_SIZE, "%d\n",
			intel_gpu_freq(rps, rps->efficient_freq));
}


static ssize_t act_freq_mhz_show(struct device *dev,
				 struct device_attribute *attr, char *buff)
{
	struct kobject *kobj = &dev->kobj;
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);

	/*
	 * For PVC show chiplet freq which is the "base" frequency, all other
	 * gt/rps frequency attributes also apply to the chiplet.
	 * intel_rps_read_actual_frequency is used in base_act_freq_mhz_show
	 */
	if (IS_PONTEVECCHIO(gt->i915)) {
		struct intel_rps *rps = &gt->rps;
		i915_reg_t rgadr = GEN12_RPSTAT1;
		u32 val = _with_pm_intel_dev_read(kobj, (struct kobj_attribute *)attr, rgadr);

		val = REG_FIELD_GET(PVC_RPSTAT1_CHIPLET_FREQ, val);
		val = intel_gpu_freq(rps, val);

		return sysfs_emit(buff, "%u\n", val);
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

/* sysfs dual-location files <dev>/vlv_rpe_freq_mhz and <dev>/gt/gt0/vlv_rpe_freq_mhz */
static DEVICE_ATTR_RO(vlv_rpe_freq_mhz);

/* sysfs dual-location files <dev>/gt_* and <dev>/gt/gt<i>/rps_* */

#define INTEL_GT_RPS_SYSFS_ATTR(_name, __mode, __show, __store) \
	static struct device_attribute dev_attr_gt_##_name =    \
		__ATTR(gt_##_name, __mode, __show, __store)

/* Note: rps_ and gt_ share common show and store functions. */
#define INTEL_GT_RPS_SYSFS_ATTR_RO(_name)				\
		INTEL_GT_RPS_SYSFS_ATTR(_name, 0444, _name##_show, NULL)
#define INTEL_GT_RPS_SYSFS_ATTR_RW(_name)				\
		INTEL_GT_RPS_SYSFS_ATTR(_name, 0644, _name##_show, _name##_store)

INTEL_GT_RPS_SYSFS_ATTR_RO(act_freq_mhz);
INTEL_GT_RPS_SYSFS_ATTR_RO(cur_freq_mhz);
INTEL_GT_RPS_SYSFS_ATTR_RW(boost_freq_mhz);
INTEL_GT_RPS_SYSFS_ATTR_RW(max_freq_mhz);
INTEL_GT_RPS_SYSFS_ATTR_RW(min_freq_mhz);
INTEL_GT_RPS_SYSFS_ATTR_RO(RP0_freq_mhz);
INTEL_GT_RPS_SYSFS_ATTR_RO(RP1_freq_mhz);
INTEL_GT_RPS_SYSFS_ATTR_RO(RPn_freq_mhz);

#define INTEL_RPS_SYSFS_ATTR(_name, __mode, __show, __store) \
static struct kobj_attribute dev_attr_rps_##_name =   \
		__ATTR(rps_##_name, __mode, rps_##__show, __store)

#define INTEL_RPS_SYSFS_ATTR_RO(_name)				\
		INTEL_RPS_SYSFS_ATTR(_name, 0444, _name##_show, NULL)
#define INTEL_RPS_SYSFS_ATTR_RW(_name)				\
		INTEL_RPS_SYSFS_ATTR(_name, 0644, _name##_show, rps_##_name##_store)

INTEL_RPS_SYSFS_ATTR_RO(act_freq_mhz);
INTEL_RPS_SYSFS_ATTR_RO(cur_freq_mhz);
INTEL_RPS_SYSFS_ATTR_RW(boost_freq_mhz);
INTEL_RPS_SYSFS_ATTR_RW(max_freq_mhz);
INTEL_RPS_SYSFS_ATTR_RW(min_freq_mhz);
INTEL_RPS_SYSFS_ATTR_RO(RP0_freq_mhz);
INTEL_RPS_SYSFS_ATTR_RO(RP1_freq_mhz);
INTEL_RPS_SYSFS_ATTR_RO(RPn_freq_mhz);

#define GEN6_ATTR(s) { \
		&dev_attr_##s##_act_freq_mhz.attr, \
		&dev_attr_##s##_cur_freq_mhz.attr, \
		&dev_attr_##s##_boost_freq_mhz.attr, \
		&dev_attr_##s##_max_freq_mhz.attr, \
		&dev_attr_##s##_min_freq_mhz.attr, \
		&dev_attr_##s##_RP0_freq_mhz.attr, \
		&dev_attr_##s##_RP1_freq_mhz.attr, \
		&dev_attr_##s##_RPn_freq_mhz.attr, \
		NULL, \
	}

/* sysfs files <dev>/gt_* */
static const struct attribute * const gen6_rps_attrs[] = GEN6_ATTR(rps);

/* sysfs files <dev>/gt/gt<i>/rps_* */
static const struct attribute * const gen6_gt_attrs[]  = GEN6_ATTR(gt);

static ssize_t rapl_PL1_freq_mhz_show(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 rapl_pl1 = intel_rps_read_rapl_pl1_frequency(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%d\n", rapl_pl1);
}

static ssize_t punit_req_freq_mhz_show(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 swreq = intel_rps_get_requested_frequency(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%d\n", swreq);
}

static ssize_t throttle_reason_status_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	bool status = !!intel_rps_read_throttle_reason_status(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%u\n", status);
}

static ssize_t throttle_reason_pl1_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	bool pl1 = !!intel_rps_read_throttle_reason_pl1(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%u\n", pl1);
}

static ssize_t throttle_reason_pl2_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	bool pl2 = !!intel_rps_read_throttle_reason_pl2(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%u\n", pl2);
}

static ssize_t throttle_reason_pl4_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	bool pl4 = !!intel_rps_read_throttle_reason_pl4(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%u\n", pl4);
}

static ssize_t throttle_reason_thermal_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	bool thermal = !!intel_rps_read_throttle_reason_thermal(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%u\n", thermal);
}

static ssize_t throttle_reason_prochot_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	bool prochot = !!intel_rps_read_throttle_reason_prochot(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%u\n", prochot);
}

static ssize_t throttle_reason_ratl_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	bool ratl = !!intel_rps_read_throttle_reason_ratl(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%u\n", ratl);
}

static ssize_t throttle_reason_vr_thermalert_show(struct kobject *kobj,
						  struct kobj_attribute *attr,
						  char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	bool thermalert = !!intel_rps_read_throttle_reason_vr_thermalert(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%u\n", thermalert);
}

static ssize_t throttle_reason_vr_tdc_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	bool tdc = !!intel_rps_read_throttle_reason_vr_tdc(&gt->rps);

	return scnprintf(buff, PAGE_SIZE, "%u\n", tdc);
}

#define INTEL_KOBJ_ATTR(_name, __mode, __show, __store) \
struct kobj_attribute dev_attr_##_name =   \
               __ATTR(_name, __mode, __show, __store)

#define INTEL_KOBJ_ATTR_RO(_name)                              \
               INTEL_KOBJ_ATTR(_name, 0444, _name##_show, NULL)
#define INTEL_KOBJ_ATTR_RW(_name)                              \
               INTEL_KOBJ_ATTR(_name, 0644, _name##_show, _name##_store)


/* dgfx sysfs files under directory <dev>/gt/gt<i>/ */
static INTEL_KOBJ_ATTR_RO(rapl_PL1_freq_mhz);

/* gen12+ sysfs files under directory <dev>/gt/gt<i>/ */

static INTEL_KOBJ_ATTR_RO(punit_req_freq_mhz);
static INTEL_KOBJ_ATTR_RO(throttle_reason_status);
static INTEL_KOBJ_ATTR_RO(throttle_reason_pl1);
static INTEL_KOBJ_ATTR_RO(throttle_reason_pl2);
static INTEL_KOBJ_ATTR_RO(throttle_reason_pl4);
static INTEL_KOBJ_ATTR_RO(throttle_reason_thermal);
static INTEL_KOBJ_ATTR_RO(throttle_reason_prochot);
static INTEL_KOBJ_ATTR_RO(throttle_reason_ratl);
static INTEL_KOBJ_ATTR_RO(throttle_reason_vr_thermalert);
static INTEL_KOBJ_ATTR_RO(throttle_reason_vr_tdc);

static const struct attribute *freq_attrs[] = {
	&dev_attr_punit_req_freq_mhz.attr,
	&dev_attr_throttle_reason_status.attr,
	&dev_attr_throttle_reason_pl1.attr,
	&dev_attr_throttle_reason_pl2.attr,
	&dev_attr_throttle_reason_pl4.attr,
	&dev_attr_throttle_reason_thermal.attr,
	&dev_attr_throttle_reason_prochot.attr,
	&dev_attr_throttle_reason_ratl.attr,
	&dev_attr_throttle_reason_vr_thermalert.attr,
	&dev_attr_throttle_reason_vr_tdc.attr,
	NULL
};

/*
 * Mem Frequency query interface -
 * sysfs files under directory <dev>/gt/gt<i>/
 */

static ssize_t mem_RP0_freq_mhz_show(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 val;
	int err;

	err = __intel_gt_pcode_read(gt, XEHPSDV_PCODE_FREQUENCY_CONFIG,
				    PCODE_MBOX_FC_SC_READ_FUSED_P0,
				    PCODE_MBOX_DOMAIN_HBM, &val);
	if (err)
		return err;

	/* data_out - Fused P0 for domain ID in units of 50 MHz */
	val *= GT_FREQUENCY_MULTIPLIER;

	return sysfs_emit(buff, "%u\n", val);
}

static ssize_t mem_RPn_freq_mhz_show(struct kobject *kobj,
				       struct kobj_attribute *attr,
				      char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 val;
	int err;

	err = __intel_gt_pcode_read(gt, XEHPSDV_PCODE_FREQUENCY_CONFIG,
				    PCODE_MBOX_FC_SC_READ_FUSED_PN,
				    PCODE_MBOX_DOMAIN_HBM, &val);
	if (err)
		return err;

	/* data_out - Fused P0 for domain ID in units of 50 MHz */
	val *= GT_FREQUENCY_MULTIPLIER;

	return sysfs_emit(buff, "%u\n", val);
}

static INTEL_KOBJ_ATTR_RO(mem_RP0_freq_mhz);
static INTEL_KOBJ_ATTR_RO(mem_RPn_freq_mhz);

static const struct attribute *mem_freq_attrs[] = {
	&dev_attr_mem_RP0_freq_mhz.attr,
	&dev_attr_mem_RPn_freq_mhz.attr,
	NULL
};

/*
 * PVC Performance control/query interface -
 * sysfs files under directory <dev>/gt/gt<i>/
 */

static ssize_t freq_factor_scale_show(struct kobject *kobj,
				      struct kobj_attribute *attr,
				       char *buff)
{
	return sysfs_emit(buff, "%s\n", U8_8_SCALE_TO_VALUE);
}

static ssize_t base_freq_factor_show(struct kobject *kobj,
                                    struct kobj_attribute *attr,
				     char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 val;
	int err;

	err = __intel_gt_pcode_read(gt, PVC_PCODE_QOS_MULTIPLIER_GET,
				    PCODE_MBOX_DOMAIN_CHIPLET,
				    PCODE_MBOX_DOMAIN_BASE, &val);
	if (err)
		return err;

	val &= U8_8_VAL_MASK;

	return sysfs_emit(buff, "%u\n", val);
}

static ssize_t base_freq_factor_store(struct kobject *kobj,
                                     struct kobj_attribute *attr,
				      const char *buff, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 val;
	int err;

	err = kstrtou32(buff, 0, &val);
	if (err)
		return err;

	if (val > U8_8_VAL_MASK)
		return -EINVAL;

	err = __intel_gt_pcode_write(gt, PVC_PCODE_QOS_MULTIPLIER_SET,
				     PCODE_MBOX_DOMAIN_CHIPLET,
				     PCODE_MBOX_DOMAIN_BASE, val);
	if (err)
		return err;

	return count;
}

static ssize_t base_RP0_freq_mhz_show(struct kobject *kobj,
                                     struct kobj_attribute *attr,
				      char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 val;
	int err;

	err = __intel_gt_pcode_read(gt, XEHPSDV_PCODE_FREQUENCY_CONFIG,
				    PCODE_MBOX_FC_SC_READ_FUSED_P0,
				    PCODE_MBOX_DOMAIN_BASE, &val);
	if (err)
		return err;

	/* data_out - Fused P0 for domain ID in units of 50 MHz */
	val *= GT_FREQUENCY_MULTIPLIER;

	return sysfs_emit(buff, "%u\n", val);
}

static ssize_t base_RPn_freq_mhz_show(struct kobject *kobj,
                                    struct kobj_attribute *attr,
				     char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 val;
	int err;

	err = __intel_gt_pcode_read(gt, XEHPSDV_PCODE_FREQUENCY_CONFIG,
				    PCODE_MBOX_FC_SC_READ_FUSED_PN,
				    PCODE_MBOX_DOMAIN_BASE, &val);
	if (err)
		return err;

	/* data_out - Fused Pn for domain ID in units of 50 MHz */
	val *= GT_FREQUENCY_MULTIPLIER;

	return sysfs_emit(buff, "%u\n", val);
}

static ssize_t base_act_freq_mhz_show(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
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

static ssize_t media_freq_factor_show(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
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
		mode = _with_pm_intel_dev_read(kobj, attr, GEN6_RPNSWREQ);
		mode = REG_FIELD_GET(GEN12_MEDIA_FREQ_RATIO, mode) ?
			SLPC_MEDIA_RATIO_MODE_FIXED_ONE_TO_ONE :
			SLPC_MEDIA_RATIO_MODE_FIXED_ONE_TO_TWO;
	}

	return sysfs_emit(buff, "%u\n", media_ratio_mode_to_factor(mode));
}

static ssize_t media_freq_factor_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buff, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
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

static ssize_t media_RP0_freq_mhz_show(struct kobject *kobj,
                                      struct kobj_attribute *attr,
				       char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 val;
	int err;

	err = __intel_gt_pcode_read(gt, XEHPSDV_PCODE_FREQUENCY_CONFIG,
				    PCODE_MBOX_FC_SC_READ_FUSED_P0,
				    PCODE_MBOX_DOMAIN_MEDIAFF, &val);

	if (err)
		return err;

	/* data_out - Fused P0 for domain ID in units of 50 MHz */
	val *= GT_FREQUENCY_MULTIPLIER;

	return sysfs_emit(buff, "%u\n", val);
}

static ssize_t media_RPn_freq_mhz_show(struct kobject *kobj,
                                      struct kobj_attribute *attr,
				       char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	u32 val;
	int err;

	err = __intel_gt_pcode_read(gt, XEHPSDV_PCODE_FREQUENCY_CONFIG,
				    PCODE_MBOX_FC_SC_READ_FUSED_PN,
				    PCODE_MBOX_DOMAIN_MEDIAFF, &val);

	if (err)
		return err;

	/* data_out - Fused P0 for domain ID in units of 50 MHz */
	val *= GT_FREQUENCY_MULTIPLIER;

	return sysfs_emit(buff, "%u\n", val);
}

static ssize_t media_act_freq_mhz_show(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       char *buff)
{
	struct device *dev = kobj_to_dev(kobj);
	struct intel_gt *gt = intel_gt_sysfs_get_drvdata(dev, attr->attr.name);
	struct intel_rps *rps = &gt->rps;
	i915_reg_t rgadr = PVC_MEDIA_PERF_STATUS;
	u32 val = _with_pm_intel_dev_read(kobj, attr, rgadr);

	/* Available from PVC B-step */
	val = REG_FIELD_GET(PVC_MEDIA_PERF_MEDIA_RATIO, val);
	val = intel_gpu_freq(rps, val);

	return sysfs_emit(buff, "%u\n", val);
}

static INTEL_KOBJ_ATTR_RW(base_freq_factor);
static struct kobj_attribute dev_attr_base_freq_factor_scale =
        __ATTR(base_freq_factor.scale, 0444, freq_factor_scale_show, NULL);
static INTEL_KOBJ_ATTR_RO(base_RP0_freq_mhz);
static INTEL_KOBJ_ATTR_RO(base_RPn_freq_mhz);
static INTEL_KOBJ_ATTR_RO(base_act_freq_mhz);

static INTEL_KOBJ_ATTR_RW(media_freq_factor);
static struct kobj_attribute dev_attr_media_freq_factor_scale =
	__ATTR(media_freq_factor.scale, 0444, freq_factor_scale_show, NULL);

static INTEL_KOBJ_ATTR_RO(media_RP0_freq_mhz);
static INTEL_KOBJ_ATTR_RO(media_RPn_freq_mhz);
static INTEL_KOBJ_ATTR_RO(media_act_freq_mhz);

static const struct attribute *pvc_perf_power_attrs[] = {
	&dev_attr_base_freq_factor.attr,
	&dev_attr_base_freq_factor_scale.attr,
	&dev_attr_base_RP0_freq_mhz.attr,
	&dev_attr_base_RPn_freq_mhz.attr,
	&dev_attr_base_act_freq_mhz.attr,
	NULL
};

static const struct attribute *media_perf_power_attrs[] = {
	&dev_attr_media_freq_factor.attr,
	&dev_attr_media_freq_factor_scale.attr,
	&dev_attr_media_RP0_freq_mhz.attr,
	&dev_attr_media_RPn_freq_mhz.attr,
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
	struct kobject *kobj = &dev->kobj;
	i915_reg_t rgadr = PVC_GT0_PACKAGE_SYS_PWR_BAL_FACTOR;
	u32 val = _with_pm_intel_dev_read(kobj, (struct kobj_attribute *)attr, rgadr);

	val = REG_FIELD_GET(PVC_SYS_PWR_BAL_FACTOR_MASK, val);

	return scnprintf(buf, PAGE_SIZE, "%d\n", val );
}
static DEVICE_ATTR_RW(sys_pwr_balance);

/* sysfs file <dev>/sys_pwr_balance */
static const struct attribute * const sys_pwr_balance_attrs[] = {
	&dev_attr_sys_pwr_balance.attr,
	NULL
};

static ssize_t
default_min_freq_mhz_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_gt *gt = kobj_to_gt(kobj->parent);

	return sysfs_emit(buf, "%d\n", gt->rps_defaults.min_freq);
}

static struct kobj_attribute default_min_freq_mhz =
__ATTR(rps_min_freq_mhz, 0444, default_min_freq_mhz_show, NULL);

static ssize_t
default_max_freq_mhz_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_gt *gt = kobj_to_gt(kobj->parent);

	return sysfs_emit(buf, "%d\n", gt->rps_defaults.max_freq);
}

static struct kobj_attribute default_max_freq_mhz =
__ATTR(rps_max_freq_mhz, 0444, default_max_freq_mhz_show, NULL);

static ssize_t
default_boost_freq_mhz_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_gt *gt = kobj_to_gt(kobj->parent);

	return sysfs_emit(buf, "%d\n", gt->rps_defaults.boost_freq);
}

static struct kobj_attribute default_boost_freq_mhz =
__ATTR(rps_boost_freq_mhz, 0444, default_boost_freq_mhz_show, NULL);

static ssize_t
default_media_freq_factor_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_gt *gt = kobj_to_gt(kobj->parent);

	return sysfs_emit(buf, "%d\n",
			  media_ratio_mode_to_factor(gt->rps_defaults.media_ratio_mode));
}

static struct kobj_attribute default_media_freq_factor =
__ATTR(media_freq_factor, 0444, default_media_freq_factor_show, NULL);

static ssize_t
default_base_freq_factor_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_gt *gt = kobj_to_gt(kobj->parent);

	return sysfs_emit(buf, "%d\n", gt->rps_defaults.base_freq_factor);
}

static struct kobj_attribute default_base_freq_factor =
__ATTR(base_freq_factor, 0444, default_base_freq_factor_show, NULL);

static const struct attribute * const rps_defaults_attrs[] = {
	&default_min_freq_mhz.attr,
	&default_max_freq_mhz.attr,
	&default_boost_freq_mhz.attr,
	NULL
};

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
		ret = sysfs_create_files(kobj, freq_attrs);
		if (ret)
			return ret;
	}

	if (IS_PONTEVECCHIO(gt->i915)) {
		ret = sysfs_create_files(kobj, pvc_perf_power_attrs);
		if (ret)
			return ret;

		set_default_base_freq_factor(gt);
		ret = sysfs_create_file(gt->sysfs_defaults, &default_base_freq_factor.attr);
		if (ret)
			return ret;
	}

	if (IS_PVC_BD_REVID(gt->i915, PVC_BD_REVID_B0, STEP_FOREVER)) {
		ret = sysfs_create_file(kobj, &dev_attr_media_act_freq_mhz.attr);
		if (ret)
			return ret;
	}

	if (IS_DGFX(gt->i915)) {
		ret = sysfs_create_file(kobj, &dev_attr_rapl_PL1_freq_mhz.attr);
		if (ret)
			return ret;

		ret = sysfs_create_files(kobj, mem_freq_attrs);
		if (ret)
			return ret;
	}

	if (HAS_MEDIA_RATIO_MODE(gt->i915) && intel_uc_uses_guc_slpc(&gt->uc)) {
		ret = sysfs_create_files(kobj, media_perf_power_attrs);
		if (ret)
			return ret;

		ret = sysfs_create_file(gt->sysfs_defaults, &default_media_freq_factor.attr);
		if (ret)
			return ret;
	}

	return add_rps_defaults(gt);
}

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
	ret = __snb_pcode_write(gt->i915, PCODE_MBOX_CD, PCODE_MBOX_CD_TRIGGER_SHUTDOWN,
				0, pcode_cmd);
	if (ret)
		return ret;

	ret = __snb_pcode_read(gt->i915, PCODE_MBOX_CD, PCODE_MBOX_CD_STATUS, 0,
			       &iaf_status);

	/*
	 * Power on can be on the order of 10s of seconds.  Try to be
	 * optimistic with 5.
	 */
	while (!ret && iaf_status != status && retry < 10) {
		ssleep(POWER_STATE_PW_DELAY_MIN);
		ret = __snb_pcode_read(gt->i915, PCODE_MBOX_CD, PCODE_MBOX_CD_STATUS, 0,
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
	GEM_BUG_ON(IS_PVC_BD_REVID(gt->i915, PVC_BD_REVID_A0, PVC_BD_REVID_B0));

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

static DEVICE_ATTR_RW(iaf_power_enable);

static const struct attribute *iaf_attrs[] = {
	&dev_attr_iaf_power_enable.attr,
	NULL
};

static int intel_sysfs_rps_init(struct intel_gt *gt, struct kobject *kobj)
{
	const struct attribute * const *attrs;
	int ret;

	if (is_object_gt(kobj))
		attrs = gen6_rps_attrs;
	else
		attrs = gen6_gt_attrs;
	ret = sysfs_create_files(kobj, attrs);
	if (ret)
		return ret;

	if (IS_VALLEYVIEW(gt->i915) || IS_CHERRYVIEW(gt->i915)) {
		ret = sysfs_create_file(kobj, &dev_attr_vlv_rpe_freq_mhz.attr);
		if (ret)
			return ret;
	}

	if (is_object_gt(kobj)) {
		/* attributes for only directory gt/gt<i> */
		ret = intel_sysfs_rps_init_gt(gt, kobj);
		if (ret)
			return ret;
	} else if (IS_PONTEVECCHIO(gt->i915)) {
		ret = sysfs_create_files(kobj, sys_pwr_balance_attrs);
		if (ret)
			return ret;

		if (IS_PVC_BD_REVID(gt->i915, PVC_BD_REVID_B0, STEP_FOREVER) &&
		    HAS_IAF(gt->i915)) {
			ret = sysfs_create_files(kobj, iaf_attrs);
			if (ret)
				return ret;
		}
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
