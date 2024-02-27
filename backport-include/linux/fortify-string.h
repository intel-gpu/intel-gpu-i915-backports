/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BACKPORT_FORTIFY_STRING_H_
#define _BACKPORT_FORTIFY_STRING_H_

#ifdef BPM_FORTIFY_STRING_H_NOT_PRESENT
#include_next <linux/fortify-string.h>
#else
#include_next <linux/string.h>
#endif

#endif /* _BACKPORT_FORTIFY_STRING_H_ */


