#ifndef __BACKPORT_PM_RUNTIME_H
#define __BACKPORT_PM_RUNTIME_H
#include_next <linux/pm_runtime.h>

#if LINUX_VERSION_IS_LESS(3,9,0)
#define pm_runtime_active LINUX_I915_BACKPORT(pm_runtime_active)
#ifdef CONFIG_PM
static inline bool pm_runtime_active(struct device *dev)
{
	return dev->power.runtime_status == RPM_ACTIVE
		|| dev->power.disable_depth;
}
#else
static inline bool pm_runtime_active(struct device *dev) { return true; }
#endif /* CONFIG_PM */

#endif /* LINUX_VERSION_IS_LESS(3,9,0) */

#if LINUX_VERSION_IS_LESS(3,15,0)
static inline int pm_runtime_force_suspend(struct device *dev)
{
#ifdef CONFIG_PM
	/* cannot backport properly, I think */
	WARN_ON_ONCE(1);
	return -EINVAL;
#endif
	return 0;
}
static inline int pm_runtime_force_resume(struct device *dev)
{
#ifdef CONFIG_PM
	/* cannot backport properly, I think */
	WARN_ON_ONCE(1);
	return -EINVAL;
#endif
	return 0;
}
#endif /* LINUX_VERSION_IS_LESS(3,15,0) */

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
#endif /* BPM_PM_RUNTIME_RESUME_AND_GET_NOT_PRESENT */

#endif /* __BACKPORT_PM_RUNTIME_H */
