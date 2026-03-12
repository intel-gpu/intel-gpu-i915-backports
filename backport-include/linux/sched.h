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

#ifdef BPM_TASK_IS_RUNNING_API_IS_NOT_PRESENT
static inline bool task_is_running(struct task_struct *task)
{
	return (READ_ONCE(task->state) == TASK_RUNNING);
}
#endif

#ifdef BPM_TRACE_SET_NEED_RESCHED_NOT_PRESENT
#define set_tsk_need_resched LINUX_I915_BACKPORT(set_tsk_need_resched)

static inline void set_tsk_need_resched(struct task_struct *tsk)
{
	set_tsk_thread_flag(tsk,TIF_NEED_RESCHED);
}
#endif

#endif /* _BACKPORT_LINUX_SCHED_H */
