// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

/*
 * Power-related hwmon entries.
 */

#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/types.h>

#include "i915_drv.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_regs.h"
#include "i915_hwmon.h"
#include "intel_mchbar_regs.h"
#include "intel_pcode.h"

/*
 * SF_* - scale factors for particular quantities according to hwmon spec.
 * - power  - microwatts
 * - energy - microjoules
 */
#define SF_POWER	1000000
#define SF_ENERGY	1000000

#define FIELD_SHIFT(__mask)				    \
	(BUILD_BUG_ON_ZERO(!__builtin_constant_p(__mask)) + \
		BUILD_BUG_ON_ZERO((__mask) == 0) +	    \
		__bf_shf(__mask))

static void
_locked_with_pm_intel_uncore_rmw(struct i915_hwmon_drvdata *ddat,
				 i915_reg_t reg, u32 clear, u32 set)
{
	struct i915_hwmon *hwmon = ddat->dd_hwmon;
	struct intel_uncore *uncore = ddat->dd_uncore;
	intel_wakeref_t wakeref;

	mutex_lock(&hwmon->hwmon_lock);

	with_intel_runtime_pm(uncore->rpm, wakeref)
		intel_uncore_rmw(uncore, reg, clear, set);

	mutex_unlock(&hwmon->hwmon_lock);
}

static u64
_scale_and_shift(u32 in, u32 scale_factor, int nshift)
{
	u64 out = mul_u32_u32(scale_factor, in);

	/* Shift, rounding to nearest */
	if (nshift > 0)
		out = (out + (1 << (nshift - 1))) >> nshift;
	return out;
}

/*
 * This function's return type of u64 allows for the case where the scaling
 * of the field taken from the 32-bit register value might cause a result to
 * exceed 32 bits.
 */
static u64
_field_read_and_scale(struct i915_hwmon_drvdata *ddat, i915_reg_t rgadr,
		      u32 field_msk, int field_shift,
		      int nshift, u32 scale_factor)
{
	struct intel_uncore *uncore = ddat->dd_uncore;
	intel_wakeref_t wakeref;
	u32 reg_value;

	with_intel_runtime_pm(uncore->rpm, wakeref)
		reg_value = intel_uncore_read(uncore, rgadr);

	reg_value = (reg_value & field_msk) >> field_shift;

	return _scale_and_shift(reg_value, scale_factor, nshift);
}

static void
_field_scale_and_write(struct i915_hwmon_drvdata *ddat, i915_reg_t rgadr,
		       u32 field_msk, int field_shift,
		       int nshift, unsigned int scale_factor, long lval)
{
	u32 nval;
	u32 bits_to_clear;
	u32 bits_to_set;

	/* Computation in 64-bits to avoid overflow. Round to nearest. */
	nval = DIV_ROUND_CLOSEST_ULL((u64)lval << nshift, scale_factor);

	bits_to_clear = field_msk;
	bits_to_set = (nval << field_shift) & field_msk;

	_locked_with_pm_intel_uncore_rmw(ddat, rgadr,
					 bits_to_clear, bits_to_set);
}

/*
 * _i915_energy1_input_sub - A custom function to obtain energy1_input.
 * Use a custom function instead of the usual hwmon helpers in order to
 * guarantee 64-bits of result to user-space.
 * Units are microjoules.
 *
 * The underlying hardware register is 32-bits and is subject to overflow.
 * This function compensates for overflow of the 32-bit register by detecting
 * wrap-around and incrementing an overflow counter.
 * This only works if the register is sampled often enough to avoid
 * missing an instance of overflow - achieved either by repeated
 * queries through the API, or via a possible timer (future - TBD) that
 * ensures values are read often enough to catch all overflows.
 *
 * How long before overflow?  For example, with an example scaling bit
 * shift of 14 bits (see register *PACKAGE_POWER_SKU_UNIT) and a power draw of
 * 1000 watts, the 32-bit counter will overflow in approximately 4.36 minutes.
 *
 * Examples:
 *    1 watt:  (2^32 >> 14) /    1 W / (60 * 60 * 24) secs/day -> 3 days
 * 1000 watts: (2^32 >> 14) / 1000 W / 60             secs/min -> 4.36 minutes
 */
