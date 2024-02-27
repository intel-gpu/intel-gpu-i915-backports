// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "i915_drv.h"
#include "i915_irq.h"
#include "intel_backlight_regs.h"
#include "intel_combo_phy.h"
#include "intel_combo_phy_regs.h"
#include "intel_de.h"
#include "intel_display_power_well.h"
#include "intel_display_types.h"
#include "intel_dmc.h"
#include "intel_dpll.h"
#include "intel_hotplug.h"
#include "intel_pcode.h"
#include "intel_pm.h"
#include "intel_pps.h"
#include "intel_tc.h"

struct i915_power_well_regs {
	i915_reg_t bios;
	i915_reg_t driver;
	i915_reg_t kvmr;
	i915_reg_t debug;
};

struct i915_power_well_ops {
	const struct i915_power_well_regs *regs;
	/*
	 * Synchronize the well's hw state to match the current sw state, for
	 * example enable/disable it based on the current refcount. Called
	 * during driver init and resume time, possibly after first calling
	 * the enable/disable handlers.
	 */
	void (*sync_hw)(struct drm_i915_private *i915,
			struct i915_power_well *power_well);
	/*
	 * Enable the well and resources that depend on it (for example
	 * interrupts located on the well). Called after the 0->1 refcount
	 * transition.
	 */
	void (*enable)(struct drm_i915_private *i915,
		       struct i915_power_well *power_well);
	/*
	 * Disable the well and resources that depend on it. Called after
	 * the 1->0 refcount transition.
	 */
	void (*disable)(struct drm_i915_private *i915,
			struct i915_power_well *power_well);
	/* Returns the hw enabled state. */
	bool (*is_enabled)(struct drm_i915_private *i915,
			   struct i915_power_well *power_well);
};

static const struct i915_power_well_instance *
i915_power_well_instance(const struct i915_power_well *power_well)
{
	return &power_well->desc->instances->list[power_well->instance_idx];
}

struct i915_power_well *
lookup_power_well(struct drm_i915_private *i915,
		  enum i915_power_well_id power_well_id)
{
	struct i915_power_well *power_well;

	for_each_power_well(i915, power_well)
		if (i915_power_well_instance(power_well)->id == power_well_id)
			return power_well;

	/*
	 * It's not feasible to add error checking code to the callers since
	 * this condition really shouldn't happen and it doesn't even make sense
	 * to abort things like display initialization sequences. Just return
	 * the first power well and hope the WARN gets reported so we can fix
	 * our driver.
	 */
	drm_WARN(&i915->drm, 1,
		 "Power well %d not defined for this platform\n",
		 power_well_id);
	return &i915->power_domains.power_wells[0];
}

void intel_power_well_enable(struct drm_i915_private *i915,
			     struct i915_power_well *power_well)
{
	drm_dbg_kms(&i915->drm, "enabling %s\n", intel_power_well_name(power_well));
	power_well->desc->ops->enable(i915, power_well);
	power_well->hw_enabled = true;
}

void intel_power_well_disable(struct drm_i915_private *i915,
			      struct i915_power_well *power_well)
{
	drm_dbg_kms(&i915->drm, "disabling %s\n", intel_power_well_name(power_well));
	power_well->hw_enabled = false;
	power_well->desc->ops->disable(i915, power_well);
}

void intel_power_well_sync_hw(struct drm_i915_private *i915,
			      struct i915_power_well *power_well)
{
	power_well->desc->ops->sync_hw(i915, power_well);
	power_well->hw_enabled =
		power_well->desc->ops->is_enabled(i915, power_well);
}

void intel_power_well_get(struct drm_i915_private *i915,
			  struct i915_power_well *power_well)
{
	if (!power_well->count++)
		intel_power_well_enable(i915, power_well);
}

void intel_power_well_put(struct drm_i915_private *i915,
			  struct i915_power_well *power_well)
{
	drm_WARN(&i915->drm, !power_well->count,
		 "Use count on power well %s is already zero",
		 i915_power_well_instance(power_well)->name);

	if (!--power_well->count)
		intel_power_well_disable(i915, power_well);
}

bool intel_power_well_is_enabled(struct drm_i915_private *i915,
				 struct i915_power_well *power_well)
{
	return power_well->desc->ops->is_enabled(i915, power_well);
}

bool intel_power_well_is_enabled_cached(struct i915_power_well *power_well)
{
	return power_well->hw_enabled;
}

bool intel_display_power_well_is_enabled(struct drm_i915_private *dev_priv,
					 enum i915_power_well_id power_well_id)
{
	struct i915_power_well *power_well;

	power_well = lookup_power_well(dev_priv, power_well_id);

	return intel_power_well_is_enabled(dev_priv, power_well);
}

bool intel_power_well_is_always_on(struct i915_power_well *power_well)
{
	return power_well->desc->always_on;
}

const char *intel_power_well_name(struct i915_power_well *power_well)
{
	return i915_power_well_instance(power_well)->name;
}

struct intel_power_domain_mask *intel_power_well_domains(struct i915_power_well *power_well)
{
	return &power_well->domains;
}

int intel_power_well_refcount(struct i915_power_well *power_well)
{
	return power_well->count;
}

