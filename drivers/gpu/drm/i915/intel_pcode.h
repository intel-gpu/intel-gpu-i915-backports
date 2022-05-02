/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2013-2021 Intel Corporation
 */

#ifndef _INTEL_PCODE_H_
#define _INTEL_PCODE_H_

#include <linux/types.h>

struct intel_gt;
struct drm_i915_private;

int intel_gt_pcode_read(struct intel_gt *gt, u32 mbox, u32 *val, u32 *val1);

int intel_gt_pcode_write_timeout(struct intel_gt *gt, u32 mbox, u32 val,
				 int fast_timeout_us, int slow_timeout_ms);

#define intel_gt_pcode_write(gt, mbox, val) \
	intel_gt_pcode_write_timeout(gt, mbox, val, 500, 0)

int intel_gt_pcode_request(struct intel_gt *gt, u32 mbox, u32 request,
			   u32 reply_mask, u32 reply, int timeout_base_ms);

#define snb_pcode_read(i915, mbox, val, val1) \
	intel_gt_pcode_read(&i915->gt, mbox, val, val1)

#define snb_pcode_write_timeout(i915, mbox, val, fast_timeout_us, slow_timeout_ms) \
	intel_gt_pcode_write_timeout(&i915->gt, mbox, val, fast_timeout_us, slow_timeout_ms)

#define snb_pcode_write(i915, mbox, val) \
	snb_pcode_write_timeout(i915, mbox, val, 500, 0)

#define skl_pcode_request(i915, mbox, request, reply_mask, reply, timeout_base_ms) \
	intel_gt_pcode_request(&i915->gt, mbox, request, reply_mask, reply, timeout_base_ms)

int intel_pcode_init(struct drm_i915_private *i915);

/*
 * Helpers for dGfx PCODE mailbox command formatting
*/
int __intel_gt_pcode_read(struct intel_gt *gt, u32 mbcmd, u32 p1, u32 p2, u32 *val);
int __intel_gt_pcode_write(struct intel_gt *gt, u32 mbcmd, u32 p1, u32 p2, u32 val);

#define __snb_pcode_read(i915, mbcmd, p1, p2, val) \
	__intel_gt_pcode_read(&i915->gt, mbcmd, p1, p2, val)

#define __snb_pcode_write(i915, mbcmd, p1, p2, val) \
	__intel_gt_pcode_write(&i915->gt, mbcmd, p1, p2, val)

#endif /* _INTEL_PCODE_H */
