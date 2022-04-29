// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

#include <linux/kobject.h>
#include <linux/sysfs.h>

#include "gt/intel_gt_sysfs.h"

#include "i915_drv.h"
#include "i915_sysfs.h"
#include "intel_sysfs_mem_health.h"

static const char *
memory_error_to_str(const struct intel_mem_sparing_event *mem)
{
	switch (mem->health_status) {
	case MEM_HEALTH_ALARM:
		return "MEMORY_HEALTH_ALARM";
	case MEM_HEALTH_EC_PENDING:
		return "EC_PENDING";
	case MEM_HEALTH_DEGRADED:
		return "DEGRADED";
	case MEM_HEALTH_UNKNOWN:
		return "MEMORY_HEALTH_UNKNOWN";
	case MEM_HEALTH_OKAY:
	default:
		return "OK";
	}
}

static ssize_t
device_memory_health_show(struct device *kdev, struct device_attribute *attr,
			  char *buf)
{
	struct drm_i915_private *i915 = kdev_minor_to_i915(kdev);
	const char *mem_status;

	mem_status = memory_error_to_str(&to_gt(i915)->mem_sparing);
	return sysfs_emit(buf, "%s\n", mem_status);
}

static const DEVICE_ATTR_RO(device_memory_health);

static const struct attribute *mem_health_attrs[] = {
	&dev_attr_device_memory_health.attr,
	NULL
};

static ssize_t
addr_range_show(struct device *kdev, struct device_attribute *attr, char *buf)
{
	struct intel_gt *gt = kobj_to_gt(&kdev->kobj);

	return sysfs_emit(buf, "%pa\n", &gt->lmem->actual_physical_mem);
}

static DEVICE_ATTR(addr_range, 0444, addr_range_show, NULL);

static const struct attribute *addr_range_attrs[] = {
	/* TODO: Report any other HBM Sparing sysfs per gt? */
	&dev_attr_addr_range.attr,
	NULL
};

void intel_gt_sysfs_register_mem(struct intel_gt *gt, struct kobject *parent)
{
	if (!HAS_MEM_SPARING_SUPPORT(gt->i915))
		return;

	if (sysfs_create_files(parent, addr_range_attrs))
		drm_err(&gt->i915->drm, "Setting up sysfs to read total physical memory per tile failed\n");
}

void intel_mem_health_report_sysfs(struct drm_i915_private *i915)
{
	struct device *kdev = i915->drm.primary->kdev;

	if (!HAS_MEM_SPARING_SUPPORT(i915))
		return;

	if (sysfs_create_files(&kdev->kobj, mem_health_attrs)) {
		dev_err(kdev, "Failed to add sysfs files to show memory health status\n");
		return;
	}
}
