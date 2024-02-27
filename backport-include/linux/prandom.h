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

#endif
