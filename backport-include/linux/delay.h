/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BACKPORT_DELAY_H_
#define _BACKPORT_DELAY_H_

/*
 * Copyright (C) 1993 Linus Torvalds
 *
 * Delay routines, using a pre-computed "loops_per_jiffy" value.
 *
 * Please note that ndelay(), udelay() and mdelay() may return early for
 * several reasons:
 *  1. computed loops_per_jiffy too low (due to the time taken to
 *     execute the timer interrupt.)
 *  2. cache behaviour affecting the time it takes to execute the
 *     loop function.
 *  3. CPU clock rate changes.
 *
 * Please see this thread:
 *   https://lists.openwall.net/linux-kernel/2011/01/09/56
 */
#include_next <linux/delay.h>

#ifdef BPM_USLEEP_RANGE_STATE_NOT_PRESENT
void usleep_range_state(unsigned long min, unsigned long max,
                        unsigned int state);
#endif

#endif /* _BACKPORT_DELAY_H_ */
