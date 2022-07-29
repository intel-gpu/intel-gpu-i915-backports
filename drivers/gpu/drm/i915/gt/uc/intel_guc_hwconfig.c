// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

#include "gt/intel_gt.h"
#include "gt/intel_gt_regs.h"
#include "gt/intel_hwconfig_types.h"
#include "i915_drv.h"
#include "i915_memcpy.h"
#include "intel_guc_hwconfig.h"

/* Auto-generated tables: */
#include "intel_guc_hwconfig_auto.c"

static
inline struct intel_guc *hwconfig_to_guc(struct intel_guc_hwconfig *hwconfig)
{
	return container_of(hwconfig, struct intel_guc, hwconfig);
}

/**
 * GuC has a blob containing the device information (hwconfig), which is a
 * simple and flexible KLV (Key/Length/Value) formatted table.
 *
 * For instance it could be simple as this:
 *
 * enum device_attr
 * {
 * 	ATTR_EUS_PER_SSLICE = 0,
 * 	ATTR_SOME_MASK 	  = 1,
 * };
 *
 * static const u32 hwconfig[] =
 * {
 * 	 ATTR_EUS_PER_SSLICE,
 * 	 1,		// Value Length in DWords
 * 	 8,		// Value
 *
 * 	 ATTR_SOME_MASK,
 * 	 3,
 * 	 0x00FFFFFFFF, 0xFFFFFFFF, 0xFF000000, // Value
 * };
 * static const u32 table_size = sizeof(hwconfig) / sizeof(hwconfig[0]));
 *
 * It is important to highlight though that the device attributes ids are common
 * across multiple components including, GuC, i915 and user space components.
 * The definition of the actual and current attributes can be found in
 * the header file: intel_hwconfig_types.h
 */

static int __guc_action_get_hwconfig(struct intel_guc_hwconfig *hwconfig,
				    u32 ggtt_offset, u32 ggtt_size)
{
	struct intel_guc *guc = hwconfig_to_guc(hwconfig);
	u32 action[] = {
		INTEL_GUC_ACTION_GET_HWCONFIG,
		ggtt_offset,
		0, /* upper 32 bits of address */
		ggtt_size,
	};
	int ret;

	ret = intel_guc_send_mmio(guc, action, ARRAY_SIZE(action), NULL, 0);
	if (ret == -ENXIO)
		return -ENOENT;

	if (!ggtt_size && !ret)
		ret = -EINVAL;

	return ret;
}

static int guc_hwconfig_discover_size(struct intel_guc_hwconfig *hwconfig)
{
	int ret;

	/* Sending a query with too small a table will return the size of the table */
	ret = __guc_action_get_hwconfig(hwconfig, 0, 0);
	if (ret < 0)
		return ret;

	hwconfig->size = ret;
	return 0;
}

static int guc_hwconfig_fill_buffer(struct intel_guc_hwconfig *hwconfig)
{
	struct intel_guc *guc = hwconfig_to_guc(hwconfig);
	u32 ggtt_offset;
	int ret;
	struct i915_vma *vma;
	void *vaddr;

	GEM_BUG_ON(!hwconfig->size);

	ret = intel_guc_allocate_and_map_vma(guc, hwconfig->size, &vma, &vaddr);
	if (ret)
		return ret;

	ggtt_offset = intel_guc_ggtt_offset(guc, vma);

	ret = __guc_action_get_hwconfig(hwconfig, ggtt_offset, hwconfig->size);
	if (ret >= 0)
		memcpy(hwconfig->ptr, vaddr, hwconfig->size);

	i915_vma_unpin_and_release(&vma, I915_VMA_RELEASE_MAP);

	return ret;
}

static const u32 *fake_hwconfig_get_table(struct drm_i915_private *i915,
					  u32 *size)
{
	if (IS_XEHPSDV(i915)) {
		*size = ARRAY_SIZE(hwinfo_xehpsdv) * sizeof(u32);
		return hwinfo_xehpsdv;
	}

	return NULL;
}

static int fake_hwconfig_discover_size(struct intel_guc_hwconfig *hwconfig)
{
	struct intel_guc *guc = hwconfig_to_guc(hwconfig);
	struct drm_i915_private *i915 = guc_to_gt(guc)->i915;
	const u32 *table;
	u32 table_size;

	table = fake_hwconfig_get_table(i915, &table_size);
	if (!table)
		return -ENOENT;

	hwconfig->size = table_size;
	return 0;
}

