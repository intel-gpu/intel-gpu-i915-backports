/* SPDX-License-Identifier: GPL-2.0 */
/* interrupt.h */
#ifndef _BACKPORT_LINUX_INTERRUPT_H
#define _BACKPORT_LINUX_INTERRUPT_H
#include_next <linux/interrupt.h>

#if defined(CONFIG_SMP) || defined(CONFIG_PREEMPT_RT)
void tasklet_unlock_spin_wait(struct tasklet_struct *t);
#else
static inline void tasklet_unlock_spin_wait(struct tasklet_struct *t) { }
#endif

#endif /* _BACKPORT_LINUX_INTERRUPT_H */
