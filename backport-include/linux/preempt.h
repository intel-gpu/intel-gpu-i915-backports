#ifndef _BP_LINUX_PREEMPT_H
#define _BP_LINUX_PREEMPT_H
#include_next <linux/preempt.h>

#ifdef BPM_IN_HARDIRQ_NOT_PRESENT
#define in_hardirq()		(hardirq_count())
#endif /* BPM_IN_HARDIRQ_NOT_PRESENT */

#endif /* _BP_LINUX_PREEMPT_H */