static int fake_hwconfig_fill_buffer(struct intel_guc_hwconfig *hwconfig)
{
	struct intel_guc *guc = hwconfig_to_guc(hwconfig);
	struct drm_i915_private *i915 = guc_to_gt(guc)->i915;
	const u32 *table;
	u32 table_size;

	table = fake_hwconfig_get_table(i915, &table_size);
	if (!table)
		return -ENOENT;

	if (hwconfig->size >= table_size)
		memcpy(hwconfig->ptr, table, table_size);

	return table_size;
}

static int intel_hwconf_override_klv(struct intel_guc_hwconfig *hwconfig, u32 new_key, u32 new_len, u32 *new_value)
{
	u32 *old_array, *new_array, *new_ptr;
	u32 old_size, new_size;
	u32 i;

	if (new_key > INTEL_HWCONFIG_MAX)
		return -EINVAL;

	old_array = (u32*)(hwconfig->ptr);
	old_size = hwconfig->size / sizeof(u32);
	new_size = old_size + 2 + new_len;
	new_array = new_ptr = kmalloc_array(new_size, sizeof(u32), GFP_KERNEL);
	if (!new_array)
		return -ENOMEM;

	i = 0;
	while (i < old_size) {
		u32 key = old_array[i];
		u32 len = old_array[i + 1];
		u32 next = i + 2 + len;

		if ((key >= __INTEL_HWCONFIG_MAX) || (next > old_size)) {
			struct intel_guc *guc = hwconfig_to_guc(hwconfig);
			struct drm_i915_private *i915 = guc_to_gt(guc)->i915;
			drm_err(&i915->drm, "HWConfig: corrupted table at %d/%d: 0x%X [0x%X] x 0x%X!\n",
				i, old_size, key, __INTEL_HWCONFIG_MAX, len);
			return -EINVAL;
		}

		if (old_array[i] == new_key)
			break;

		i = next;
	}

	if (i) {
		memcpy(new_array, old_array, i * sizeof(u32));
		new_ptr += i;
	}

	*(new_ptr++) = new_key;
	*(new_ptr++) = new_len;
	memcpy(new_ptr, new_value, new_len * sizeof(u32));
	new_ptr += new_len;

	if (i < old_size) {
		memcpy(new_ptr, old_array + i, (old_size - i) * sizeof(u32));
		new_ptr += old_size - i;
	}

	hwconfig->ptr = new_array;
	hwconfig->size = (new_ptr - new_array) * sizeof(u32);
	kfree(old_array);
	return 0;
}

static int fused_l3_banks(struct drm_i915_private *i915)
{
	u32 meml3, fused_banks, fused_base;
	bool rambo;

	meml3 = intel_uncore_read(&i915->uncore, GEN10_MIRROR_FUSE3);
	fused_banks = hweight32(meml3 & GEN12_MEML3_EN_MASK) * 12;
	rambo = meml3 & XEHPC_L3_MODE_FUSE_RAMBO;
	fused_base = hweight32(meml3 & XEHPC_L3_MODE_FUSE_BASE_MASK);

	switch (fused_banks) {
	case 12:
		if (fused_base == 2)
			fused_banks = 8;
		break;
	case 24:
		if (rambo)
			fused_banks = 32;
		break;
	case 48:
		if (rambo)
			fused_banks = 64;
		break;
	}

	return fused_banks;
}

static int sanitize_l3_size(struct drm_i915_private *i915)
{
	struct intel_gt *gt = to_gt(i915);
	struct intel_guc_hwconfig *hwconfig = &gt->uc.guc.hwconfig;
	u32 new_size, orig_size;
	u32 spec_banks, fused_banks;

	if (i915->params.l3_size_override == 0)
		return 0;

	orig_size = intel_guc_hwconfig_get_value(hwconfig,
					INTEL_HWCONFIG_DEPRECATED_L3_CACHE_SIZE_IN_KB);

	if (i915->params.l3_size_override > 0) {
		new_size = i915->params.l3_size_override;
		if (new_size > orig_size) {
			drm_err(&i915->drm, "Invalid i915.l3_size_override. Value should never exceed the original spec size of %d\n", orig_size);
			return -EINVAL;
		}
		drm_info(&i915->drm, "Overriding L3_size. Original:%d New:%d\n", orig_size, new_size);
	} else {
		spec_banks = intel_guc_hwconfig_get_value(hwconfig,
						INTEL_HWCONFIG_DEPRECATED_L3_BANK_COUNT);
		fused_banks = fused_l3_banks(i915);

		if (fused_banks < spec_banks) {
			new_size = fused_banks * orig_size / spec_banks;
			drm_info(&i915->drm, "Fused-off banks found: Limiting L3 size to %d\n", new_size);
		}
	}

	return intel_hwconf_override_klv(hwconfig, INTEL_HWCONFIG_DEPRECATED_L3_CACHE_SIZE_IN_KB, 1, &new_size);
}