/*
 * Starting with Haswell, we have a "Power Down Well" that can be turned off
 * when not needed anymore. We have 4 registers that can request the power well
 * to be enabled, and it will only be disabled if none of the registers is
 * requesting it to be enabled.
 */
static void hsw_power_well_post_enable(struct drm_i915_private *dev_priv,
				       u8 irq_pipe_mask, bool has_vga)
{
	if (irq_pipe_mask)
		gen8_irq_power_well_post_enable(dev_priv, irq_pipe_mask);
}

static void hsw_power_well_pre_disable(struct drm_i915_private *dev_priv,
				       u8 irq_pipe_mask)
{
	if (irq_pipe_mask)
		gen8_irq_power_well_pre_disable(dev_priv, irq_pipe_mask);
}

#define ICL_AUX_PW_TO_CH(pw_idx)	\
	((pw_idx) - ICL_PW_CTL_IDX_AUX_A + AUX_CH_A)

#define ICL_TBT_AUX_PW_TO_CH(pw_idx)	\
	((pw_idx) - ICL_PW_CTL_IDX_AUX_TBT1 + AUX_CH_C)

static enum aux_ch icl_aux_pw_to_ch(const struct i915_power_well *power_well)
{
	int pw_idx = i915_power_well_instance(power_well)->hsw.idx;

	return power_well->desc->is_tc_tbt ? ICL_TBT_AUX_PW_TO_CH(pw_idx) :
					     ICL_AUX_PW_TO_CH(pw_idx);
}

static struct intel_digital_port *
aux_ch_to_digital_port(struct drm_i915_private *dev_priv,
		       enum aux_ch aux_ch)
{
	struct intel_digital_port *dig_port = NULL;
	struct intel_encoder *encoder;

	for_each_intel_encoder(&dev_priv->drm, encoder) {
		/* We'll check the MST primary port */
		if (encoder->type == INTEL_OUTPUT_DP_MST)
			continue;

		dig_port = enc_to_dig_port(encoder);
		if (!dig_port)
			continue;

		if (dig_port->aux_ch != aux_ch) {
			dig_port = NULL;
			continue;
		}

		break;
	}

	return dig_port;
}

static enum phy icl_aux_pw_to_phy(struct drm_i915_private *i915,
				  const struct i915_power_well *power_well)
{
	enum aux_ch aux_ch = icl_aux_pw_to_ch(power_well);
	struct intel_digital_port *dig_port = aux_ch_to_digital_port(i915, aux_ch);

	return intel_port_to_phy(i915, dig_port->base.port);
}

static void hsw_wait_for_power_well_enable(struct drm_i915_private *dev_priv,
					   struct i915_power_well *power_well,
					   bool timeout_expected)
{
	const struct i915_power_well_regs *regs = power_well->desc->ops->regs;
	int pw_idx = i915_power_well_instance(power_well)->hsw.idx;

	/*
	 * For some power wells we're not supposed to watch the status bit for
	 * an ack, but rather just wait a fixed amount of time and then
	 * proceed.  This is only used on DG2.
	 */
	if (IS_DG2(dev_priv) && power_well->desc->fixed_enable_delay) {
		usleep_range(600, 1200);
		return;
	}

	/* Timeout for PW1:10 us, AUX:not specified, other PWs:20 us. */
	if (intel_de_wait_for_set(dev_priv, regs->driver,
				  HSW_PWR_WELL_CTL_STATE(pw_idx), 1)) {
		drm_dbg_kms(&dev_priv->drm, "%s power well enable timeout\n",
			    intel_power_well_name(power_well));

		drm_WARN_ON(&dev_priv->drm, !timeout_expected);

	}
}

static u32 hsw_power_well_requesters(struct drm_i915_private *dev_priv,
				     const struct i915_power_well_regs *regs,
				     int pw_idx)
{
	u32 req_mask = HSW_PWR_WELL_CTL_REQ(pw_idx);
	u32 ret;

	ret = intel_de_read(dev_priv, regs->bios) & req_mask ? 1 : 0;
	ret |= intel_de_read(dev_priv, regs->driver) & req_mask ? 2 : 0;
	if (regs->kvmr.reg)
		ret |= intel_de_read(dev_priv, regs->kvmr) & req_mask ? 4 : 0;
	ret |= intel_de_read(dev_priv, regs->debug) & req_mask ? 8 : 0;

	return ret;
}

static void hsw_wait_for_power_well_disable(struct drm_i915_private *dev_priv,
					    struct i915_power_well *power_well)
{
	const struct i915_power_well_regs *regs = power_well->desc->ops->regs;
	int pw_idx = i915_power_well_instance(power_well)->hsw.idx;
	bool disabled;
	u32 reqs;

	/*
	 * Bspec doesn't require waiting for PWs to get disabled, but still do
	 * this for paranoia. The known cases where a PW will be forced on:
	 * - a KVMR request on any power well via the KVMR request register
	 * - a DMC request on PW1 and MISC_IO power wells via the BIOS and
	 *   DEBUG request registers
	 * Skip the wait in case any of the request bits are set and print a
	 * diagnostic message.
	 */
	wait_for((disabled = !(intel_de_read(dev_priv, regs->driver) &
			       HSW_PWR_WELL_CTL_STATE(pw_idx))) ||
		 (reqs = hsw_power_well_requesters(dev_priv, regs, pw_idx)), 1);
	if (disabled)
		return;

