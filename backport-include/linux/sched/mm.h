#ifndef __BACKPORT_LINUX_SCHED_MM_H
#define __BACKPORT_LINUX_SCHED_MM_H

#include_next <linux/sched/mm.h>

#ifdef FOLIO_ADDRESS_PRESENT
#include_next <linux/mm.h>
#endif

#endif /* __BACKPORT_LINUX_SCHED_MM_H */