static int
_i915_energy1_input_sub(struct i915_hwmon_drvdata *ddat, u64 *energy)
{
	struct intel_uncore *uncore = ddat->dd_uncore;
	struct i915_hwmon *hwmon = ddat->dd_hwmon;
	struct i915_energy_info *pei = &ddat->dd_ei;
	int nshift = hwmon->scl_shift_energy;
	intel_wakeref_t wakeref;
	u32 reg_value;
	u64 vlo;
	u64 vhi;
	i915_reg_t rgaddr;

	if (ddat->dd_gtix >= 0)
		rgaddr = hwmon->rg.energy_status_tile;
	else
		rgaddr = hwmon->rg.energy_status_all;

	if (!i915_mmio_reg_valid(rgaddr))
		return -EOPNOTSUPP;

	mutex_lock(&hwmon->hwmon_lock);

	with_intel_runtime_pm(uncore->rpm, wakeref)
		reg_value = intel_uncore_read(uncore, rgaddr);

	/*
	 * The u32 register concatenated with the u32 overflow counter
	 * gives an effective energy counter size of 64-bits.  However, the
	 * computations below are done modulo 2^96 to avoid overflow during
	 * scaling in the conversion to microjoules.
	 *
	 * The low-order 64-bits of the resulting quantity are returned to
	 * the caller in units of microjoules, encoded into a decimal string.
	 *
	 * For a power of 1000 watts, 64 bits in units of microjoules will
	 * overflow after 584 years.
	 */

	if (pei->energy_counter_prev > reg_value)
		pei->energy_counter_overflow++;

	pei->energy_counter_prev = reg_value;

	/*
	 * 64-bit variables vlo and vhi are used for the scaling process.
	 * The 96-bit counter value is composed from the two 64-bit variables
	 * vhi and vlo thusly:  counter == vhi << 32 + vlo .
	 * The 32-bits of overlap between the two variables is convenient for
	 * handling overflows out of vlo.
	 */

	vlo = reg_value;
	vhi = pei->energy_counter_overflow;

	mutex_unlock(&hwmon->hwmon_lock);

	vlo = SF_ENERGY * vlo;

	/* Prepare to round to nearest */
	if (nshift > 0)
		vlo += 1 << (nshift - 1);

	/*
	 * Anything in the upper-32 bits of vlo gets added into vhi here,
	 * and then cleared from vlo.
	 */
	vhi = (SF_ENERGY * vhi) + (vlo >> 32);
	vlo &= 0xffffffffULL;

	/*
	 * Apply the right shift.
	 * - vlo shifted by itself.
	 * - vlo receiving what's shifted out of vhi.
	 * - vhi shifted by itself
	 */
	vlo = vlo >> nshift;
	vlo |= (vhi << (32 - nshift)) & 0xffffffffULL;
	vhi = vhi >> nshift;

	/* Combined to get a 64-bit result in vlo. */
	vlo |= (vhi << 32);

	*energy = vlo;

	return 0;
}

static ssize_t
i915_energy1_input_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct i915_hwmon_drvdata *ddat = dev_get_drvdata(dev);
	ssize_t ret = 0;
	u64 energy;

	if (!_i915_energy1_input_sub(ddat, &energy))
		ret = sysfs_emit(buf, "%llu\n", energy);

	return ret;
}

int
i915_energy_status_get(struct drm_i915_private *i915, u64 *energy)
{
	struct i915_hwmon *hwmon = i915->hwmon;
	struct i915_hwmon_drvdata *ddat = &hwmon->ddat;

	return _i915_energy1_input_sub(ddat, energy);
}

