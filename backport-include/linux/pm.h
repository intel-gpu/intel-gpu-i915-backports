#ifndef __BACKPORT_PM_H
#define __BACKPORT_PM_H
#include_next <linux/pm.h>

#ifndef PM_EVENT_AUTO
#define PM_EVENT_AUTO		0x0400
#endif

#ifndef PM_EVENT_SLEEP
#define PM_EVENT_SLEEP  (PM_EVENT_SUSPEND)
#endif

#ifndef PMSG_IS_AUTO
#define PMSG_IS_AUTO(msg)	(((msg).event & PM_EVENT_AUTO) != 0)
#endif

#ifdef BPM_DPM_FLAG_NEVER_SKIP_RENAMED
#define DPM_FLAG_NO_DIRECT_COMPLETE DPM_FLAG_NEVER_SKIP
#endif

#endif /* __BACKPORT_PM_H */
