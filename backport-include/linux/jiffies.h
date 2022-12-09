#ifndef _BACKPORT_JIFFIES_H
#define _BACKPORT_JIFFIES_H

#include <linux/version.h>
#include_next <linux/jiffies.h>

#ifdef BPM_JIFFIES_DELTA_TO_MSECS_NOT_PRESENT

static inline unsigned int jiffies_delta_to_msecs(long delta)
{
        return jiffies_to_msecs(max(0L, delta));
}

#endif
#endif /* __BACKPORT_JIFFIES_H */

