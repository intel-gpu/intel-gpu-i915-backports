// SPDX-License-Identifier: GPL-2.0-only
/*
 *  kernel/sched/core.c
 *
 *  Core kernel scheduler code and related syscalls
 *
 *  Copyright (C) 1991-2002  Linus Torvalds
 */
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>

#ifdef BPM_SCHED_SET_API_NOT_PRESENT
/*
 * For when you don't much care about FIFO, but want to be above SCHED_NORMAL.
 */
void sched_set_fifo_low(struct task_struct *p)
{
        struct sched_param sp = { .sched_priority = 1 };
        WARN_ON_ONCE(sched_setscheduler_nocheck(p, SCHED_FIFO, &sp) != 0);
}
EXPORT_SYMBOL_GPL(sched_set_fifo_low);

void sched_set_normal(struct task_struct *p, int nice)
{
        struct sched_attr attr = {
                .sched_policy = SCHED_NORMAL,
                .sched_nice = nice,
        };
        WARN_ON_ONCE(sched_setattr(p, &attr) != 0);
}
EXPORT_SYMBOL_GPL(sched_set_normal);
#endif
