#ifndef _BACKPORT_LINUX_SCHED_H
#define _BACKPORT_LINUX_SCHED_H

#include_next <linux/sched.h>

#define wake_up_state LINUX_DMABUF_BACKPORT(wake_up_state) 
static inline int wake_up_state(struct task_struct *p, unsigned int state)
{
	return wake_up_process(p);
}

#endif /* _BACKPORT_LINUX_SCHED_H */
