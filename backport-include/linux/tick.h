/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Tick related global functions
 */
#ifndef _BACKPORT_LINUX_TICK_H
#define _BACKPORT_LINUX_TICK_H

#include_next <linux/tick.h>

#ifdef BPM_TICK_NOHZ_FULL_MASK_NOT_PRESENT
#undef tick_nohz_full_enabled
#undef tick_nohz_full_cpu
#define tick_nohz_full_enabled() false
#define tick_nohz_full_cpu(cpu) false
#endif

#endif /* _BACKPORT_LINUX_TICK_H */