static ssize_t
i915_power1_max_default_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct i915_hwmon_drvdata *ddat = dev_get_drvdata(dev);
	struct i915_hwmon *hwmon = ddat->dd_hwmon;
	u64 val = 0; /* uapi specifies to keep visible but return 0 if unsupported */

	if (i915_mmio_reg_valid(hwmon->rg.pkg_power_sku))
		val = _field_read_and_scale(ddat,
					    hwmon->rg.pkg_power_sku,
					    PKG_PKG_TDP,
					    FIELD_SHIFT(PKG_PKG_TDP),
					    hwmon->scl_shift_power,
					    SF_POWER);
	return sysfs_emit(buf, "%llu\n", val);
}

static SENSOR_DEVICE_ATTR(power1_max_default, 0444,
			  i915_power1_max_default_show, NULL, 0);
static SENSOR_DEVICE_ATTR(energy1_input, 0444,
			  i915_energy1_input_show, NULL, 0);

static struct attribute *hwmon_attributes[] = {
	&sensor_dev_attr_power1_max_default.dev_attr.attr,
	&sensor_dev_attr_energy1_input.dev_attr.attr,
	NULL
};

static struct sensor_device_attribute sensor_dev_attr_energy1_input_gt =
	SENSOR_ATTR(energy1_input, 0444,
		    i915_energy1_input_show, NULL, 0);

static struct attribute *hwmon_attributes_gt[] = {
	&sensor_dev_attr_energy1_input_gt.dev_attr.attr,
	NULL
};

static umode_t hwmon_attributes_visible(struct kobject *kobj,
					struct attribute *attr, int index)
{
	struct device *dev = kobj_to_dev(kobj);
	struct i915_hwmon_drvdata *ddat = dev_get_drvdata(dev);
	struct drm_i915_private *i915 = ddat->dd_uncore->i915;
	struct i915_hwmon *hwmon = ddat->dd_hwmon;
	i915_reg_t rgadr;

	if (attr == &sensor_dev_attr_energy1_input.dev_attr.attr)
		rgadr = hwmon->rg.energy_status_all;
	else if (attr == &sensor_dev_attr_power1_max_default.dev_attr.attr)
		return IS_DGFX(i915) ? attr->mode : 0;
	else
		return 0;

	if (!i915_mmio_reg_valid(rgadr))
		return 0;

	return attr->mode;
}

static umode_t hwmon_attributes_gt_visible(struct kobject *kobj,
					   struct attribute *attr, int index)
{
	struct device *dev = kobj_to_dev(kobj);
	struct i915_hwmon_drvdata *ddat = dev_get_drvdata(dev);
	struct i915_hwmon *hwmon = ddat->dd_hwmon;
	i915_reg_t rgadr;

	if (attr == &sensor_dev_attr_energy1_input_gt.dev_attr.attr)
		rgadr = hwmon->rg.energy_status_tile;
	else
		return 0;

	if (!i915_mmio_reg_valid(rgadr))
		return 0;

	return attr->mode;
}

static const struct attribute_group hwmon_attrgroup = {
	.attrs = hwmon_attributes,
	.is_visible = hwmon_attributes_visible,
};

static const struct attribute_group *hwmon_groups[] = {
	&hwmon_attrgroup,
	NULL
};

static const struct attribute_group hwmon_attrgroup_gt = {
	.attrs = hwmon_attributes_gt,
	.is_visible = hwmon_attributes_gt_visible,
};

static const struct attribute_group *hwmg_gt[] = {
	&hwmon_attrgroup_gt,
	NULL
};

/*
 * HWMON SENSOR TYPE = hwmon_power
 *  - Sustained Power (power1_max)
 *  - Peak power      (power1_crit)
 */
static const u32 i915_config_power[] = {
	HWMON_P_MAX | HWMON_P_CRIT,
	0
};

static const struct hwmon_channel_info i915_power = {
	.type = hwmon_power,
	.config = i915_config_power,
};

/*
 * HWMON SENSOR TYPE = hwmon_in
 *  - Voltage Input value (in0_input)
 */