	drm_dbg_kms(&dev_priv->drm,
		    "%s forced on (bios:%d driver:%d kvmr:%d debug:%d)\n",
		    intel_power_well_name(power_well),
		    !!(reqs & 1), !!(reqs & 2), !!(reqs & 4), !!(reqs & 8));
}

static void gen9_wait_for_power_well_fuses(struct drm_i915_private *dev_priv,
					   enum skl_power_gate pg)
{
	/* Timeout 5us for PG#0, for other PGs 1us */
	drm_WARN_ON(&dev_priv->drm,
		    intel_de_wait_for_set(dev_priv, SKL_FUSE_STATUS,
					  SKL_FUSE_PG_DIST_STATUS(pg), 1));
}

static void hsw_power_well_enable(struct drm_i915_private *dev_priv,
				  struct i915_power_well *power_well)
{
	const struct i915_power_well_regs *regs = power_well->desc->ops->regs;
	int pw_idx = i915_power_well_instance(power_well)->hsw.idx;
	u32 val;

	if (power_well->desc->has_fuses) {
		enum skl_power_gate pg = ICL_PW_CTL_IDX_TO_PG(pw_idx);

		/* Wa_16013190616:adlp */
		if (IS_ALDERLAKE_P(dev_priv) && pg == SKL_PG1)
			intel_de_rmw(dev_priv, GEN8_CHICKEN_DCPR_1, 0, DISABLE_FLR_SRC);

		/*
		 * For PW1 we have to wait both for the PW0/PG0 fuse state
		 * before enabling the power well and PW1/PG1's own fuse
		 * state after the enabling. For all other power wells with
		 * fuses we only have to wait for that PW/PG's fuse state
		 * after the enabling.
		 */
		if (pg == SKL_PG1)
			gen9_wait_for_power_well_fuses(dev_priv, SKL_PG0);
	}

	val = intel_de_read(dev_priv, regs->driver);
	intel_de_write(dev_priv, regs->driver,
		       val | HSW_PWR_WELL_CTL_REQ(pw_idx));

	hsw_wait_for_power_well_enable(dev_priv, power_well, false);

	if (power_well->desc->has_fuses) {
		enum skl_power_gate pg = ICL_PW_CTL_IDX_TO_PG(pw_idx);

		gen9_wait_for_power_well_fuses(dev_priv, pg);
	}

	hsw_power_well_post_enable(dev_priv,
				   power_well->desc->irq_pipe_mask,
				   power_well->desc->has_vga);
}

static void hsw_power_well_disable(struct drm_i915_private *dev_priv,
				   struct i915_power_well *power_well)
{
	const struct i915_power_well_regs *regs = power_well->desc->ops->regs;
	int pw_idx = i915_power_well_instance(power_well)->hsw.idx;
	u32 val;

	hsw_power_well_pre_disable(dev_priv,
				   power_well->desc->irq_pipe_mask);

	val = intel_de_read(dev_priv, regs->driver);
	intel_de_write(dev_priv, regs->driver,
		       val & ~HSW_PWR_WELL_CTL_REQ(pw_idx));
	hsw_wait_for_power_well_disable(dev_priv, power_well);
}

static void
icl_combo_phy_aux_power_well_enable(struct drm_i915_private *dev_priv,
				    struct i915_power_well *power_well)
{
	const struct i915_power_well_regs *regs = power_well->desc->ops->regs;
	int pw_idx = i915_power_well_instance(power_well)->hsw.idx;
	enum phy phy = icl_aux_pw_to_phy(dev_priv, power_well);
	u32 val;

	drm_WARN_ON(&dev_priv->drm, !IS_ICELAKE(dev_priv));

	val = intel_de_read(dev_priv, regs->driver);
	intel_de_write(dev_priv, regs->driver,
		       val | HSW_PWR_WELL_CTL_REQ(pw_idx));

	hsw_wait_for_power_well_enable(dev_priv, power_well, false);

	/* Display WA #1178: icl */
	if (pw_idx >= ICL_PW_CTL_IDX_AUX_A && pw_idx <= ICL_PW_CTL_IDX_AUX_B &&
	    !intel_bios_is_port_edp(dev_priv, (enum port)phy)) {
		val = intel_de_read(dev_priv, ICL_AUX_ANAOVRD1(pw_idx));
		val |= ICL_AUX_ANAOVRD1_ENABLE | ICL_AUX_ANAOVRD1_LDO_BYPASS;
		intel_de_write(dev_priv, ICL_AUX_ANAOVRD1(pw_idx), val);
	}
}

static void
icl_combo_phy_aux_power_well_disable(struct drm_i915_private *dev_priv,
				     struct i915_power_well *power_well)
{
	const struct i915_power_well_regs *regs = power_well->desc->ops->regs;
	int pw_idx = i915_power_well_instance(power_well)->hsw.idx;
	enum phy phy = icl_aux_pw_to_phy(dev_priv, power_well);
	u32 val;

	drm_WARN_ON(&dev_priv->drm, !IS_ICELAKE(dev_priv));

