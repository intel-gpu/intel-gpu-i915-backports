#ifndef _BACKPORT_LINUX_LIMITS_H
#define _BACKPORT_LINUX_LIMITS_H

#include_next <linux/limits.h>

#ifdef BPM_U32_MIN_NOT_PRESESNT
#define U32_MIN                ((u32)0)
#endif

#endif /* _BACKPORT_LINUX_LIMITS_H */