static const u32 i915_config_in[] = {
	HWMON_I_INPUT,
	0
};

static const struct hwmon_channel_info i915_in = {
	.type = hwmon_in,
	.config = i915_config_in,
};

static const struct hwmon_channel_info *i915_info[] = {
	&i915_power,
	&i915_in,
	NULL
};

static umode_t
i915_power_is_visible(const struct i915_hwmon_drvdata *ddat, u32 attr, int chan)
{
	struct drm_i915_private *i915 = ddat->dd_uncore->i915;
	struct i915_hwmon *hwmon = ddat->dd_hwmon;
	i915_reg_t rgadr;

	switch (attr) {
	case hwmon_power_max:
		rgadr = hwmon->rg.pkg_rapl_limit;
		break;
	case hwmon_power_crit:
		return IS_DGFX(i915) ? 0664 : 0;
	default:
		return 0;
	}

	if (!i915_mmio_reg_valid(rgadr))
		return 0;

	return 0664;
}

static int
i915_power_read(struct i915_hwmon_drvdata *ddat, u32 attr, int chan, long *val)
{
	struct drm_i915_private *i915 = ddat->dd_uncore->i915;
	struct i915_hwmon *hwmon = ddat->dd_hwmon;
	int ret = 0;
	u32 uval;

	switch (attr) {
	case hwmon_power_max:
		*val = _field_read_and_scale(ddat,
					     hwmon->rg.pkg_rapl_limit,
					     PKG_PWR_LIM_1,
					     FIELD_SHIFT(PKG_PWR_LIM_1),
					     hwmon->scl_shift_power,
					     SF_POWER);
		break;
	case hwmon_power_crit:
		ret = __snb_pcode_read(i915, PCODE_POWER_SETUP,
				       POWER_SETUP_SUBCOMMAND_READ_I1, 0, &uval);
		if (ret)
			return ret;
		if (!(uval & POWER_SETUP_I1_WATTS)) {
			drm_err(&i915->drm, "Power I1 value is in Amperes\n");
			return -ENODEV;
		}
		*val = _scale_and_shift(uval, SF_POWER, POWER_SETUP_I1_SHIFT);
		break;
	default:
		ret = -EOPNOTSUPP;
	}

	return ret;
}

static int
i915_power_write(struct i915_hwmon_drvdata *ddat, u32 attr, int chan, long val)
{
	struct i915_hwmon *hwmon = ddat->dd_hwmon;
	u32 uval;
	int ret = 0;

	switch (attr) {
	case hwmon_power_max:
		_field_scale_and_write(ddat,
				       hwmon->rg.pkg_rapl_limit,
				       PKG_PWR_LIM_1,
				       FIELD_SHIFT(PKG_PWR_LIM_1),
				       hwmon->scl_shift_power,
				       SF_POWER, val);
		break;
	case hwmon_power_crit:
		uval = DIV_ROUND_CLOSEST_ULL(val << POWER_SETUP_I1_SHIFT, SF_POWER);
		ret = __snb_pcode_write(ddat->dd_uncore->i915, PCODE_POWER_SETUP,
					POWER_SETUP_SUBCOMMAND_WRITE_I1, 0, uval);
		break;
	default:
		ret = -EOPNOTSUPP;
	}

	return ret;
}

static umode_t
i915_in_is_visible(const struct i915_hwmon_drvdata *ddat, u32 attr)
{
	struct drm_i915_private *i915 = ddat->dd_uncore->i915;

	switch (attr) {
	case hwmon_in_input:
		return (IS_DG1(i915) || IS_DG2(i915)) ? 0444 : 0;
	default:
		return 0;
	}

	return 0444;
}