	val = intel_de_read(dev_priv, ICL_PORT_CL_DW12(phy));
	intel_de_write(dev_priv, ICL_PORT_CL_DW12(phy),
		       val & ~ICL_LANE_ENABLE_AUX);

	val = intel_de_read(dev_priv, regs->driver);
	intel_de_write(dev_priv, regs->driver,
		       val & ~HSW_PWR_WELL_CTL_REQ(pw_idx));

	hsw_wait_for_power_well_disable(dev_priv, power_well);
}

#if IS_ENABLED(CPTCFG_DRM_I915_DEBUG_RUNTIME_PM)

static void icl_tc_port_assert_ref_held(struct drm_i915_private *dev_priv,
					struct i915_power_well *power_well,
					struct intel_digital_port *dig_port)
{
	if (drm_WARN_ON(&dev_priv->drm, !dig_port))
		return;

	drm_WARN_ON(&dev_priv->drm, !intel_tc_port_ref_held(dig_port));
}

#else

static void icl_tc_port_assert_ref_held(struct drm_i915_private *dev_priv,
					struct i915_power_well *power_well,
					struct intel_digital_port *dig_port)
{
}

#endif

#define TGL_AUX_PW_TO_TC_PORT(pw_idx)	((pw_idx) - TGL_PW_CTL_IDX_AUX_TC1)

static void
icl_tc_phy_aux_power_well_enable(struct drm_i915_private *dev_priv,
				 struct i915_power_well *power_well)
{
	enum aux_ch aux_ch = icl_aux_pw_to_ch(power_well);
	struct intel_digital_port *dig_port = aux_ch_to_digital_port(dev_priv, aux_ch);
	const struct i915_power_well_regs *regs = power_well->desc->ops->regs;
	bool is_tbt = power_well->desc->is_tc_tbt;
	bool timeout_expected;
	u32 val;

	icl_tc_port_assert_ref_held(dev_priv, power_well, dig_port);

	val = intel_de_read(dev_priv, DP_AUX_CH_CTL(aux_ch));
	val &= ~DP_AUX_CH_CTL_TBT_IO;
	if (is_tbt)
		val |= DP_AUX_CH_CTL_TBT_IO;
	intel_de_write(dev_priv, DP_AUX_CH_CTL(aux_ch), val);

	val = intel_de_read(dev_priv, regs->driver);
	intel_de_write(dev_priv, regs->driver,
		       val | HSW_PWR_WELL_CTL_REQ(i915_power_well_instance(power_well)->hsw.idx));

	/*
	 * An AUX timeout is expected if the TBT DP tunnel is down,
	 * or need to enable AUX on a legacy TypeC port as part of the TC-cold
	 * exit sequence.
	 */
	timeout_expected = is_tbt || intel_tc_cold_requires_aux_pw(dig_port);

	hsw_wait_for_power_well_enable(dev_priv, power_well, timeout_expected);

	if (!is_tbt) {
		enum tc_port tc_port;

		tc_port = TGL_AUX_PW_TO_TC_PORT(i915_power_well_instance(power_well)->hsw.idx);
		intel_de_write(dev_priv, HIP_INDEX_REG(tc_port),
			       HIP_INDEX_VAL(tc_port, 0x2));

		if (intel_de_wait_for_set(dev_priv, DKL_CMN_UC_DW_27(tc_port),
					  DKL_CMN_UC_DW27_UC_HEALTH, 1))
			drm_warn(&dev_priv->drm,
				 "Timeout waiting TC uC health\n");
	}
}

static void
icl_aux_power_well_enable(struct drm_i915_private *dev_priv,
			  struct i915_power_well *power_well)
{
	enum phy phy = icl_aux_pw_to_phy(dev_priv, power_well);

	if (intel_phy_is_tc(dev_priv, phy))
		return icl_tc_phy_aux_power_well_enable(dev_priv, power_well);
	else if (IS_ICELAKE(dev_priv))
		return icl_combo_phy_aux_power_well_enable(dev_priv,
							   power_well);
	else
		return hsw_power_well_enable(dev_priv, power_well);
}

static void
icl_aux_power_well_disable(struct drm_i915_private *dev_priv,
			   struct i915_power_well *power_well)
{
	enum phy phy = icl_aux_pw_to_phy(dev_priv, power_well);

	if (intel_phy_is_tc(dev_priv, phy))
		return hsw_power_well_disable(dev_priv, power_well);
	else if (IS_ICELAKE(dev_priv))
		return icl_combo_phy_aux_power_well_disable(dev_priv,
							    power_well);
	else
		return hsw_power_well_disable(dev_priv, power_well);
}

/*
 * We should only use the power well if we explicitly asked the hardware to
 * enable it, so check if it's enabled and also check if we've requested it to
 * be enabled.
 */
static bool hsw_power_well_enabled(struct drm_i915_private *dev_priv,
				   struct i915_power_well *power_well)
{
	const struct i915_power_well_regs *regs = power_well->desc->ops->regs;
	int pw_idx = i915_power_well_instance(power_well)->hsw.idx;
	u32 mask = HSW_PWR_WELL_CTL_REQ(pw_idx) |
		   HSW_PWR_WELL_CTL_STATE(pw_idx);
	u32 val;

	val = intel_de_read(dev_priv, regs->driver);
	return (val & mask) == mask;
}

