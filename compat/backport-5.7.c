/*
 * Copyright (c) 2021
 *
 * Backport functionality introduced in Linux 5.7.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/pm_runtime.h>
#include <linux/pm_wakeirq.h>
#ifdef BPM_PM_RUNTIME_GET_IF_ACTIVE_NOT_PRESENT
/**
 * pm_runtime_get_if_active - Conditionally bump up device usage counter.
 * @dev: Device to handle.
 * @ign_usage_count: Whether or not to look at the current usage counter value.
 *
 * Return -EINVAL if runtime PM is disabled for @dev.
 *
 * Otherwise, if the runtime PM status of @dev is %RPM_ACTIVE and either
 * @ign_usage_count is %true or the runtime PM usage counter of @dev is not
 * zero, increment the usage counter of @dev and return 1. Otherwise, return 0
 * without changing the usage counter.
 *
 * If @ign_usage_count is %true, this function can be used to prevent suspending
 * the device when its runtime PM status is %RPM_ACTIVE.
 *
 * If @ign_usage_count is %false, this function can be used to prevent
 * suspending the device when both its runtime PM status is %RPM_ACTIVE and its
 * runtime PM usage counter is not zero.
 *
 * The caller is resposible for decrementing the runtime PM usage counter of
 * @dev after this function has returned a positive value for it.
 */
int pm_runtime_get_if_active(struct device *dev, bool ign_usage_count)
{
        unsigned long flags;
        int retval;

        spin_lock_irqsave(&dev->power.lock, flags);
        if (dev->power.disable_depth > 0) {
                retval = -EINVAL;
        } else if (dev->power.runtime_status != RPM_ACTIVE) {
                retval = 0;
        } else if (ign_usage_count) {
                retval = 1;
                atomic_inc(&dev->power.usage_count);
        } else {
                retval = atomic_inc_not_zero(&dev->power.usage_count);
        }
        spin_unlock_irqrestore(&dev->power.lock, flags);

        return retval;
}
EXPORT_SYMBOL_GPL(pm_runtime_get_if_active);
#endif
