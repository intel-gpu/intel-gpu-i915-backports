#ifndef __BACKPORT_PM_H
#define __BACKPORT_PM_H
#include <linux/version.h>
#include_next <linux/pm.h>

#ifdef BPM_DPM_FLAG_NEVER_SKIP_RENAMED
#define DPM_FLAG_NO_DIRECT_COMPLETE DPM_FLAG_NEVER_SKIP
#endif

#endif /* __BACKPORT_PM_H */
