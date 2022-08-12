/* SPDX-License-Identifier: GPL-2.0 */
/*
 * include/linux/minmax.h
 * This is dummy header for minmax.h
 */


#ifndef _BACKPORT_LINUX_MINMAX_H
#define _BACKPORT_LINUX_MINMAX_H

#if RHEL_RELEASE_VERSION(8, 5) <= RHEL_RELEASE_CODE
#include_next <linux/minmax.h>
#endif

#endif  /* _LINUX_MINMAX_H */
