#ifndef _BACKPORT_UTSRELEASE_
#define _BACKPORT_UTSRELEASE_

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
#define UTS_RELEASE "2.6.32"
#else
#include_next <generated/utsrelease.h>
#endif

#endif
