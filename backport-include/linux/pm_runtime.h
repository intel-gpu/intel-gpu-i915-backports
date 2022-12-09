/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * pm_runtime.h - Device run-time power management helper functions.
 *
 * Copyright (C) 2009 Rafael J. Wysocki <rjw@sisk.pl>
 */

#ifndef __BACKPORT_PM_RUNTIME_H
#define __BACKPORT_PM_RUNTIME_H
#include <linux/version.h>
#include_next <linux/pm_runtime.h>


#ifdef BPM_PM_RUNTIME_GET_IF_ACTIVE_NOT_PRESENT
int pm_runtime_get_if_active(struct device *dev, bool ign_usage_count);
#endif

#ifdef BPM_PM_RUNTIME_RESUME_AND_GET_NOT_PRESENT
/**
 * pm_runtime_resume_and_get - Bump up usage counter of a device and resume it.
 * @dev: Target device.
 *
 * Resume @dev synchronously and if that is successful, increment its runtime
 * PM usage counter. Return 0 if the runtime PM usage counter of @dev has been
 * incremented or a negative error code otherwise.
 */
static inline int pm_runtime_resume_and_get(struct device *dev)
{
        int ret;

        ret = __pm_runtime_resume(dev, RPM_GET_PUT);
        if (ret < 0) {
                pm_runtime_put_noidle(dev);
                return ret;
        }

        return 0;
}
#endif

#endif /* __BACKPORT_PM_RUNTIME_H */