static void assert_can_enable_dc9(struct drm_i915_private *dev_priv)
{
	drm_WARN_ONCE(&dev_priv->drm,
		      (intel_de_read(dev_priv, DC_STATE_EN) & DC_STATE_EN_DC9),
		      "DC9 already programmed to be enabled.\n");
	drm_WARN_ONCE(&dev_priv->drm,
		      intel_de_read(dev_priv, DC_STATE_EN) &
		      DC_STATE_EN_UPTO_DC5,
		      "DC5 still not disabled to enable DC9.\n");
	drm_WARN_ONCE(&dev_priv->drm,
		      intel_de_read(dev_priv, HSW_PWR_WELL_CTL2) &
		      HSW_PWR_WELL_CTL_REQ(SKL_PW_CTL_IDX_PW_2),
		      "Power well 2 on.\n");
	drm_WARN_ONCE(&dev_priv->drm, intel_irqs_enabled(dev_priv),
		      "Interrupts not disabled yet.\n");

	 /*
	  * TODO: check for the following to verify the conditions to enter DC9
	  * state are satisfied:
	  * 1] Check relevant display engine registers to verify if mode set
	  * disable sequence was followed.
	  * 2] Check if display uninitialize sequence is initialized.
	  */
}

static void assert_can_disable_dc9(struct drm_i915_private *dev_priv)
{
	drm_WARN_ONCE(&dev_priv->drm, intel_irqs_enabled(dev_priv),
		      "Interrupts not disabled yet.\n");
	drm_WARN_ONCE(&dev_priv->drm,
		      intel_de_read(dev_priv, DC_STATE_EN) &
		      DC_STATE_EN_UPTO_DC5,
		      "DC5 still not disabled.\n");

	 /*
	  * TODO: check for the following to verify DC9 state was indeed
	  * entered before programming to disable it:
	  * 1] Check relevant display engine registers to verify if mode
	  *  set disable sequence was followed.
	  * 2] Check if display uninitialize sequence is initialized.
	  */
}

static void gen9_write_dc_state(struct drm_i915_private *dev_priv,
				u32 state)
{
	int rewrites = 0;
	int rereads = 0;
	u32 v;

	intel_de_write(dev_priv, DC_STATE_EN, state);

	/* It has been observed that disabling the dc6 state sometimes
	 * doesn't stick and dmc keeps returning old value. Make sure
	 * the write really sticks enough times and also force rewrite until
	 * we are confident that state is exactly what we want.
	 */
	do  {
		v = intel_de_read(dev_priv, DC_STATE_EN);

		if (v != state) {
			intel_de_write(dev_priv, DC_STATE_EN, state);
			rewrites++;
			rereads = 0;
		} else if (rereads++ > 5) {
			break;
		}

	} while (rewrites < 100);

	if (v != state)
		drm_err(&dev_priv->drm,
			"Writing dc state to 0x%x failed, now 0x%x\n",
			state, v);

	/* Most of the times we need one retry, avoid spam */
	if (rewrites > 1)
		drm_dbg_kms(&dev_priv->drm,
			    "Rewrote dc state to 0x%x %d times\n",
			    state, rewrites);
}

static u32 gen9_dc_mask(struct drm_i915_private *dev_priv)
{
	u32 mask;

	mask = DC_STATE_EN_UPTO_DC5;
	mask |= DC_STATE_EN_DC3CO | DC_STATE_EN_UPTO_DC6 | DC_STATE_EN_DC9;

	return mask;
}

void gen9_sanitize_dc_state(struct drm_i915_private *dev_priv)
{
	u32 val;

	if (!HAS_DISPLAY(dev_priv))
		return;

	val = intel_de_read(dev_priv, DC_STATE_EN) & gen9_dc_mask(dev_priv);

	drm_dbg_kms(&dev_priv->drm,
		    "Resetting DC state tracking from %02x to %02x\n",
		    dev_priv->dmc.dc_state, val);
	dev_priv->dmc.dc_state = val;
}

/**
 * gen9_set_dc_state - set target display C power state
 * @dev_priv: i915 device instance
 * @state: target DC power state
 * - DC_STATE_DISABLE
 * - DC_STATE_EN_UPTO_DC5
 * - DC_STATE_EN_UPTO_DC6
 * - DC_STATE_EN_DC9
 *
 * Signal to DMC firmware/HW the target DC power state passed in @state.
 * DMC/HW can turn off individual display clocks and power rails when entering
 * a deeper DC power state (higher in number) and turns these back when exiting
 * that state to a shallower power state (lower in number). The HW will decide
 * when to actually enter a given state on an on-demand basis, for instance
 * depending on the active state of display pipes. The state of display
 * registers backed by affected power rails are saved/restored as needed.
 *
 * Based on the above enabling a deeper DC power state is asynchronous wrt.
 * enabling it. Disabling a deeper power state is synchronous: for instance
 * setting %DC_STATE_DISABLE won't complete until all HW resources are turned
 * back on and register state is restored. This is guaranteed by the MMIO write
 * to DC_STATE_EN blocking until the state is restored.
 */
