// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef __INTEL_CX0_PHY_H__
#define __INTEL_CX0_PHY_H__

#include <linux/types.h>
#include <linux/bitfield.h>
#include <linux/bits.h>

#include "i915_drv.h"
#include "intel_display_types.h"

/**
 * REG_BIT8() - Prepare a u8 bit value
 * @__n: 0-based bit number
 *
 * Local wrapper for BIT() to force u8, with compile time checks.
 *
 * @return: Value with bit @__n set.
 */
#define REG_BIT8(__n)							\
	((u8)(BIT(__n) +						\
	       BUILD_BUG_ON_ZERO(__is_constexpr(__n) &&		\
				 ((__n) < 0 || (__n) > 7))))

/**
 * REG_GENMASK8() - Prepare a continuous u8 bitmask
 * @__high: 0-based high bit
 * @__low: 0-based low bit
 *
 * Local wrapper for GENMASK() to force u8, with compile time checks.
 *
 * @return: Continuous bitmask from @__high to @__low, inclusive.
 */
#define REG_GENMASK8(__high, __low)					\
	((u8)(GENMASK(__high, __low) +					\
	       BUILD_BUG_ON_ZERO(__is_constexpr(__high) &&	\
				 __is_constexpr(__low) &&		\
				 ((__low) < 0 || (__high) > 7 || (__low) > (__high)))))

/*
 * Local integer constant expression version of is_power_of_2().
 */
#define IS_POWER_OF_2(__x)		((__x) && (((__x) & ((__x) - 1)) == 0))

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
#define REG_FIELD_PREP8(__mask, __val)						\
	((u8)((((typeof(__mask))(__val) << __bf_shf(__mask)) & (__mask)) +	\
	       BUILD_BUG_ON_ZERO(!__is_constexpr(__mask)) +		\
	       BUILD_BUG_ON_ZERO((__mask) == 0 || (__mask) > U8_MAX) +		\
	       BUILD_BUG_ON_ZERO(!IS_POWER_OF_2((__mask) + (1ULL << __bf_shf(__mask)))) + \
	       BUILD_BUG_ON_ZERO(__builtin_choose_expr(__is_constexpr(__val), (~((__mask) >> __bf_shf(__mask)) & (__val)), 0))))

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
#define REG_FIELD_GET8(__mask, __val)	((u8)FIELD_GET(__mask, __val))

struct drm_i915_private;
struct intel_encoder;
struct intel_crtc_state;
enum phy;

enum intel_cx0_lanes {
	INTEL_CX0_LANE0,
	INTEL_CX0_LANE1,
	INTEL_CX0_BOTH_LANES,
};

#define MB_WRITE_COMMITTED		1
#define MB_WRITE_UNCOMMITTED		0

/* C10 Vendor Registers */
#define PHY_C10_VDR_PLL(idx)		(0xC00 + (idx))
#define  C10_PLL0_FRACEN		REG_BIT8(4)
#define  C10_PLL3_MULTIPLIERH_MASK	REG_GENMASK8(3, 0)
#define  C10_PLL15_HDMIDIV_MASK	REG_GENMASK8(5, 3)
#define  C10_PLL15_TXCLKDIV_MASK	REG_GENMASK8(2, 0)
#define PHY_C10_VDR_CMN(idx)		(0xC20 + (idx))
#define  C10_CMN0_DP_VAL		0x21
#define  C10_CMN0_HDMI_VAL		0x1
#define  C10_CMN3_TXVBOOST_MASK		REG_GENMASK8(7, 5)
#define  C10_CMN3_TXVBOOST(val)		REG_FIELD_PREP8(C10_CMN3_TXVBOOST_MASK, val)
#define PHY_C10_VDR_TX(idx)		(0xC30 + (idx))
#define  C10_TX0_VAL			0x10
#define PHY_C10_VDR_CONTROL(idx)	(0xC70 + (idx) - 1)
#define  C10_VDR_CTRL_MSGBUS_ACCESS	REG_BIT8(2)
#define  C10_VDR_CTRL_MASTER_LANE	REG_BIT8(1)
#define  C10_VDR_CTRL_UPDATE_CFG	REG_BIT8(0)
#define PHY_C10_VDR_CUSTOM_WIDTH	0xD02

#define CX0_P0_STATE_ACTIVE		0x0
#define CX0_P2_STATE_READY		0x2
#define C10_P2PG_STATE_DISABLE		0x9
#define C20_P4PG_STATE_DISABLE		0xC
#define CX0_P2_STATE_RESET		0x2

/* PHY_C10_VDR_PLL0 */

#define PLL_C10_MPLL_SSC_EN		REG_BIT8(0)

