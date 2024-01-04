#ifndef __BACKPORT_LINUX_MATH64_H
#define __BACKPORT_LINUX_MATH64_H
#include_next <linux/math64.h>

#if LINUX_VERSION_IS_LESS(3,12,0)

#if BITS_PER_LONG == 64
/**
 * div64_u64_rem - unsigned 64bit divide with 64bit divisor and remainder
 */
#define div64_u64_rem LINUX_I915_BACKPORT(div64_u64_rem)
static inline u64 div64_u64_rem(u64 dividend, u64 divisor, u64 *remainder)
{
	*remainder = dividend % divisor;
	return dividend / divisor;
}
#elif BITS_PER_LONG == 32
#ifndef div64_u64_rem
#define div64_u64_rem LINUX_I915_BACKPORT(div64_u64_rem)
#define backports_div64_u64_rem_add 1
extern u64 div64_u64_rem(u64 dividend, u64 divisor, u64 *remainder);
#endif

#endif /* BITS_PER_LONG */
#endif /* < 3.12 */

/**
 * div64_u64_rem - unsigned 64bit divide with 64bit divisor and remainder
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

#endif /* __BACKPORT_LINUX_MATH64_H */
