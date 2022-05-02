/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BACKPORT_LINUX_MATH64_H
#define _BACKPORT_LINUX_MATH64_H

#include_next <linux/math64.h>

/**
 * DIV64_U64_ROUND_CLOSEST - unsigned 64bit divide with 64bit divisor rounded to nearest integer
 * @dividend: unsigned 64bit dividend
 * @divisor: unsigned 64bit divisor
 *
 * Divide unsigned 64bit dividend by unsigned 64bit divisor
 * and round to closest integer.
 *
 * Return: dividend / divisor rounded to nearest integer
 */
#define DIV64_U64_ROUND_CLOSEST(dividend, divisor)      \
        ({ u64 _tmp = (divisor); div64_u64((dividend) + _tmp / 2, _tmp); })

#endif /* _LINUX_MATH64_H */
                              
