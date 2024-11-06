/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef __I915_SRIOV_TYPES_H__
#define __I915_SRIOV_TYPES_H__

#include <linux/types.h>
#include "i915_sriov_sysfs_types.h"

/**
 * struct i915_sriov_telemetry_data - telemetry data of particular VF.
 * @lmem_alloc_size: lmem size that has been allocated by VF.
 */
struct i915_sriov_telemetry_data {
	u64 lmem_alloc_size;
};

/**
 * struct i915_sriov_telemetry_pf - PF telemetry data.
 * @data: pointer to array containing telemetry data of all VFs.
 */
struct i915_sriov_telemetry_pf {
	struct i915_sriov_telemetry_data *data;
};

/**
 * struct i915_sriov_pf - i915 SR-IOV PF data.
 * @__status: Status of the PF. Don't access directly!
 * @device_vfs: Number of VFs supported by the device.
 * @driver_vfs: Number of VFs supported by the driver.
 * @sysfs.home: Home object for all entries in sysfs.
 * @sysfs.kobjs: Array with PF and VFs objects exposed in sysfs.
 */
struct i915_sriov_pf {
	int __status;
	u16 device_vfs;
	u16 driver_vfs;
	struct {
		struct i915_sriov_kobj *home;
		struct i915_sriov_ext_kobj **kobjs;
	} sysfs;

	/** @disable_auto_provisioning: flag to control VFs auto-provisioning */
	bool disable_auto_provisioning;

	/** @telemetry: PF telemetry data */
	struct i915_sriov_telemetry_pf telemetry;

	/** @smem_buffers: list of allocated SMEM buffers */
	struct list_head smem_buffers;
};

/**
 * struct i915_sriov_telemetry_vf - VF telemetry data.
 * @rate: telemetry rate.
 * @worker: worker for sending telemetry data.
 * @timer: timer for sending telemetry data periodically.
 */
struct i915_sriov_telemetry_vf {
	u32 rate;
	struct work_struct worker;
	struct timer_list timer;
	struct cached_data {
		u64 lmem_total_size;
	} cached;
};

/**
 * struct i915_sriov_vf - i915 SR-IOV VF data.
 */
struct i915_sriov_vf {

	/** @migration_worker: migration recovery worker */
	struct work_struct migration_worker;
	unsigned long migration_gt_flags;

	/** @telemetry: VF telemetry data */
	struct i915_sriov_telemetry_vf telemetry;
};

/**
 * struct i915_sriov - i915 SR-IOV data.
 * @pf: PF only data.
 */
struct i915_sriov {
	union {
		struct i915_sriov_pf pf;
		struct i915_sriov_vf vf;
	};
};

#endif /* __I915_SRIOV_TYPES_H__ */