/* C20 Registers */
#define PHY_C20_WR_ADDRESS_L		0xC02
#define PHY_C20_WR_ADDRESS_H		0xC03
#define PHY_C20_WR_DATA_L		0xC04
#define PHY_C20_WR_DATA_H		0xC05
#define PHY_C20_RD_ADDRESS_L		0xC06
#define PHY_C20_RD_ADDRESS_H		0xC07
#define PHY_C20_RD_DATA_L		0xC08
#define PHY_C20_RD_DATA_H		0xC09
#define PHY_C20_VDR_CUSTOM_SERDES_RATE	0xD00
#define PHY_C20_VDR_HDMI_RATE 0xD01
#define  PHY_C20_CONTEXT_TOGGLE         REG_BIT8(0)
#define PHY_C20_VDR_CUSTOM_WIDTH	0xD02
#define PHY_C20_A_TX_CNTX_CFG(idx)	(0xCF2E - (idx))
#define PHY_C20_B_TX_CNTX_CFG(idx)	(0xCF2A - (idx))
#define PHY_C20_A_CMN_CNTX_CFG(idx)	(0xCDAA - (idx))
#define PHY_C20_B_CMN_CNTX_CFG(idx)	(0xCDA5 - (idx))
#define PHY_C20_A_MPLLA_CNTX_CFG(idx)	(0xCCF0 - (idx))
#define PHY_C20_B_MPLLA_CNTX_CFG(idx)	(0xCCE5 - (idx))
#define PHY_C20_A_MPLLB_CNTX_CFG(idx)	(0xCB5A - (idx))
#define PHY_C20_B_MPLLB_CNTX_CFG(idx)	(0xCB4E - (idx))

#define C20_MPLLB_FRACEN		REG_BIT(13)
#define C20_MPLLA_FRACEN		REG_BIT(14)
#define C20_MULTIPLIER_MASK		REG_GENMASK(11, 0)
#define C20_MPLLB_TX_CLK_DIV_MASK	REG_GENMASK(15, 13)
#define C20_MPLLA_TX_CLK_DIV_MASK	REG_GENMASK(10, 8)

#define RAWLANEAONX_DIG_TX_MPLLB_CAL_DONE_BANK(idx)	(0x303D + (idx))

/* PIPE SPEC Defined Registers */
#define PHY_CX0_TX_CONTROL(tx, control)	(0x400 + ((tx) - 1) * 0x200 + (control))
#define CONTROL2_DISABLE_SINGLE_TX	REG_BIT(6)

/* C10 Phy VSWING Masks */
#define C10_PHY_VSWING_LEVEL_MASK		REG_GENMASK8(2, 0)
#define C10_PHY_VSWING_LEVEL(val)		REG_FIELD_PREP8(C10_PHY_VSWING_LEVEL_MASK, val)
#define C10_PHY_VSWING_PREEMPH_MASK		REG_GENMASK8(1, 0)
#define C10_PHY_VSWING_PREEMPH(val)		REG_FIELD_PREP8(C10_PHY_VSWING_PREEMPH_MASK, val)

#define C20_PHY_VSWING_PREEMPH_MASK		REG_GENMASK8(5, 0)
#define C20_PHY_VSWING_PREEMPH(val)		REG_FIELD_PREP8(C20_PHY_VSWING_PREEMPH_MASK, val)
static inline bool intel_is_c10phy(struct drm_i915_private *dev_priv, enum phy phy)
{
	if (!IS_METEORLAKE(dev_priv))
		return false;
	else
		return (phy < PHY_C);
}

void intel_mtl_pll_enable(struct intel_encoder *encoder,
			  const struct intel_crtc_state *crtc_state);
void intel_mtl_pll_disable(struct intel_encoder *encoder);
void intel_c10mpllb_readout_hw_state(struct intel_encoder *encoder,
				     struct intel_c10mpllb_state *pll_state);
int intel_cx0mpllb_calc_state(struct intel_crtc_state *crtc_state,
			      struct intel_encoder *encoder);
int intel_c20pll_calc_state(struct intel_crtc_state *crtc_state,
			    struct intel_encoder *encoder);
void intel_c20pll_readout_hw_state(struct intel_encoder *encoder,
                                   struct intel_c20pll_state *pll_state);
void intel_c10mpllb_dump_hw_state(struct drm_i915_private *dev_priv,
				  const struct intel_c10mpllb_state *hw_state);
int intel_c10mpllb_calc_port_clock(struct intel_encoder *encoder,
				   const struct intel_c10mpllb_state *pll_state);
void intel_c10mpllb_state_verify(struct intel_atomic_state *state,
				 struct intel_crtc_state *new_crtc_state);
int intel_c20pll_calc_port_clock(struct intel_encoder *encoder,
				 const struct intel_c20pll_state *pll_state);
int intel_cx0_phy_check_hdmi_link_rate(struct intel_hdmi *hdmi, int clock);
void intel_cx0_phy_set_signal_levels(struct intel_encoder *encoder,
				     const struct intel_crtc_state *crtc_state);

int intel_c20_phy_check_hdmi_link_rate(int clock);
void intel_cx0_phy_ddi_vswing_sequence(struct intel_encoder *encoder,
				       const struct intel_crtc_state *crtc_state,
				       u32 level);
int intel_mtl_tbt_calc_port_clock(struct intel_encoder *encoder);
#endif /* __INTEL_CX0_PHY_H__ */
