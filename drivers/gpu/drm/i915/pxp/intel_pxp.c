// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2020 Intel Corporation.
 */
#include <linux/workqueue.h>
#include "intel_pxp.h"
#include "intel_pxp_irq.h"
#include "intel_pxp_session.h"
#include "intel_pxp_tee.h"
#include "gt/intel_context.h"
#include "i915_drv.h"

/**
 * DOC: PXP
 *
 * PXP (Protected Xe Path) is a feature available in Gen12 and newer platforms.
 * It allows execution and flip to display of protected (i.e. encrypted)
 * objects. The SW support is enabled via the CPTCFG_DRM_I915_PXP kconfig.
 *
 * Objects can opt-in to PXP encryption at creation time via the
 * I915_GEM_CREATE_EXT_PROTECTED_CONTENT create_ext flag. For objects to be
 * correctly protected they must be used in conjunction with a context created
 * with the I915_CONTEXT_PARAM_PROTECTED_CONTENT flag. See the documentation
 * of those two uapi flags for details and restrictions.
 *
 * Protected objects are tied to a pxp session; currently we only support one
 * session, which i915 manages and whose index is available in the uapi
 * (I915_PROTECTED_CONTENT_DEFAULT_SESSION) for use in instructions targeting
 * protected objects.
 * The session is invalidated by the HW when certain events occur (e.g.
 * suspend/resume). When this happens, all the objects that were used with the
 * session are marked as invalid and all contexts marked as using protected
 * content are banned. Any further attempt at using them in an execbuf call is
 * rejected, while flips are converted to black frames.
 *
 * Some of the PXP setup operations are performed by the Management Engine,
 * which is handled by the mei driver; communication between i915 and mei is
 * performed via the mei_pxp component module.
 */

struct intel_gt *pxp_to_gt(const struct intel_pxp *pxp)
{
	return container_of(pxp, struct intel_gt, pxp);
}

bool intel_pxp_is_active(const struct intel_pxp *pxp)
{
	return pxp->arb_is_valid;
}

/* KCR register definitions */
#define KCR_INIT _MMIO(0x320f0)
/* Setting KCR Init bit is required after system boot */
#define KCR_INIT_ALLOW_DISPLAY_ME_WRITES REG_BIT(14)

static void kcr_pxp_enable(struct intel_gt *gt)
{
	intel_uncore_write(gt->uncore, KCR_INIT,
			   _MASKED_BIT_ENABLE(KCR_INIT_ALLOW_DISPLAY_ME_WRITES));
}

static void kcr_pxp_disable(struct intel_gt *gt)
{
	intel_uncore_write(gt->uncore, KCR_INIT,
			   _MASKED_BIT_DISABLE(KCR_INIT_ALLOW_DISPLAY_ME_WRITES));
}

static int create_vcs_context(struct intel_pxp *pxp)
{
	static struct lock_class_key pxp_lock;
	struct intel_gt *gt = pxp_to_gt(pxp);
	struct intel_engine_cs *engine;
	struct intel_context *ce;
	int i;

	/*
	 * Find the first VCS engine present. We're guaranteed there is one
	 * if we're in this function due to the check in has_pxp
	 */
	for (i = 0, engine = NULL; !engine; i++)
		engine = gt->engine_class[VIDEO_DECODE_CLASS][i];

	GEM_BUG_ON(!engine || engine->class != VIDEO_DECODE_CLASS);

	ce = intel_engine_create_pinned_context(engine, engine->gt->vm, SZ_4K,
						I915_GEM_HWS_PXP_ADDR,
						&pxp_lock, "pxp_context");
	if (IS_ERR(ce)) {
		drm_err(&gt->i915->drm, "failed to create VCS ctx for PXP\n");
		return PTR_ERR(ce);
	}

	pxp->ce = ce;

	return 0;
}

static void destroy_vcs_context(struct intel_pxp *pxp)
{
	if (pxp->ce)
		intel_engine_destroy_pinned_context(fetch_and_zero(&pxp->ce));
}

static void pxp_init_full(struct intel_pxp *pxp)
{
	struct intel_gt *gt = pxp_to_gt(pxp);
	int ret;

	/*
	 * we'll use the completion to check if there is a termination pending,
	 * so we start it as completed and we reinit it when a termination
	 * is triggered.
	 */
	init_completion(&pxp->termination);
	complete_all(&pxp->termination);

	intel_pxp_session_management_init(pxp);

	ret = create_vcs_context(pxp);
	if (ret)
		return;

	ret = intel_pxp_tee_component_init(pxp);
	if (ret)
		goto out_context;

	drm_info(&gt->i915->drm, "Protected Xe Path (PXP) protected content support initialized\n");

	return;

out_context:
	destroy_vcs_context(pxp);
}

void intel_pxp_init(struct intel_pxp *pxp)
{
	struct intel_gt *gt = pxp_to_gt(pxp);

	/*DKMS I915 assumes CONFIG_INTEL_MEI_PXP is enabled always*/

	/*
	 * If HuC is loaded by GSC but PXP is disabled, we can skip the init of
	 * the full PXP session/object management and just init the tee channel.
	 */
	if (HAS_PXP(gt->i915))
		pxp_init_full(pxp);
	else if (intel_huc_is_loaded_by_gsc(&gt->uc.huc) && intel_uc_uses_huc(&gt->uc))
		intel_pxp_tee_component_init(pxp);
}

void intel_pxp_fini(struct intel_pxp *pxp)
{
	pxp->arb_is_valid = false;

	intel_pxp_tee_component_fini(pxp);

	destroy_vcs_context(pxp);
}

void intel_pxp_mark_termination_in_progress(struct intel_pxp *pxp)
{
	pxp->arb_is_valid = false;
	reinit_completion(&pxp->termination);
}

static void intel_pxp_queue_termination(struct intel_pxp *pxp)
{
	struct intel_gt *gt = pxp_to_gt(pxp);

	/*
	 * We want to get the same effect as if we received a termination
	 * interrupt, so just pretend that we did.
	 */
	spin_lock_irq(gt->irq_lock);
	intel_pxp_mark_termination_in_progress(pxp);
	pxp->session_events |= PXP_TERMINATION_REQUEST;
	queue_work(system_unbound_wq, &pxp->session_work);
	spin_unlock_irq(gt->irq_lock);
}

/*
 * the arb session is restarted from the irq work when we receive the
 * termination completion interrupt
 */
int intel_pxp_wait_for_arb_start(struct intel_pxp *pxp)
{
	if (!intel_pxp_is_enabled(pxp))
		return 0;

	if (!wait_for_completion_timeout(&pxp->termination,
					 msecs_to_jiffies(100)))
		return -ETIMEDOUT;

	if (!pxp->arb_is_valid)
		return -EIO;

	return 0;
}

void intel_pxp_init_hw(struct intel_pxp *pxp)
{
	kcr_pxp_enable(pxp_to_gt(pxp));
	intel_pxp_irq_enable(pxp);

	/*
	 * the session could've been attacked while we weren't loaded, so
	 * handle it as if it was and re-create it.
	 */
	intel_pxp_queue_termination(pxp);
}

void intel_pxp_fini_hw(struct intel_pxp *pxp)
{
	kcr_pxp_disable(pxp_to_gt(pxp));

	intel_pxp_irq_disable(pxp);
}
