#ifndef _BACKPORT_LINUX_WORKQUEUE_H
#define _BACKPORT_LINUX_WORKQUEUE_H
#include <linux/version.h>
#include_next <linux/workqueue.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,18,0)
#if RHEL_RELEASE_CODE != RHEL_RELEASE_VERSION(7,5)
#ifdef CONFIG_DEBUG_OBJECTS_WORK
extern void destroy_delayed_work_on_stack(struct delayed_work *work);
#else
static inline void destroy_delayed_work_on_stack(struct delayed_work *work) { }
#endif
#endif
#endif
#endif
