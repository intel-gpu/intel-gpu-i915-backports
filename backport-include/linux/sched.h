#ifndef _BACKPORT_LINUX_SCHED_H
#define _BACKPORT_LINUX_SCHED_H

#include_next <linux/sched.h>

#define wake_up_state LINUX_DMABUF_BACKPORT(wake_up_state) 
static inline int wake_up_state(struct task_struct *p, unsigned int state)
{
	return wake_up_process(p);
}

#ifdef BPM_SCHED_SET_API_NOT_PRESENT
extern void sched_set_fifo_low(struct task_struct *p);
extern void sched_set_normal(struct task_struct *p, int nice);
#endif

#endif /* _BACKPORT_LINUX_SCHED_H */
