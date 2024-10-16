/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef __INTEL_GT_PRINT__
#define __INTEL_GT_PRINT__

#include <drm/drm_print.h>
#include "intel_gt_types.h"
#include "i915_utils.h"

#define __gt_dev__(_gt) ((_gt)->i915->drm.dev)

#define gt_err(_gt, _fmt, ...) \
	dev_err(__gt_dev__(_gt), "GT%u: " _fmt, (_gt)->info.id, ##__VA_ARGS__)

#define gt_err_once(_gt, _fmt, ...) \
	dev_err_once(__gt_dev__(_gt), "GT%u: " _fmt, (_gt)->info.id, ##__VA_ARGS__)

#define gt_warn(_gt, _fmt, ...) \
	dev_warn(__gt_dev__(_gt), "GT%u: " _fmt, (_gt)->info.id, ##__VA_ARGS__)

#define gt_notice(_gt, _fmt, ...) \
	dev_notice(__gt_dev__(_gt), "GT%u: " _fmt, (_gt)->info.id, ##__VA_ARGS__)

#define gt_info(_gt, _fmt, ...) \
	dev_info(__gt_dev__(_gt), "GT%u: " _fmt, (_gt)->info.id, ##__VA_ARGS__)

#define gt_info_once(_gt, _fmt, ...) \
	dev_info_once(__gt_dev__(_gt), "GT%u: " _fmt, (_gt)->info.id, ##__VA_ARGS__)

#define gt_dbg(_gt, _fmt, ...) \
	dev_dbg(__gt_dev__(_gt), "GT%u: " _fmt, (_gt)->info.id, ##__VA_ARGS__)

#define gt_err_ratelimited(_gt, _fmt, ...) \
	dev_err_ratelimited(__gt_dev__(_gt), "GT%u: " _fmt, (_gt)->info.id, ##__VA_ARGS__)

#define gt_notice_ratelimited(_gt, _fmt, ...) \
	dev_notice_ratelimited(__gt_dev__(_gt), "GT%u: " _fmt, (_gt)->info.id, ##__VA_ARGS__)

#define gt_probe_error(_gt, _fmt, ...) \
	do { \
		if (i915_error_injected()) \
			gt_dbg(_gt, _fmt, ##__VA_ARGS__); \
		else \
			gt_err(_gt, _fmt, ##__VA_ARGS__); \
	} while (0)

#define gt_WARN(_gt, _condition, _fmt, ...) \
	WARN(_condition, "%s %s: GT%u:" _fmt, dev_driver_string(__gt_dev__(_gt)), dev_name(__gt_dev__(_gt)), (_gt)->info.id, ##__VA_ARGS__)

#define gt_WARN_ONCE(_gt, _condition, _fmt, ...) \
	WARN_ONCE(_condition, "%s %s: GT%u:" _fmt, dev_driver_string(__gt_dev__(_gt)), dev_name(__gt_dev__(_gt)), (_gt)->info.id, ##__VA_ARGS__)

#define gt_WARN_ON(_gt, _condition) \
	gt_WARN(_gt, _condition, "%s", "WARN_ON(" __stringify(_condition) ")")

#define gt_WARN_ON_ONCE(_gt, _condition) \
	gt_WARN_ONCE(_gt, _condition, "%s", "WARN_ON(" __stringify(_condition) ")")

#endif /* __INTEL_GT_PRINT_H__ */
