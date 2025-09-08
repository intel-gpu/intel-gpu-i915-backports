/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BACKPORT_LINUX_PAGE_FLAGS_H
#define _BACKPORT_LINUX_PAGE_FLAGS_H

#include_next <linux/page-flags.h>

#ifdef BPM_PAGE_TRANSTAIL_NOT_PRESENT
#define PageTransTail PageTail
#endif
#endif
