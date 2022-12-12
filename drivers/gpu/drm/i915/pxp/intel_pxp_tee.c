// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2020 Intel Corporation.
 */

#include <linux/component.h>

#include <drm/i915_pxp_tee_interface.h>
#include <drm/i915_component.h>

#include "gem/i915_gem_region.h"
#include "i915_drv.h"
#include "intel_pxp.h"
#include "intel_pxp_session.h"
#include "intel_pxp_tee.h"
#include "intel_pxp_tee_interface.h"
#include "intel_pxp_huc.h"
#include "gt/uc/intel_gsc_fw.h"
#include "gt/uc/intel_gsc_fwif.h"

static inline struct intel_pxp *i915_dev_to_pxp(struct device *i915_kdev)
{
	struct drm_i915_private *i915 = kdev_to_i915(i915_kdev);

	return &to_gt(i915)->pxp;
}

static int intel_pxp_tee_io_message(struct intel_pxp *pxp,
				    void *msg_in, u32 msg_in_size,
				    void *msg_out, u32 msg_out_max_size,
				    u32 *msg_out_rcv_size)
{
	struct drm_i915_private *i915 = pxp_to_gt(pxp)->i915;
	struct i915_pxp_component *pxp_component = pxp->pxp_component;
	int ret = 0;

	mutex_lock(&pxp->tee_mutex);

	/*
	 * The binding of the component is asynchronous from i915 probe, so we
	 * can't be sure it has happened.
	 */
	if (!pxp_component) {
		ret = -ENODEV;
		goto unlock;
	}

	ret = pxp_component->ops->send(pxp_component->tee_dev, msg_in, msg_in_size);
	if (ret) {
		drm_err(&i915->drm, "Failed to send PXP TEE message\n");
		goto unlock;
	}

	ret = pxp_component->ops->recv(pxp_component->tee_dev, msg_out, msg_out_max_size);
	if (ret < 0) {
		drm_err(&i915->drm, "Failed to receive PXP TEE message\n");
		goto unlock;
	}

	if (ret > msg_out_max_size) {
		drm_err(&i915->drm,
			"Failed to receive PXP TEE message due to unexpected output size\n");
		ret = -ENOSPC;
		goto unlock;
	}

	if (msg_out_rcv_size)
		*msg_out_rcv_size = ret;

	ret = 0;
unlock:
	mutex_unlock(&pxp->tee_mutex);
	return ret;
}

int intel_pxp_tee_stream_message(struct intel_pxp *pxp,
				 u8 client_id, u32 fence_id,
				 void *msg_in, size_t msg_in_len,
				 void *msg_out, size_t msg_out_len)
{
	/* TODO: for bigger objects we need to use a sg of 4k pages */
	const size_t max_msg_size = PAGE_SIZE;
	struct intel_gt *gt = pxp_to_gt(pxp);
	struct drm_i915_private *i915 = gt->i915;
	struct i915_pxp_component *pxp_component = pxp->pxp_component;
	unsigned int offset = 0;
	struct scatterlist *sg;
	int ret;

	if (intel_uc_supports_gsc_uc(&gt->uc))
		return -ENODEV;

	if (msg_in_len > max_msg_size || msg_out_len > max_msg_size)
		return -ENOSPC;

	mutex_lock(&pxp->tee_mutex);

	if (unlikely(!pxp_component || !pxp_component->ops->gsc_command)) {
		ret = -ENODEV;
		goto unlock;
	}

	GEM_BUG_ON(!pxp->stream_cmd.obj);

	sg = i915_gem_object_get_sg_dma(pxp->stream_cmd.obj, 0, &offset);

	memcpy(pxp->stream_cmd.vaddr, msg_in, msg_in_len);

	ret = pxp_component->ops->gsc_command(pxp_component->tee_dev, client_id,
					      fence_id, sg, msg_in_len, sg);
	if (ret < 0)
		drm_err(&i915->drm, "Failed to send PXP TEE gsc command\n");
	else
		memcpy(msg_out, pxp->stream_cmd.vaddr, msg_out_len);

unlock:
	mutex_unlock(&pxp->tee_mutex);
	return ret;
}