static int intel_hwconf_apply_overrides(struct intel_guc_hwconfig *hwconfig)
{
	struct intel_guc *guc = hwconfig_to_guc(hwconfig);
	struct intel_gt *gt = guc_to_gt(guc);
	int count;

	/* For A0 validation only: 22011497615 */
	if (IS_PVC_BD_STEP(gt->i915, STEP_A0, STEP_B0)) {
		int ret = sanitize_l3_size(gt->i915);
		if (ret)
			return ret;
	}

	count = intel_gt_get_l3bank_count(gt);
	if (count < 0)
		return 0;

	return intel_hwconf_override_klv(hwconfig,
					 INTEL_HWCONFIG_DEPRECATED_L3_BANK_COUNT,
					 1, &count);
}

/**
 * intel_guc_hwconfig_get_value - Get single value for a given key
 * @key: KLV's key for the attribute
 *
 * Parse our KLV table returning the single value for a given key.
 * This function is intended to return only 1 dword-sized value.
 * If used with a key where len >= 2, only the first value will be
 * returned.
 * Attributes with multiple entries are not yet needed by i915.
 */
u32 intel_guc_hwconfig_get_value(struct intel_guc_hwconfig *hwconfig, u32 key)
{
	int i, len;
	u32 *array = (u32*)(hwconfig->ptr);

	if (key > INTEL_HWCONFIG_MAX)
		return -EINVAL;

	for (i = 0; i < hwconfig->size / sizeof(u32); i += 2 + len) {
		if (array[i] == key)
			return array[i + 2];
		len = array[i + 1];
	}

	return -ENOENT;
}

static bool has_table(struct drm_i915_private *i915)
{
	if (IS_ADLP_GRAPHICS_STEP(i915, STEP_B0, STEP_FOREVER))
		return 1;
	if (IS_DG2_G11(i915) || IS_DG2_GRAPHICS_STEP(i915, G10, STEP_A2, STEP_FOREVER))
		return 1;
	if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 60))
		return 1;

	return 0;
}

static bool has_fake_table(struct drm_i915_private *i915)
{
	u32 size;

	return fake_hwconfig_get_table(i915, &size) != NULL;
}

/**
 * intel_guc_hwconfig_init - Initialize the HWConfig
 *
 * Allocates and pin a GGTT buffer to be filled with the HWConfig table.
 * This buffer will be ready to be queried as needed at any time.
 */
int intel_guc_hwconfig_init(struct intel_guc_hwconfig *hwconfig)
{
	struct intel_guc *guc = hwconfig_to_guc(hwconfig);
	struct drm_i915_private *i915 = guc_to_gt(guc)->i915;
	bool fake_db = false;
	int ret;

	if (hwconfig->size)
		return 0;

	if (!has_table(i915) && !has_fake_table(i915))
		return 0;

	if (!has_table(i915)) {
		fake_db = true;
		ret = fake_hwconfig_discover_size(hwconfig);
	} else {
		ret = guc_hwconfig_discover_size(hwconfig);
	}
	if (ret)
		return ret;

	hwconfig->ptr = kmalloc(hwconfig->size, GFP_KERNEL);
	if (!hwconfig->ptr) {
		hwconfig->size = 0;
		return -ENOMEM;
	}

	if (fake_db)
		ret = fake_hwconfig_fill_buffer(hwconfig);
	else
		ret = guc_hwconfig_fill_buffer(hwconfig);
	if (ret < 0)
		goto err;

	ret = intel_hwconf_apply_overrides(hwconfig);
	if (!ret)
		return 0;

err:
	kfree(hwconfig->ptr);
	hwconfig->size = 0;
	hwconfig->ptr = NULL;
	return ret;
}

/**
 * intel_guc_hwconfig_fini - Finalize the HWConfig
 *
 * This unpin and release the GGTT buffer containing the HWConfig table.
 * The table needs to be cached and available during the runtime, so
 * this function should only be called only when disabling guc.
 */
void intel_guc_hwconfig_fini(struct intel_guc_hwconfig *hwconfig)
{
	kfree(hwconfig->ptr);
	hwconfig->size = 0;
	hwconfig->ptr = NULL;
}
