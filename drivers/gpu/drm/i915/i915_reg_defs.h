/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef __I915_REG_DEFS__
#define __I915_REG_DEFS__

#include <linux/bitfield.h>
#include <linux/bits.h>

/**
 * REG_BIT() - Prepare a u32 bit value
 * @__n: 0-based bit number
 *
 * Local wrapper for BIT() to force u32, with compile time checks.
 *
 * @return: Value with bit @__n set.
 */
#define REG_BIT(__n)							\
	((u32)(BIT(__n) +						\
	       BUILD_BUG_ON_ZERO(__is_constexpr(__n) &&		\
				 ((__n) < 0 || (__n) > 31))))

/**
 * REG_BIT8() - Prepare a u8 bit value
 * @__n: 0-based bit number
 *
 * Local wrapper for BIT() to force u8, with compile time checks.
 *
 * @return: Value with bit @__n set.
 */
#define REG_BIT8(__n)                                                   \
	((u8)(BIT(__n) +                                                \
	       BUILD_BUG_ON_ZERO(__is_constexpr(__n) &&         \
				 ((__n) < 0 || (__n) > 7))))

/**
 * REG_GENMASK() - Prepare a continuous u32 bitmask
 * @__high: 0-based high bit
 * @__low: 0-based low bit
 *
 * Local wrapper for GENMASK() to force u32, with compile time checks.
 *
 * @return: Continuous bitmask from @__high to @__low, inclusive.
 */
#define REG_GENMASK(__high, __low)					\
	((u32)(GENMASK(__high, __low) +					\
	       BUILD_BUG_ON_ZERO(__is_constexpr(__high) &&	\
				 __is_constexpr(__low) &&		\
				 ((__low) < 0 || (__high) > 31 || (__low) > (__high)))))

/**
 * REG_GENMASK64() - Prepare a continuous u64 bitmask
 * @__high: 0-based high bit
 * @__low: 0-based low bit
 *
 * Local wrapper for GENMASK_ULL() to force u64, with compile time checks.
 *
 * @return: Continuous bitmask from @__high to @__low, inclusive.
 */
#define REG_GENMASK64(__high, __low)					\
	((u64)(GENMASK_ULL(__high, __low) +				\
	       BUILD_BUG_ON_ZERO(__is_constexpr(__high) &&		\
				 __is_constexpr(__low) &&		\
				 ((__low) < 0 || (__high) > 63 || (__low) > (__high)))))

/**
 * REG_GENMASK8() - Prepare a continuous u8 bitmask
 * @__high: 0-based high bit
 * @__low: 0-based low bit
 *
 * Local wrapper for GENMASK() to force u8, with compile time checks.
 *
 * @return: Continuous bitmask from @__high to @__low, inclusive.
 */
#define REG_GENMASK8(__high, __low)                                     \
	((u8)(GENMASK(__high, __low) +                                  \
	       BUILD_BUG_ON_ZERO(__is_constexpr(__high) &&      \
				 __is_constexpr(__low) &&               \
				 ((__low) < 0 || (__high) > 7 || (__low) > (__high)))))

/*
 * Local integer constant expression version of is_power_of_2().
 */
#define IS_POWER_OF_2(__x)		((__x) && (((__x) & ((__x) - 1)) == 0))

/**
 * REG_FIELD_PREP() - Prepare a u32 bitfield value
 * @__mask: shifted mask defining the field's length and position
 * @__val: value to put in the field
 *
 * Local copy of FIELD_PREP() to generate an integer constant expression, force
 * u32 and for consistency with REG_FIELD_GET(), REG_BIT() and REG_GENMASK().
 *
 * @return: @__val masked and shifted into the field defined by @__mask.
 */
#define REG_FIELD_PREP(__mask, __val)						\
	((u32)((((typeof(__mask))(__val) << __bf_shf(__mask)) & (__mask)) +	\
	       BUILD_BUG_ON_ZERO(!__is_constexpr(__mask)) +		\
	       BUILD_BUG_ON_ZERO((__mask) == 0 || (__mask) > U32_MAX) +		\
	       BUILD_BUG_ON_ZERO(!IS_POWER_OF_2((__mask) + (1ULL << __bf_shf(__mask)))) + \
	       BUILD_BUG_ON_ZERO(__builtin_choose_expr(__is_constexpr(__val), (~((__mask) >> __bf_shf(__mask)) & (__val)), 0))))