int intel_pxp_gsc_fw_message(struct intel_pxp *pxp,
			     void *msg_in, size_t msg_in_len,
			     void *msg_out, size_t msg_out_len)
{
	struct intel_gt *gt = pxp_to_gt(pxp);
	struct drm_i915_private *i915 = gt->i915;
	struct intel_gsc_mtl_header *header = pxp->stream_cmd.vaddr;
	const size_t max_msg_size = PAGE_SIZE - sizeof(*header);
	void *payload = pxp->stream_cmd.vaddr + sizeof(*header);
	u64 addr;
	u32 reply_size;
	int ret;

	if (!intel_uc_uses_gsc_uc(&gt->uc))
		return -ENODEV;

	if (msg_in_len > max_msg_size || msg_out_len > max_msg_size)
		return -ENOSPC;

	GEM_BUG_ON(!pxp->stream_cmd.vma);
	addr = i915_ggtt_offset(pxp->stream_cmd.vma);

	mutex_lock(&pxp->tee_mutex);

	memset(header, 0, sizeof(*header));
	header->validity_marker = GSC_HECI_VALIDITY_MARKER;
	header->gsc_address = HECI_MEADDRESS_PXP;
	header->header_version = MTL_GSC_HEADER_VERSION;
	header->message_size = msg_in_len + sizeof(*header);

	memcpy(payload, msg_in, msg_in_len);

	ret = intel_gsc_fw_heci_send(&gt->uc.gsc, addr, header->message_size,
				     addr, msg_out_len + sizeof(*header));
	if (ret) {
		drm_err(&i915->drm, "failed to send gsc PXP msg (%d)\n", ret);
		goto unlock;
	}

	/* we use the same mem for the reply, so header is in the same loc */
	reply_size = header->message_size - sizeof(*header);
	if (reply_size != msg_out_len)
		drm_err(&i915->drm, "unexpected PXP reply size %u (%u)\n",
			reply_size, (u32)msg_out_len);

	memcpy(msg_out, payload, msg_out_len);

unlock:
	mutex_unlock(&pxp->tee_mutex);
	return ret;
}

/**
 * i915_pxp_tee_component_bind - bind function to pass the function pointers to pxp_tee
 * @i915_kdev: pointer to i915 kernel device
 * @tee_kdev: pointer to tee kernel device
 * @data: pointer to pxp_tee_master containing the function pointers
 *
 * This bind function is called during the system boot or resume from system sleep.
 *
 * Return: return 0 if successful.
 */
static int i915_pxp_tee_component_bind(struct device *i915_kdev,
				       struct device *tee_kdev, void *data)
{
	struct drm_i915_private *i915 = kdev_to_i915(i915_kdev);
	struct intel_pxp *pxp = i915_dev_to_pxp(i915_kdev);
	struct intel_uc *uc = &pxp_to_gt(pxp)->uc;
	intel_wakeref_t wakeref;
	int ret = 0;

	/* If we control the GSC there is no need for the mei_pxp component */
	if (unlikely(intel_uc_supports_gsc_uc(uc)))
		return -EIO;

	mutex_lock(&pxp->tee_mutex);
	pxp->pxp_component = data;
	pxp->pxp_component->tee_dev = tee_kdev;
	mutex_unlock(&pxp->tee_mutex);

	if (intel_uc_uses_huc(uc) && intel_huc_is_loaded_by_gsc(&uc->huc)) {
		with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
			/* load huc via pxp */
			ret = intel_huc_fw_load_and_auth_via_gsc(&uc->huc);
			if (ret < 0)
				drm_err(&i915->drm, "failed to load huc via gsc %d\n", ret);
		}
	}

	if (intel_pxp_is_enabled(pxp)) {
		/* the component is required to fully start the PXP HW */
		intel_pxp_init_hw(pxp);
		ret = intel_pxp_wait_for_arb_start(pxp);
		if (ret) {
			drm_err(&i915->drm, "Failed to create arb session during bind\n");
			intel_pxp_fini_hw(pxp);
			pxp->pxp_component = NULL;
		}
	}

	return ret;
}

static void i915_pxp_tee_component_unbind(struct device *i915_kdev,
					  struct device *tee_kdev, void *data)
{
	struct intel_pxp *pxp = i915_dev_to_pxp(i915_kdev);

	if (intel_pxp_is_enabled(pxp))
		intel_pxp_fini_hw(pxp);

	mutex_lock(&pxp->tee_mutex);
	pxp->pxp_component = NULL;
	mutex_unlock(&pxp->tee_mutex);
}

static const struct component_ops i915_pxp_tee_component_ops = {
	.bind   = i915_pxp_tee_component_bind,
	.unbind = i915_pxp_tee_component_unbind,
};

