// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "gt/intel_gt.h"
#include "gt/intel_hwconfig.h"
#include "gt/intel_hwconfig_types.h"
#include "i915_drv.h"
#include "i915_memcpy.h"

/* Auto-generated tables: */
#include "intel_guc_hwconfig_auto.c"

/*
 * GuC has a blob containing hardware configuration information (HWConfig).
 * This is formatted as a simple and flexible KLV (Key/Length/Value) table.
 *
 * For example, a minimal version could be:
 *   enum device_attr {
 *     ATTR_SOME_VALUE = 0,
 *     ATTR_SOME_MASK  = 1,
 *   };
 *
 *   static const u32 hwconfig[] = {
 *     ATTR_SOME_VALUE,
 *     1,		// Value Length in DWords
 *     8,		// Value
 *
 *     ATTR_SOME_MASK,
 *     3,
 *     0x00FFFFFFFF, 0xFFFFFFFF, 0xFF000000,
 *   };
 *
 * The attribute ids are defined in a hardware spec.
 */

static int __guc_action_get_hwconfig(struct intel_guc *guc,
				     u32 ggtt_offset, u32 ggtt_size)
{
	u32 action[] = {
		INTEL_GUC_ACTION_GET_HWCONFIG,
		lower_32_bits(ggtt_offset),
		upper_32_bits(ggtt_offset),
		ggtt_size,
	};
	int ret;

	ret = intel_guc_send_mmio(guc, action, ARRAY_SIZE(action), NULL, 0);
	if (ret == -ENXIO)
		return -ENOENT;

	return ret;
}

static int guc_hwconfig_discover_size(struct intel_guc *guc)
{
	int ret;

	/*
	 * Sending a query with zero offset and size will return the
	 * size of the blob.
	 */
	ret = __guc_action_get_hwconfig(guc, 0, 0);
	if (ret == 0)
		return -EINVAL;

	return ret;
}

static int guc_hwconfig_fill_buffer(struct intel_guc *guc, void *ptr, int size)
{
	struct i915_vma *vma;
	u32 ggtt_offset;
	void *vaddr;
	int ret;

	ret = __intel_guc_allocate_and_map_vma(guc, size, true, &vma, &vaddr);
	if (ret)
		return ret;

	ggtt_offset = intel_guc_ggtt_offset(guc, vma);
	ret = __guc_action_get_hwconfig(guc, ggtt_offset, size);
	if (ret >= 0)
		memcpy(ptr, vaddr, size);

	i915_vma_unpin_and_release(&vma, I915_VMA_RELEASE_MAP);
	return ret;
}

/*
static inline struct intel_gt *hwconfig_to_gt(struct intel_hwconfig *hwconfig)
{
	return container_of(hwconfig, struct intel_gt, info.hwconfig);
}

static int intel_hwconf_override_klv(struct intel_hwconfig *hwconfig, u32 new_key, u32 new_len, u32 *new_value)
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
			struct intel_gt *gt = hwconfig_to_gt(hwconfig);
			drm_err(&gt->i915->drm, "HWConfig: corrupted table at %d/%d: 0x%X [0x%X] x 0x%X!\n",
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
*/

static const u32 *fake_hwconfig_get_table(struct drm_i915_private *i915,
					  u32 *size)
{
#ifndef BPM_ADLP_A0_PART_DISABLE
	if (IS_ALDERLAKE_P(i915)) {
		*size = ARRAY_SIZE(hwinfo_adlp) * sizeof(u32);
		return hwinfo_adlp;
	}
#endif

	return NULL;
}

static int fake_hwconfig_discover_size(struct intel_guc *guc)
{
	const u32 *table;
	u32 table_size;

	table = fake_hwconfig_get_table(guc_to_gt(guc)->i915, &table_size);
	if (!table)
		return -ENOENT;

	return table_size;
}

static int fake_hwconfig_fill_buffer(struct intel_guc *guc, void *ptr, int size)
{
	const u32 *table;
	u32 table_size;

	table = fake_hwconfig_get_table(guc_to_gt(guc)->i915, &table_size);
	if (!table)
		return -ENOENT;

	if (size >= table_size)
		memcpy(ptr, table, table_size);

	return table_size;
}

static bool has_table(struct drm_i915_private *i915)
{
	if (IS_ALDERLAKE_P(i915) && !IS_ADLP_N(i915))
		return true;
	if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 55))
		return true;

	return false;
}

static bool has_fake_table(struct drm_i915_private *i915)
{
	u32 size;

	return fake_hwconfig_get_table(i915, &size) != NULL;
}

/*
 * intel_guc_hwconfig_init - Initialize the HWConfig
 *
 * Retrieve the HWConfig table from the GuC and save it locally.
 * It can then be queried on demand by other users later on.
 */
static int guc_hwconfig_init(struct intel_gt *gt)
{
	struct intel_hwconfig *hwconfig = &gt->info.hwconfig;
	struct intel_guc *guc = &gt->uc.guc;
	intel_wakeref_t wf;
	int ret = 0;

	if (READ_ONCE(hwconfig->size))
		return 0;

	if (!has_table(gt->i915) && !has_fake_table(gt->i915))
		return -ENODEV;

	wf = intel_runtime_pm_get(gt->uncore->rpm);
	mutex_lock(&hwconfig->mutex);
	if (!hwconfig->size) {
		bool fake_db = !has_table(gt->i915);
		void *ptr;

		if (fake_db)
			ret = fake_hwconfig_discover_size(guc);
		else
			ret = guc_hwconfig_discover_size(guc);
		if (unlikely(ret < 0))
			goto unlock;

		ptr = kmalloc(ret, GFP_KERNEL);
		if (unlikely(!ptr)) {
			ret = -ENOMEM;
			goto unlock;
		}

		if (fake_db)
			ret = fake_hwconfig_fill_buffer(guc, ptr, ret);
		else
			ret = guc_hwconfig_fill_buffer(guc, ptr, ret);
		if (unlikely(ret < 0)) {
			kfree(ptr);
			goto unlock;
		}

		hwconfig->ptr = ptr;
		hwconfig->size = ret;
		ret = 0;
	}
unlock:
	mutex_unlock(&hwconfig->mutex);
	intel_runtime_pm_put(gt->uncore->rpm, wf);
	return ret;
}

/*
 * intel_gt_init_hwconfig - Initialize the HWConfig if available
 *
 * Retrieve the HWConfig table if available on the current platform.
 */
int intel_gt_init_hwconfig(struct intel_gt *gt)
{
	if (!intel_uc_uses_guc(&gt->uc))
		return -ENODEV;

	return guc_hwconfig_init(gt);
}

/*
 * intel_gt_fini_hwconfig - Finalize the HWConfig
 *
 * Free up the memory allocation holding the table.
 */
void intel_gt_fini_hwconfig(struct intel_gt *gt)
{
	struct intel_hwconfig *hwconfig = &gt->info.hwconfig;

	kfree(hwconfig->ptr);
	hwconfig->ptr = NULL;
	hwconfig->size = 0;

	mutex_destroy(&hwconfig->mutex);
}