/**
 * REG_FIELD_PREP8() - Prepare a u8 bitfield value
 * @__mask: shifted mask defining the field's length and position
 * @__val: value to put in the field
 *
 * Local copy of FIELD_PREP8() to generate an integer constant expression, force
 * u8 and for consistency with REG_FIELD_GET8(), REG_BIT8() and REG_GENMASK8().
 *
 * @return: @__val masked and shifted into the field defined by @__mask.
 */
#define REG_FIELD_PREP8(__mask, __val)                                          \
	((u8)((((typeof(__mask))(__val) << __bf_shf(__mask)) & (__mask)) +      \
	       BUILD_BUG_ON_ZERO(!__is_constexpr(__mask)) +             \
	       BUILD_BUG_ON_ZERO((__mask) == 0 || (__mask) > U8_MAX) +          \
	       BUILD_BUG_ON_ZERO(!IS_POWER_OF_2((__mask) + (1ULL << __bf_shf(__mask)))) + \
	       BUILD_BUG_ON_ZERO(__builtin_choose_expr(__is_constexpr(__val), (~((__mask) >> __bf_shf(__mask)) & (__val)), 0))))

/**
 * REG_FIELD_GET() - Extract a u32 bitfield value
 * @__mask: shifted mask defining the field's length and position
 * @__val: value to extract the bitfield value from
 *
 * Local wrapper for FIELD_GET() to force u32 and for consistency with
 * REG_FIELD_PREP(), REG_BIT() and REG_GENMASK().
 *
 * @return: Masked and shifted value of the field defined by @__mask in @__val.
 */
#define REG_FIELD_GET(__mask, __val)	((u32)FIELD_GET(__mask, __val))

/**
 * REG_FIELD_GET64() - Extract a u64 bitfield value
 * @__mask: shifted mask defining the field's length and position
 * @__val: value to extract the bitfield value from
 *
 * Local wrapper for FIELD_GET() to force u64 and for consistency with
 * REG_GENMASK64().
 *
 * @return: Masked and shifted value of the field defined by @__mask in @__val.
 */
#define REG_FIELD_GET64(__mask, __val)	((u64)FIELD_GET(__mask, __val))

/**
 * REG_FIELD_GET8() - Extract a u8 bitfield value
 * @__mask: shifted mask defining the field's length and position
 * @__val: value to extract the bitfield value from
 *
 * Local wrapper for FIELD_GET() to force u8 and for consistency with
 * REG_FIELD_PREP(), REG_BIT() and REG_GENMASK().
 *
 * @return: Masked and shifted value of the field defined by @__mask in @__val.
 */
#define REG_FIELD_GET8(__mask, __val)   ((u8)FIELD_GET(__mask, __val))

typedef struct {
	u32 reg;
} i915_reg_t;

#define _MMIO(r) ((const i915_reg_t){ .reg = (r) })

typedef struct {
	u32 reg;
} i915_mcr_reg_t;

#define INVALID_MMIO_REG _MMIO(0)

/*
 * These macros can be used on either i915_reg_t or i915_mcr_reg_t since they're
 * simply operations on the register's offset and don't care about the MCR vs
 * non-MCR nature of the register.
 */
#define i915_mmio_reg_offset(r) \
	_Generic((r), i915_reg_t: (r).reg, i915_mcr_reg_t: (r).reg)
#define i915_mmio_reg_equal(a, b) (i915_mmio_reg_offset(a) == i915_mmio_reg_offset(b))
#define i915_mmio_reg_valid(r) (!i915_mmio_reg_equal(r, INVALID_MMIO_REG))

#define VLV_DISPLAY_BASE		0x180000

#define TD_ATT_MAX				3

#endif /* __I915_REG_DEFS__ */