static int alloc_streaming_command(struct intel_pxp *pxp)
{
	struct intel_gt *gt = pxp_to_gt(pxp);
	struct drm_i915_gem_object *obj = NULL;
	struct i915_vma *vma = NULL;
	void *cmd;
	int err;

	pxp->stream_cmd.obj = NULL;
	pxp->stream_cmd.vaddr = NULL;
	pxp->stream_cmd.vma = NULL;

	if (!IS_DGFX(gt->i915) && !intel_uc_uses_gsc_uc(&gt->uc))
		return 0;

	/* allocate object of one page for PXP command memory and store it */
	if (HAS_LMEM(gt->i915))
		obj = intel_gt_object_create_lmem(gt, PAGE_SIZE, I915_BO_ALLOC_CONTIGUOUS);
	else
		obj = i915_gem_object_create_shmem(gt->i915, PAGE_SIZE);

	if (IS_ERR(obj)) {
		drm_err(&gt->i915->drm, "Failed to allocate pxp streaming command!\n");
		return PTR_ERR(obj);
	}

	err = i915_gem_object_pin_pages_unlocked(obj);
	if (err) {
		drm_err(&gt->i915->drm, "Failed to pin gsc message page!\n");
		goto out_put;
	}

	/* map the lmem into the virtual memory pointer */
	cmd = i915_gem_object_pin_map_unlocked(obj, i915_coherent_map_type(gt->i915, obj, true));
	if (IS_ERR(cmd)) {
		drm_err(&gt->i915->drm, "Failed to map gsc message page!\n");
		err = PTR_ERR(cmd);
		goto out_unpin;
	}

	if (intel_uc_uses_gsc_uc(&gt->uc)) {
		vma = i915_vma_instance(obj, &gt->ggtt->vm, NULL);
		if (IS_ERR(vma)) {
			err = PTR_ERR(vma);
			goto out_unmap;
		}

		err = i915_vma_pin(vma, 0, 0, PIN_GLOBAL);
		if (err)
			goto out_unmap;
	}

	memset(cmd, 0, obj->base.size);

	pxp->stream_cmd.obj = obj;
	pxp->stream_cmd.vaddr = cmd;
	pxp->stream_cmd.vma = vma;

	return 0;

out_unmap:
	i915_gem_object_unpin_map(obj);
out_unpin:
	i915_gem_object_unpin_pages(obj);
out_put:
	i915_gem_object_put(obj);
	return err;
}

static void free_streaming_command(struct intel_pxp *pxp)
{
	struct drm_i915_gem_object *obj = fetch_and_zero(&pxp->stream_cmd.obj);

	if (!obj)
		return;

	if (pxp->stream_cmd.vma)
		i915_vma_unpin(fetch_and_zero(&pxp->stream_cmd.vma));

	i915_gem_object_unpin_map(obj);
	i915_gem_object_unpin_pages(obj);
	i915_gem_object_put(obj);
}

int intel_pxp_tee_component_init(struct intel_pxp *pxp)
{
	int ret;
	struct intel_gt *gt = pxp_to_gt(pxp);
	struct drm_i915_private *i915 = gt->i915;

	mutex_init(&pxp->tee_mutex);

	ret = alloc_streaming_command(pxp);
	if (ret)
		return ret;

	if (!intel_uc_supports_gsc_uc(&gt->uc)) {
		ret = component_add_typed(i915->drm.dev, &i915_pxp_tee_component_ops,
					  I915_COMPONENT_PXP);
		if (ret < 0) {
			drm_err(&i915->drm, "Failed to add PXP component (%d)\n", ret);
			goto out_free;
		}

		pxp->pxp_component_added = true;
	}

	return 0;

out_free:
	free_streaming_command(pxp);
	return ret;
}

void intel_pxp_tee_component_fini(struct intel_pxp *pxp)
{
	struct drm_i915_private *i915 = pxp_to_gt(pxp)->i915;

	if (pxp->pxp_component_added) {
		component_del(i915->drm.dev, &i915_pxp_tee_component_ops);
		pxp->pxp_component_added = false;
	}

	free_streaming_command(pxp);
}

int intel_pxp_tee_cmd_create_arb_session(struct intel_pxp *pxp,
					 int arb_session_id)
{
	struct drm_i915_private *i915 = pxp_to_gt(pxp)->i915;
	struct pxp_tee_create_arb_in msg_in = {0};
	struct pxp_tee_create_arb_out msg_out = {0};
	int ret;

	msg_in.header.api_version = PXP_TEE_APIVER;
	msg_in.header.command_id = PXP_TEE_ARB_CMDID;
	msg_in.header.buffer_len = sizeof(msg_in) - sizeof(msg_in.header);
	msg_in.protection_mode = PXP_TEE_ARB_PROTECTION_MODE;
	msg_in.session_id = arb_session_id;

	ret = intel_pxp_tee_io_message(pxp,
				       &msg_in, sizeof(msg_in),
				       &msg_out, sizeof(msg_out),
				       NULL);

	if (ret)
		drm_err(&i915->drm, "Failed to send tee msg ret=[%d]\n", ret);
	else if (msg_out.header.status != 0x0)
		drm_warn(&i915->drm, "PXP firmware failed arb session init request ret=[0x%08x]\n",
			 msg_out.header.status);

	return ret;
}
