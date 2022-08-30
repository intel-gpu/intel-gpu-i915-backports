#ifndef _BP_LINUX_BACKPORT_MACRO_H
#define _BP_LINUX_BACKPORT_MACRO_H
#include <linux/version.h>

#define BPC_LOWMEM_FOR_DG1_NOT_SUPPORTED

#if LINUX_VERSION_IS_LESS(5,7,0)

#if (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(8,5))

/* c111566bea7c PM: runtime: Add pm_runtime_get_if_active() */
#define PM_RUNTIME_GET_IF_ACTIVE_NOT_PRESENT

#endif
#endif

#if LINUX_VERSION_IS_LESS(5,9,11)

#if (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(8,6))

/*
 * dd8088d5a896 PM: runtime: Add pm_runtime_resume_and_get
 * to deal with usage counter
 */

#define PM_RUNTIME_RESUME_AND_GET_NOT_PRESENT

#endif
#endif

#endif /* _BP_LINUX_BACKPORT_MACRO_H */