void gen9_set_dc_state(struct drm_i915_private *dev_priv, u32 state)
{
	u32 val;
	u32 mask;

	if (!HAS_DISPLAY(dev_priv))
		return;

	if (drm_WARN_ON_ONCE(&dev_priv->drm,
			     state & ~dev_priv->dmc.allowed_dc_mask))
		state &= dev_priv->dmc.allowed_dc_mask;

	val = intel_de_read(dev_priv, DC_STATE_EN);
	mask = gen9_dc_mask(dev_priv);
	drm_dbg_kms(&dev_priv->drm, "Setting DC state from %02x to %02x\n",
		    val & mask, state);

	/* Check if DMC is ignoring our DC state requests */
	if ((val & mask) != dev_priv->dmc.dc_state)
		drm_err(&dev_priv->drm, "DC state mismatch (0x%x -> 0x%x)\n",
			dev_priv->dmc.dc_state, val & mask);

	val &= ~mask;
	val |= state;

	gen9_write_dc_state(dev_priv, val);

	dev_priv->dmc.dc_state = val & mask;
}

static void tgl_enable_dc3co(struct drm_i915_private *dev_priv)
{
	drm_dbg_kms(&dev_priv->drm, "Enabling DC3CO\n");
	gen9_set_dc_state(dev_priv, DC_STATE_EN_DC3CO);
}

static void tgl_disable_dc3co(struct drm_i915_private *dev_priv)
{
	u32 val;

	drm_dbg_kms(&dev_priv->drm, "Disabling DC3CO\n");
	val = intel_de_read(dev_priv, DC_STATE_EN);
	val &= ~DC_STATE_DC3CO_STATUS;
	intel_de_write(dev_priv, DC_STATE_EN, val);
	gen9_set_dc_state(dev_priv, DC_STATE_DISABLE);
	/*
	 * Delay of 200us DC3CO Exit time B.Spec 49196
	 */
	usleep_range(200, 210);
}

static void assert_can_enable_dc5(struct drm_i915_private *dev_priv)
{
	enum i915_power_well_id high_pg;

	/* Power wells at this level and above must be disabled for DC5 entry */
	if (DISPLAY_VER(dev_priv) == 12)
		high_pg = ICL_DISP_PW_3;
	else
		high_pg = SKL_DISP_PW_2;

	drm_WARN_ONCE(&dev_priv->drm,
		      intel_display_power_well_is_enabled(dev_priv, high_pg),
		      "Power wells above platform's DC5 limit still enabled.\n");

	drm_WARN_ONCE(&dev_priv->drm,
		      (intel_de_read(dev_priv, DC_STATE_EN) &
		       DC_STATE_EN_UPTO_DC5),
		      "DC5 already programmed to be enabled.\n");
	assert_rpm_wakelock_held(&dev_priv->runtime_pm);

	assert_dmc_loaded(dev_priv);
}

void gen9_enable_dc5(struct drm_i915_private *dev_priv)
{
	assert_can_enable_dc5(dev_priv);

	drm_dbg_kms(&dev_priv->drm, "Enabling DC5\n");

	gen9_set_dc_state(dev_priv, DC_STATE_EN_UPTO_DC5);
}

static void assert_can_enable_dc6(struct drm_i915_private *dev_priv)
{
	drm_WARN_ONCE(&dev_priv->drm,
		      intel_de_read(dev_priv, UTIL_PIN_CTL) & UTIL_PIN_ENABLE,
		      "Backlight is not disabled.\n");
	drm_WARN_ONCE(&dev_priv->drm,
		      (intel_de_read(dev_priv, DC_STATE_EN) &
		       DC_STATE_EN_UPTO_DC6),
		      "DC6 already programmed to be enabled.\n");

	assert_dmc_loaded(dev_priv);
}

void skl_enable_dc6(struct drm_i915_private *dev_priv)
{
	assert_can_enable_dc6(dev_priv);

	drm_dbg_kms(&dev_priv->drm, "Enabling DC6\n");

	gen9_set_dc_state(dev_priv, DC_STATE_EN_UPTO_DC6);
}

void bxt_enable_dc9(struct drm_i915_private *dev_priv)
{
	assert_can_enable_dc9(dev_priv);

	drm_dbg_kms(&dev_priv->drm, "Enabling DC9\n");
	/*
	 * Power sequencer reset is not needed on
	 * platforms with South Display Engine on PCH,
	 * because PPS registers are always on.
	 */
	gen9_set_dc_state(dev_priv, DC_STATE_EN_DC9);
}

void bxt_disable_dc9(struct drm_i915_private *dev_priv)
{
	assert_can_disable_dc9(dev_priv);

	drm_dbg_kms(&dev_priv->drm, "Disabling DC9\n");

	gen9_set_dc_state(dev_priv, DC_STATE_DISABLE);
}

static void hsw_power_well_sync_hw(struct drm_i915_private *dev_priv,
				   struct i915_power_well *power_well)
{
	const struct i915_power_well_regs *regs = power_well->desc->ops->regs;
	int pw_idx = i915_power_well_instance(power_well)->hsw.idx;
	u32 mask = HSW_PWR_WELL_CTL_REQ(pw_idx);
	u32 bios_req = intel_de_read(dev_priv, regs->bios);

