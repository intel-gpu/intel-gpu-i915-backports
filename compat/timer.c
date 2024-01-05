// SPDX-License-Identifier: GPL-2.0
/*
 *  Kernel internal timers
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  1997-01-28  Modified by Finn Arne Gangstad to make timers scale better.
 *
 *  1997-09-10  Updated NTP code according to technical memorandum Jan '96
 *              "A Kernel Model for Precision Timekeeping" by Dave Mills
 *  1998-12-24  Fixed a xtime SMP race (we need the xtime_lock rw spinlock to
 *              serialize accesses to xtime/lost_ticks).
 *                              Copyright (C) 1998  Andrea Arcangeli
 *  1999-03-10  Improved NTP compatibility by Ulrich Windl
 *  2002-05-31  Move sys_sysinfo here and make its locking sane, Robert Love
 *  2000-10-05  Implemented scalable SMP per-CPU timer handling.
 *                              Copyright (C) 2000, 2001, 2002  Ingo Molnar
 *              Designed by David S. Miller, Alexey Kuznetsov and Ingo Molnar
 */
#include <linux/time.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/sched/debug.h>

#ifdef BPM_USLEEP_RANGE_STATE_NOT_PRESENT
/**
 * usleep_range_state - Sleep for an approximate time in a given state
 * @min:        Minimum time in usecs to sleep
 * @max:        Maximum time in usecs to sleep
 * @state:      State of the current task that will be while sleeping
 *
 * In non-atomic context where the exact wakeup time is flexible, use
 * usleep_range_state() instead of udelay().  The sleep improves responsiveness
 * by avoiding the CPU-hogging busy-wait of udelay(), and the range reduces
 * power usage by allowing hrtimers to take advantage of an already-
 * scheduled interrupt instead of scheduling a new one just for this sleep.
 */
void __sched usleep_range_state(unsigned long min, unsigned long max,
                                unsigned int state)
{
        ktime_t exp = ktime_add_us(ktime_get(), min);
        u64 delta = (u64)(max - min) * NSEC_PER_USEC;

        for (;;) {
                __set_current_state(state);
                /* Do not return before the requested sleep time has elapsed */
                if (!schedule_hrtimeout_range(&exp, delta, HRTIMER_MODE_ABS))
                        break;
        }
}
EXPORT_SYMBOL(usleep_range_state);
#endif