static int
i915_in_read(struct i915_hwmon_drvdata *ddat, u32 attr, long *val)
{
	struct i915_hwmon *hwmon = ddat->dd_hwmon;
	intel_wakeref_t wakeref;
	u32 reg_value;

	switch (attr) {
	case hwmon_in_input:
		with_intel_runtime_pm(ddat->dd_uncore->rpm, wakeref)
			reg_value = intel_uncore_read(ddat->dd_uncore, hwmon->rg.gt_perf_status);
		*val = DIV_ROUND_CLOSEST(REG_FIELD_GET(GEN12_VOLTAGE_MASK, reg_value) * 25, 10);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static umode_t
i915_is_visible(const void *drvdata, enum hwmon_sensor_types type,
		u32 attr, int channel)
{
	struct i915_hwmon_drvdata *ddat = (struct i915_hwmon_drvdata *)drvdata;

	switch (type) {
	case hwmon_power:
		return i915_power_is_visible(ddat, attr, channel);
	case hwmon_in:
		return i915_in_is_visible(ddat, attr);
	default:
		return 0;
	}
}

static int
i915_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
	  int channel, long *val)
{
	struct i915_hwmon_drvdata *ddat = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_power:
		return i915_power_read(ddat, attr, channel, val);
	case hwmon_in:
		return i915_in_read(ddat, attr, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int
i915_write(struct device *dev, enum hwmon_sensor_types type, u32 attr,
	   int channel, long val)
{
	struct i915_hwmon_drvdata *ddat = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_power:
		return i915_power_write(ddat, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_ops i915_hwmon_ops = {
	.is_visible = i915_is_visible,
	.read = i915_read,
	.write = i915_write,
};

static const struct hwmon_chip_info i915_chip_info = {
	.ops = &i915_hwmon_ops,
	.info = i915_info,
};

static void
i915_hwmon_get_preregistration_info(struct drm_i915_private *i915)
{
	struct i915_hwmon *hwmon = i915->hwmon;
	struct intel_uncore *uncore = &i915->uncore;
	struct i915_hwmon_drvdata *ddat = &hwmon->ddat;
	struct i915_energy_info *pei;
	intel_wakeref_t wakeref;
	u32 val_sku_unit;
	__le32 le_sku_unit;
	int gtix;
	struct intel_gt *gt;

	if (IS_DG1(i915) || IS_DG2(i915)) {
		hwmon->rg.pkg_power_sku_unit = PCU_PACKAGE_POWER_SKU_UNIT;
		hwmon->rg.pkg_power_sku = INVALID_MMIO_REG;
		hwmon->rg.pkg_rapl_limit = PCU_PACKAGE_RAPL_LIMIT;
		hwmon->rg.energy_status_all = PCU_PACKAGE_ENERGY_STATUS;
		hwmon->rg.energy_status_tile = INVALID_MMIO_REG;
		hwmon->rg.gt_perf_status = GEN12_RPSTAT1;
	} else if (IS_XEHPSDV(i915)) {
		hwmon->rg.pkg_power_sku_unit = GT0_PACKAGE_POWER_SKU_UNIT;
		hwmon->rg.pkg_power_sku = GT0_PACKAGE_POWER_SKU;
		hwmon->rg.pkg_rapl_limit = GT0_PACKAGE_RAPL_LIMIT;
		hwmon->rg.energy_status_all = GT0_PLATFORM_ENERGY_STATUS;
		hwmon->rg.energy_status_tile = GT0_PACKAGE_ENERGY_STATUS;
		hwmon->rg.gt_perf_status = INVALID_MMIO_REG;
	} else if (IS_PONTEVECCHIO(i915)) {
		hwmon->rg.pkg_power_sku_unit = PVC_GT0_PACKAGE_POWER_SKU_UNIT;
		hwmon->rg.pkg_power_sku = PVC_GT0_PACKAGE_POWER_SKU;
		hwmon->rg.pkg_rapl_limit = PVC_GT0_PACKAGE_RAPL_LIMIT;
		hwmon->rg.energy_status_all = PVC_GT0_PLATFORM_ENERGY_STATUS;
		hwmon->rg.energy_status_tile = PVC_GT0_PACKAGE_ENERGY_STATUS;
		hwmon->rg.gt_perf_status = INVALID_MMIO_REG;
	} else {
		hwmon->rg.pkg_power_sku_unit = INVALID_MMIO_REG;
		hwmon->rg.pkg_power_sku = INVALID_MMIO_REG;
		hwmon->rg.pkg_rapl_limit = INVALID_MMIO_REG;
		hwmon->rg.energy_status_all = INVALID_MMIO_REG;
		hwmon->rg.energy_status_tile = INVALID_MMIO_REG;
		hwmon->rg.gt_perf_status = INVALID_MMIO_REG;
	}

	wakeref = intel_runtime_pm_get(uncore->rpm);

	/*
	 * The contents of register hwmon->rg.pkg_power_sku_unit do not change,
	 * so read it once and store the shift values.
	 *
	 * For some platforms, this value is defined as available "for all
	 * tiles", with the values consistent across all tiles.
	 * In this case, use the tile 0 value for all.
	 */
	if (i915_mmio_reg_valid(hwmon->rg.pkg_power_sku_unit))
		val_sku_unit = intel_uncore_read(uncore,
						 hwmon->rg.pkg_power_sku_unit);
	else
		val_sku_unit = 0;

	pei = &ddat->dd_ei;
	pei->energy_counter_overflow = 0;

	if (i915_mmio_reg_valid(hwmon->rg.energy_status_all))
		pei->energy_counter_prev =
			intel_uncore_read(uncore, hwmon->rg.energy_status_all);
	else
		pei->energy_counter_prev = 0;

	intel_runtime_pm_put(uncore->rpm, wakeref);

	le_sku_unit = cpu_to_le32(val_sku_unit);
	hwmon->scl_shift_power = le32_get_bits(le_sku_unit, PKG_PWR_UNIT);
	hwmon->scl_shift_energy = le32_get_bits(le_sku_unit, PKG_ENERGY_UNIT);

	/*
	 * The value of power1_max is reset to the default on reboot, but is
	 * not reset by a module unload/load sequence.  To allow proper
	 * functioning after a module reload, the value for power1_max is
	 * restored to its original value at module unload time in
	 * i915_hwmon_unregister().
	 */
	hwmon->power_max_initial_value =
		(u32)_field_read_and_scale(ddat,
					   hwmon->rg.pkg_rapl_limit,
					   PKG_PWR_LIM_1,
					   FIELD_SHIFT(PKG_PWR_LIM_1),
					   hwmon->scl_shift_power, SF_POWER);

	for_each_gt(i915, gtix, gt) {
		pei = &hwmon->ddat_gt[gtix].dd_ei;
		pei->energy_counter_overflow = 0;
		pei->energy_counter_prev = 0;
	}

	if (i915_mmio_reg_valid(hwmon->rg.energy_status_tile)) {
		for_each_gt(i915, gtix, gt) {
			pei = &hwmon->ddat_gt[gtix].dd_ei;
			wakeref = intel_runtime_pm_get(gt->uncore->rpm);
			pei->energy_counter_prev =
				intel_uncore_read(gt->uncore,
						  hwmon->rg.energy_status_tile);
			intel_runtime_pm_put(gt->uncore->rpm, wakeref);
		}
	}
}

/*
 * any_attrs_visible() - Return 1 if any specified attributes is visible
 * @kobj: pointer to the kobject of the i915 hwmon device.
 * @hmag_tab: table of zero or more "struct attribute_group *", ended by NULL.
 */
static int any_attrs_visible(struct kobject *kobj,
			     const struct attribute_group **hmag_tab)
{
	const struct attribute_group **hmag_tab_entry;
	const struct attribute_group *hmag;
	struct attribute **hma;

	for (hmag_tab_entry = hmag_tab; *hmag_tab_entry; hmag_tab_entry++) {
		hmag = *hmag_tab_entry;
		hma = hmag->attrs;
		if (*hma && !hmag->is_visible)
			return 1;
		for (; *hma; hma++) {
			if (hmag->is_visible(kobj, *hma, 0))
				return 1;
		}
	}

	return 0;
}

void i915_hwmon_register(struct drm_i915_private *i915)
{
	struct device *dev = i915->drm.dev;
	struct i915_hwmon *hwmon;
	struct device *hwmon_dev;
	struct i915_hwmon_drvdata *ddat;
	struct i915_hwmon_drvdata *ddat_gt;
	struct intel_gt *gt;
	int gtix;
	struct kobject *kobj;
	const char *ddname;

	hwmon = kzalloc(sizeof(*hwmon), GFP_KERNEL);
	if (!hwmon)
		return;

	i915->hwmon = hwmon;

	mutex_init(&hwmon->hwmon_lock);

	ddat = &hwmon->ddat;

	ddat->dd_hwmon = hwmon;
	ddat->dd_uncore = &i915->uncore;
	snprintf(ddat->dd_name, sizeof(ddat->dd_name), "i915");
	ddat->dd_gtix = -1;

	for_each_gt(i915, gtix, gt) {
		ddat_gt = hwmon->ddat_gt + gtix;

		ddat_gt->dd_hwmon = hwmon;
		ddat_gt->dd_uncore = gt->uncore;
		snprintf(ddat_gt->dd_name, sizeof(ddat_gt->dd_name),
			 "i915_gt%u", gtix);
		ddat_gt->dd_gtix = gtix;
	}

	i915_hwmon_get_preregistration_info(i915);

	/*  hwmon_dev points to device hwmon<i> */
	hwmon_dev = hwmon_device_register_with_info(dev, ddat->dd_name,
						    ddat,
						    &i915_chip_info,
						    hwmon_groups);

	if (IS_ERR(hwmon_dev)) {
		mutex_destroy(&hwmon->hwmon_lock);
		i915->hwmon = NULL;
		kfree(hwmon);
		return;
	}

	ddat->dd_hwmon_dev = hwmon_dev;

	kobj = &hwmon_dev->kobj;

	/* Create per-gt directories only if a per-gt attribute is visible. */
	if (any_attrs_visible(kobj, hwmg_gt)) {
		for_each_gt(i915, gtix, gt) {
			ddat_gt = hwmon->ddat_gt + gtix;

			ddname = ddat_gt->dd_name;
			hwmon_dev = hwmon_device_register_with_groups(dev,
								      ddname,
								      ddat_gt,
								      hwmg_gt);
			if (!IS_ERR(hwmon_dev))
				ddat_gt->dd_hwmon_dev = hwmon_dev;
		}
	}
}

void i915_hwmon_unregister(struct drm_i915_private *i915)
{
	struct i915_hwmon *hwmon;
	struct i915_hwmon_drvdata *ddat;
	struct intel_gt *gt;
	int gtix;

	hwmon = fetch_and_zero(&i915->hwmon);
	if (!hwmon)
		return;

	ddat = &hwmon->ddat;

	if (hwmon->power_max_initial_value) {
		/* Restore power1_max. */
		_field_scale_and_write(ddat, hwmon->rg.pkg_rapl_limit,
				       PKG_PWR_LIM_1,
				       FIELD_SHIFT(PKG_PWR_LIM_1),
				       hwmon->scl_shift_power,
				       SF_POWER,
				       hwmon->power_max_initial_value);
	}

	for_each_gt(i915, gtix, gt) {
		struct i915_hwmon_drvdata *ddat_gt;

		ddat_gt = hwmon->ddat_gt + gtix;

		if (ddat_gt->dd_hwmon_dev) {
			hwmon_device_unregister(ddat_gt->dd_hwmon_dev);
			ddat_gt->dd_hwmon_dev = NULL;
		}
	}

	if (ddat->dd_hwmon_dev)
		hwmon_device_unregister(ddat->dd_hwmon_dev);

	mutex_destroy(&hwmon->hwmon_lock);

	kfree(hwmon);
}
