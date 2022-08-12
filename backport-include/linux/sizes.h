/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * include/linux/sizes.h
 */
#ifndef _BACKPORT_LINUX_SIZES_H__
#define _BACKPORT_LINUX_SIZES_H__

#include_next<linux/sizes.h>

#define SZ_8G                           _AC(0x200000000, ULL)
#define SZ_16G                          _AC(0x400000000, ULL)
#define SZ_32G                          _AC(0x800000000, ULL)

#endif  /* _BACKPORT_LINUX_SIZES_H__ */
