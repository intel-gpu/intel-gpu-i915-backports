#ifndef __BACKPORT_PM_RUNTIME_H
#define __BACKPORT_PM_RUNTIME_H
#include <linux/version.h>
#include_next <linux/pm_runtime.h>


#if RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(8,5) && LINUX_VERSION_IS_LESS(5,7,0)

int pm_runtime_get_if_active(struct device *dev, bool ign_usage_count);

#endif
#endif /* __BACKPORT_PM_RUNTIME_H */
