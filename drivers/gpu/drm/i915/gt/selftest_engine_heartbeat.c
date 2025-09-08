// SPDX-License-Identifier: MIT
/*
 * Copyright © 2018 Intel Corporation
 */

#include <linux/sort.h>

#include "i915_drv.h"

#include "intel_gt_requests.h"
#include "i915_selftest.h"
#include "selftest_engine_heartbeat.h"

static void intel_engine_park_heartbeat_sync(struct intel_engine_cs *engine)
{
	cancel_delayed_work_sync(&engine->heartbeat.work);
	i915_request_put(fetch_and_zero(&engine->heartbeat.systole));
}

int intel_heartbeat_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
	};
	int saved_hangcheck;
	int err;

	if (intel_gt_is_wedged(to_gt(i915)))
		return 0;

	saved_hangcheck = i915->params.enable_hangcheck;
	i915->params.enable_hangcheck = INT_MAX;

	err = intel_gt_live_subtests(tests, to_gt(i915));

	i915->params.enable_hangcheck = saved_hangcheck;
	return err;
}

void st_engine_heartbeat_disable(struct intel_engine_cs *engine)
{
	engine->props.heartbeat_interval_ms = 0;

	intel_engine_pm_get(engine);
	intel_engine_park_heartbeat_sync(engine);
}

void st_engine_heartbeat_enable(struct intel_engine_cs *engine)
{
	intel_engine_pm_put(engine);

	engine->props.heartbeat_interval_ms =
		engine->defaults.heartbeat_interval_ms;
}

void st_engine_heartbeat_disable_no_pm(struct intel_engine_cs *engine)
{
	engine->props.heartbeat_interval_ms = 0;

	/*
	 * Park the heartbeat but without holding the PM lock as that
	 * makes the engines appear not-idle. Note that if/when unpark
	 * is called due to the PM lock being acquired later the
	 * heartbeat still won't be enabled because of the above = 0.
	 */
	if (intel_engine_pm_get_if_awake(engine)) {
		intel_engine_park_heartbeat_sync(engine);
		intel_engine_pm_put(engine);
	}
}

void st_engine_heartbeat_enable_no_pm(struct intel_engine_cs *engine)
{
	engine->props.heartbeat_interval_ms =
		engine->defaults.heartbeat_interval_ms;
}
