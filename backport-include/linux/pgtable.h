/* SPDX-License-Identifier: GPL-2.0 */
/*
 * include/linux/pgtable.h
 *
 * In KV(5,8,0), include/asm-generic/pgtable.h is moved
 * to include/linux/pgtable.h. Add a header to include
 * asm/pgtable.h
 *
 * commit detail: ca5999f mm: introduce include/linux/pgtable.h
 *
 */

#ifndef _BACKPORT_LINUX_PGTABLE_H
#define _BACKPORT_LINUX_PGTABLE_H

#if LINUX_VERSION_IS_LESS(5,8,0)
#include <asm/pgtable.h>
#endif

#endif /* _LINUX_PGTABLE_H */