	/* Take over the request bit if set by BIOS. */
	if (bios_req & mask) {
		u32 drv_req = intel_de_read(dev_priv, regs->driver);

		if (!(drv_req & mask))
			intel_de_write(dev_priv, regs->driver, drv_req | mask);
		intel_de_write(dev_priv, regs->bios, bios_req & ~mask);
	}
}

static bool gen9_dc_off_power_well_enabled(struct drm_i915_private *dev_priv,
					   struct i915_power_well *power_well)
{
	return ((intel_de_read(dev_priv, DC_STATE_EN) & DC_STATE_EN_DC3CO) == 0 &&
		(intel_de_read(dev_priv, DC_STATE_EN) & DC_STATE_EN_UPTO_DC5_DC6_MASK) == 0);
}

static void gen9_assert_dbuf_enabled(struct drm_i915_private *dev_priv)
{
	u8 hw_enabled_dbuf_slices = intel_enabled_dbuf_slices_mask(dev_priv);
	u8 enabled_dbuf_slices = dev_priv->dbuf.enabled_slices;

	drm_WARN(&dev_priv->drm,
		 hw_enabled_dbuf_slices != enabled_dbuf_slices,
		 "Unexpected DBuf power power state (0x%08x, expected 0x%08x)\n",
		 hw_enabled_dbuf_slices,
		 enabled_dbuf_slices);
}

void gen9_disable_dc_states(struct drm_i915_private *dev_priv)
{
	struct intel_cdclk_config cdclk_config = {};

	if (dev_priv->dmc.target_dc_state == DC_STATE_EN_DC3CO) {
		tgl_disable_dc3co(dev_priv);
		return;
	}

	gen9_set_dc_state(dev_priv, DC_STATE_DISABLE);

	if (!HAS_DISPLAY(dev_priv))
		return;

	intel_cdclk_get_cdclk(dev_priv, &cdclk_config);
	/* Can't read out voltage_level so can't use intel_cdclk_changed() */
	drm_WARN_ON(&dev_priv->drm,
		    intel_cdclk_needs_modeset(&dev_priv->cdclk.hw,
					      &cdclk_config));

	gen9_assert_dbuf_enabled(dev_priv);

	/*
	 * DMC retains HW context only for port A, the other combo
	 * PHY's HW context for port B is lost after DC transitions,
	 * so we need to restore it manually.
	 */
	intel_combo_phy_init(dev_priv);
}

static void gen9_dc_off_power_well_enable(struct drm_i915_private *dev_priv,
					  struct i915_power_well *power_well)
{
	gen9_disable_dc_states(dev_priv);
}

static void gen9_dc_off_power_well_disable(struct drm_i915_private *dev_priv,
					   struct i915_power_well *power_well)
{
	if (!intel_dmc_has_payload(dev_priv))
		return;

	switch (dev_priv->dmc.target_dc_state) {
	case DC_STATE_EN_DC3CO:
		tgl_enable_dc3co(dev_priv);
		break;
	case DC_STATE_EN_UPTO_DC6:
		skl_enable_dc6(dev_priv);
		break;
	case DC_STATE_EN_UPTO_DC5:
		gen9_enable_dc5(dev_priv);
		break;
	}
}

static void
tgl_tc_cold_request(struct drm_i915_private *i915, bool block)
{
	u8 tries = 0;
	int ret;

	while (1) {
		u32 low_val;
		u32 high_val = 0;

		if (block)
			low_val = TGL_PCODE_EXIT_TCCOLD_DATA_L_BLOCK_REQ;
		else
			low_val = TGL_PCODE_EXIT_TCCOLD_DATA_L_UNBLOCK_REQ;

		/*
		 * Spec states that we should timeout the request after 200us
		 * but the function below will timeout after 500us
		 */
		ret = snb_pcode_read(&i915->uncore, TGL_PCODE_TCCOLD, &low_val, &high_val);
		if (ret == 0) {
			if (block &&
			    (low_val & TGL_PCODE_EXIT_TCCOLD_DATA_L_EXIT_FAILED))
				ret = -EIO;
			else
				break;
		}

		if (++tries == 3)
			break;

		msleep(1);
	}

	if (ret)
		drm_err(&i915->drm, "TC cold %sblock failed\n",
			block ? "" : "un");
	else
		drm_dbg_kms(&i915->drm, "TC cold %sblock succeeded\n",
			    block ? "" : "un");
}

static void
tgl_tc_cold_off_power_well_enable(struct drm_i915_private *i915,
				  struct i915_power_well *power_well)
{
	tgl_tc_cold_request(i915, true);
}

static void
tgl_tc_cold_off_power_well_disable(struct drm_i915_private *i915,
				   struct i915_power_well *power_well)
{
	tgl_tc_cold_request(i915, false);
}

static void
tgl_tc_cold_off_power_well_sync_hw(struct drm_i915_private *i915,
				   struct i915_power_well *power_well)
{
	if (intel_power_well_refcount(power_well) > 0)
		tgl_tc_cold_off_power_well_enable(i915, power_well);
	else
		tgl_tc_cold_off_power_well_disable(i915, power_well);
}

static bool
tgl_tc_cold_off_power_well_is_enabled(struct drm_i915_private *dev_priv,
				      struct i915_power_well *power_well)
{
	/*
	 * Not the correctly implementation but there is no way to just read it
	 * from PCODE, so returning count to avoid state mismatch errors
	 */
	return intel_power_well_refcount(power_well);
}

