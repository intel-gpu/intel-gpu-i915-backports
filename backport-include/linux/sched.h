#ifndef __BACKPORT_LINUX_SCHED_H
#define __BACKPORT_LINUX_SCHED_H
#include_next<linux/sched.h>

#ifdef LINUX_SCHED_CLOCK_H_ADDED
#include <linux/sched/clock.h>
#endif 

#endif /* __BACKPORT_LINUX_SCHED_H */
