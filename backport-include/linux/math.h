/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BACKPORT_LINUX_MATH_H
#define _BACKPORT_LINUX_MATH_H

#ifdef BPM_MATH_H_NOT_PRESENT
#include <linux/kernel.h>
#else
#include_next <linux/math.h>
#endif

#endif /* _LINUX_MATH_H */
                              
