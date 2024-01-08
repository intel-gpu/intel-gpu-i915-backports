#ifndef __BACKPORT_LNIUX_JIFFIES_H
#define __BACKPORT_LNIUX_JIFFIES_H
#include_next <linux/jiffies.h>

#ifndef time_is_before_jiffies
#define time_is_before_jiffies(a) time_after(jiffies, a)
#endif

#ifndef time_is_after_jiffies
#define time_is_after_jiffies(a) time_before(jiffies, a)
#endif

#ifndef time_is_before_eq_jiffies
#define time_is_before_eq_jiffies(a) time_after_eq(jiffies, a)
#endif

#ifndef time_is_after_eq_jiffies
#define time_is_after_eq_jiffies(a) time_before_eq(jiffies, a)
#endif

/*
 * This function is available, but not exported in kernel < 3.17, add
 * an own version.
 */
#if LINUX_VERSION_IS_LESS(3,17,0)
#define nsecs_to_jiffies LINUX_I915_BACKPORT(nsecs_to_jiffies)
extern unsigned long nsecs_to_jiffies(u64 n);
#endif /* 3.17 */

#ifdef BPM_JIFFIES_DELTA_TO_MSECS_NOT_PRESENT

#ifndef time_is_before_eq_jiffies
#define time_is_before_eq_jiffies(a) time_after_eq(jiffies, a)
#endif
static inline unsigned int jiffies_delta_to_msecs(long delta)
{
	return jiffies_to_msecs(max(0L, delta));
}
#endif /* BPM_JIFFIES_DELTA_TO_MSECS_NOT_PRESENT */

#endif /* __BACKPORT_LNIUX_JIFFIES_H */
