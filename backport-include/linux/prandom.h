/* SPDX-License-Identifier: GPL-2.0 */
/*
 * include/linux/prandom.h
 *
 * Include file for the fast pseudo-random 32-bit
 * generation.
 */
#ifndef _BACKPORT_PRANDOM_H
#define _BACKPORT_PRANDOM_H

#ifdef BPM_PRANDOM_H_NOT_PRESENT
#include <linux/random.h>
#else
#include_next <linux/prandom.h>
#endif

#ifdef BPM_PRANDOM_U32_NOT_PRESENT
#define prandom_u32 get_random_u32
#endif

#ifdef BPM_NEXT_PSEUDO_RANDOM32_NOT_PRESENT
/* Pseudo random number generator from numerical recipes. */
static inline u32 next_pseudo_random32(u32 seed)
{
	return seed * 1664525 + 1013904223;
}
#endif

#endif
