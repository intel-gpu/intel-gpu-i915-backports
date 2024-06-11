// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

#ifndef __SYSFS_GT_PM_H__
#define __SYSFS_GT_PM_H__

#include <linux/kobject.h>

#include "intel_gt_types.h"

void intel_gt_sysfs_pm_init(struct intel_gt *gt, struct kobject *kobj);
void intel_gt_sysfs_pm_remove(struct intel_gt *gt, struct kobject *kobj);

#ifdef BPM_DEVICE_ATTR_NOT_PRESENT

typedef ssize_t (*show_kobj)(struct kobject *kobj, struct kobj_attribute *attr,
                char *buf);
typedef ssize_t (*store_kobj)(struct kobject *kobj, struct kobj_attribute *attr,
                const char *buf, size_t count);

struct i915_ext_attr_kobj {
        struct kobj_attribute attr;
        show_kobj i915_show_kobj;
        store_kobj i915_store_kobj;
};

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

#endif /* SYSFS_RC6_H */