static void xelpdp_aux_power_well_sync_hw(struct drm_i915_private *i915,
					  struct i915_power_well *power_well)
{
}

static void xelpdp_aux_power_well_enable(struct drm_i915_private *dev_priv,
					 struct i915_power_well *power_well)
{
	enum aux_ch aux_ch = i915_power_well_instance(power_well)->xelpdp.aux_ch;

	intel_de_rmw(dev_priv, XELPDP_DP_AUX_CH_CTL(aux_ch),
		     XELPDP_DP_AUX_CH_CTL_POWER_REQUEST,
		     XELPDP_DP_AUX_CH_CTL_POWER_REQUEST);
	/* The power status flag doesn't indicate that the power up completed. */
	usleep_range(600, 1200);
}

static void xelpdp_aux_power_well_disable(struct drm_i915_private *dev_priv,
					  struct i915_power_well *power_well)
{
	enum aux_ch aux_ch = i915_power_well_instance(power_well)->xelpdp.aux_ch;

	intel_de_rmw(dev_priv, XELPDP_DP_AUX_CH_CTL(aux_ch),
		     XELPDP_DP_AUX_CH_CTL_POWER_REQUEST,
		     0);
	usleep_range(10, 30);
}

static bool xelpdp_aux_power_well_enabled(struct drm_i915_private *dev_priv,
					  struct i915_power_well *power_well)
{
	enum aux_ch aux_ch = i915_power_well_instance(power_well)->xelpdp.aux_ch;

	return intel_de_read(dev_priv, XELPDP_DP_AUX_CH_CTL(aux_ch)) &
		XELPDP_DP_AUX_CH_CTL_POWER_STATUS;
}

static void sync_hw_noop(struct drm_i915_private *dev_priv,
			 struct i915_power_well *power_well)
{
}

static void power_well_noop(struct drm_i915_private *dev_priv,
			    struct i915_power_well *power_well)
{
}

static bool power_well_enabled(struct drm_i915_private *dev_priv,
			       struct i915_power_well *power_well)
{
	return true;
}

const struct i915_power_well_ops i9xx_always_on_power_well_ops = {
       .sync_hw = sync_hw_noop,
       .enable = power_well_noop,
       .disable = power_well_noop,
       .is_enabled = power_well_enabled,
};

static const struct i915_power_well_regs hsw_power_well_regs = {
	.bios   = HSW_PWR_WELL_CTL1,
	.driver = HSW_PWR_WELL_CTL2,
	.kvmr   = HSW_PWR_WELL_CTL3,
	.debug  = HSW_PWR_WELL_CTL4,
};

const struct i915_power_well_ops hsw_power_well_ops = {
	.regs = &hsw_power_well_regs,
	.sync_hw = hsw_power_well_sync_hw,
	.enable = hsw_power_well_enable,
	.disable = hsw_power_well_disable,
	.is_enabled = hsw_power_well_enabled,
};

const struct i915_power_well_ops gen9_dc_off_power_well_ops = {
	.sync_hw = sync_hw_noop,
	.enable = gen9_dc_off_power_well_enable,
	.disable = gen9_dc_off_power_well_disable,
	.is_enabled = gen9_dc_off_power_well_enabled,
};

static const struct i915_power_well_regs icl_aux_power_well_regs = {
	.bios	= ICL_PWR_WELL_CTL_AUX1,
	.driver	= ICL_PWR_WELL_CTL_AUX2,
	.debug	= ICL_PWR_WELL_CTL_AUX4,
};

const struct i915_power_well_ops icl_aux_power_well_ops = {
	.regs = &icl_aux_power_well_regs,
	.sync_hw = hsw_power_well_sync_hw,
	.enable = icl_aux_power_well_enable,
	.disable = icl_aux_power_well_disable,
	.is_enabled = hsw_power_well_enabled,
};

static const struct i915_power_well_regs icl_ddi_power_well_regs = {
	.bios	= ICL_PWR_WELL_CTL_DDI1,
	.driver	= ICL_PWR_WELL_CTL_DDI2,
	.debug	= ICL_PWR_WELL_CTL_DDI4,
};

const struct i915_power_well_ops icl_ddi_power_well_ops = {
	.regs = &icl_ddi_power_well_regs,
	.sync_hw = hsw_power_well_sync_hw,
	.enable = hsw_power_well_enable,
	.disable = hsw_power_well_disable,
	.is_enabled = hsw_power_well_enabled,
};

const struct i915_power_well_ops tgl_tc_cold_off_ops = {
	.sync_hw = tgl_tc_cold_off_power_well_sync_hw,
	.enable = tgl_tc_cold_off_power_well_enable,
	.disable = tgl_tc_cold_off_power_well_disable,
	.is_enabled = tgl_tc_cold_off_power_well_is_enabled,
};

const struct i915_power_well_ops xelpdp_aux_power_well_ops = {
	.sync_hw = xelpdp_aux_power_well_sync_hw,
	.enable = xelpdp_aux_power_well_enable,
	.disable = xelpdp_aux_power_well_disable,
	.is_enabled = xelpdp_aux_power_well_enabled,
};
