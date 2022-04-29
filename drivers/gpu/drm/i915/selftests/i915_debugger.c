// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include "gt/intel_gt.h"
#include "gt/intel_ring.h"
#include "gt/intel_gpu_commands.h"

#include "i915_selftest.h"

static int emit_srm(struct i915_request *rq, i915_reg_t reg, u32 *out)
{
	u32 *cs;

	cs = intel_ring_begin(rq, 4);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	*cs++ = MI_STORE_REGISTER_MEM_GEN8 | MI_USE_GGTT;
	*cs++ = i915_mmio_reg_offset(reg);
	*cs++ = i915_ggtt_offset(rq->engine->status_page.vma) +
		offset_in_page(out);
	*cs++ = 0;

	intel_ring_advance(rq, cs);

	return 0;
}

static int dg2_workarounds(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct intel_engine_cs *engine;

	/*
	 * For exceptions and attention notification to work, we have
	 * to ensure various bits are configured globally and in each
	 * context. While these should be checked on application by
	 * the workaround handlers, we want an explicit checklist
	 * of known eudbg workarounds.
	 */

	if (!IS_DG2(i915))
		return 0;

	for_each_uabi_engine(engine, i915) {
		u32 *result = memset32(engine->status_page.addr + 4000, 0, 96);
		struct intel_context *ce;
		struct i915_request *rq;
		u32 td_ctl;
		int err;

		if (engine->class != RENDER_CLASS &&
		    engine->class != COMPUTE_CLASS)
			continue;

		ce = intel_context_create(engine);
		if (IS_ERR(ce))
			return PTR_ERR(ce);

		rq = intel_context_create_request(ce);
		intel_context_put(ce);
		if (IS_ERR(rq))
			return PTR_ERR(rq);

		err = emit_srm(rq, TD_CTL, result);

		i915_request_get(rq);
		i915_request_add(rq);
		if (err == 0 && i915_request_wait(rq, 0, HZ) < 0)
			err = -ETIME;
		i915_request_put(rq);
		if (err)
			return err;

		td_ctl = READ_ONCE(*result);
		pr_info("%s TD_CTL: %08x\n", engine->name, td_ctl);
		if (!(td_ctl & TD_CTL_FORCE_THREAD_BREAKPOINT_ENABLE)) { /* vlk-29551 */
			pr_err("%s TD_CTL does not have FORCE_THREAD_BREAKPOINT_ENABLE set\n",
			       engine->name);
			err = -EINVAL;
		}
		if (!(td_ctl & TD_CTL_FEH_AND_FEE_ENABLE)) { /* vlk-29182 */
			pr_err("%s TD_CTL does not have FEH_AND_FEE_ENABLE set\n",
			       engine->name);
			err = -EINVAL;
		}
		if (err)
			return err;
	}

	return 0;
}

int i915_debugger_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(dg2_workarounds),
	};

	if (!i915_modparams.debug_eu)
		return 0;

	if (intel_gt_is_wedged(to_gt(i915)))
		return 0;

	return i915_subtests(tests, i915);
}
