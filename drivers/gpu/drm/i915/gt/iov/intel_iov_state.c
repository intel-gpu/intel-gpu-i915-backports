// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "i915_sriov_telemetry.h"

#include "intel_iov.h"
#include "intel_iov_event.h"
#include "intel_iov_state.h"
#include "intel_iov_utils.h"
#include "gem/i915_gem_lmem.h"
#include "gt/intel_context.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_pm.h"
#include "gt/uc/abi/guc_actions_pf_abi.h"

static void pf_state_worker_func(struct work_struct *w);

/**
 * intel_iov_state_init_early - Allocate structures for VFs state data.
 * @iov: the IOV struct
 *
 * VFs state data is maintained in the flexible array where:
 *   - entry [0] contains state data of the PF (if applicable),
 *   - entries [1..n] contain state data of VF1..VFn::
 *
 *       <--------------------------- 1 + total_vfs ----------->
 *      +-------+-------+-------+-----------------------+-------+
 *      |   0   |   1   |   2   |                       |   n   |
 *      +-------+-------+-------+-----------------------+-------+
 *      |  PF   |  VF1  |  VF2  |      ...     ...      |  VFn  |
 *      +-------+-------+-------+-----------------------+-------+
 *
 * This function can only be called on PF.
 */
void intel_iov_state_init_early(struct intel_iov *iov)
{
	struct intel_iov_data *data;

	GEM_BUG_ON(!intel_iov_is_pf(iov));
	GEM_BUG_ON(iov->pf.state.data);

	INIT_WORK(&iov->pf.state.worker, pf_state_worker_func);

	data = kcalloc(1 + pf_get_totalvfs(iov), sizeof(*data), GFP_KERNEL);
	if (unlikely(!data)) {
		pf_update_status(iov, -ENOMEM, "state");
		return;
	}

	iov->pf.state.data = data;
}

/**
 * intel_iov_state_release - Release structures used VFs data.
 * @iov: the IOV struct
 *
 * Release structures used for VFs data.
 * This function can only be called on PF.
 */
void intel_iov_state_release(struct intel_iov *iov)
{
	GEM_BUG_ON(!intel_iov_is_pf(iov));

	cancel_work_sync(&iov->pf.state.worker);
	kfree(fetch_and_zero(&iov->pf.state.data));
}

static void pf_reset_vf_state(struct intel_iov *iov, u32 vfid)
{
	iov->pf.state.data[vfid].state = 0;
	iov->pf.state.data[vfid].paused = false;
}

/**
 * intel_iov_state_reset - Reset VFs data.
 * @iov: the IOV struct
 *
 * Reset VFs data.
 * This function can only be called on PF.
 */
void intel_iov_state_reset(struct intel_iov *iov)
{
	u16 n;

	GEM_BUG_ON(!intel_iov_is_pf(iov));

	if (!iov->pf.state.data)
		return;

	for (n = 0; n < 1 + pf_get_totalvfs(iov); n++) {
		pf_reset_vf_state(iov, n);
	}
}

static int guc_action_vf_control_cmd(struct intel_guc *guc, u32 vfid, u32 cmd)
{
	u32 request[PF2GUC_VF_CONTROL_REQUEST_MSG_LEN] = {
		FIELD_PREP(GUC_HXG_MSG_0_ORIGIN, GUC_HXG_ORIGIN_HOST) |
		FIELD_PREP(GUC_HXG_MSG_0_TYPE, GUC_HXG_TYPE_REQUEST) |
		FIELD_PREP(GUC_HXG_REQUEST_MSG_0_ACTION, GUC_ACTION_PF2GUC_VF_CONTROL),
		FIELD_PREP(PF2GUC_VF_CONTROL_REQUEST_MSG_1_VFID, vfid),
		FIELD_PREP(PF2GUC_VF_CONTROL_REQUEST_MSG_2_COMMAND, cmd),
	};
	int ret;

	ret = intel_guc_send(guc, request, ARRAY_SIZE(request));
	return ret > 0 ? -EPROTO : ret;
}

static int pf_control_vf(struct intel_iov *iov, u32 vfid, u32 cmd)
{
	intel_wakeref_t wakeref;
	int err = -ENONET;

	GEM_BUG_ON(!intel_iov_is_pf(iov));
	GEM_BUG_ON(vfid > pf_get_totalvfs(iov));
	GEM_BUG_ON(!vfid);

	with_intel_gt_pm(iov_to_gt(iov), wakeref)
		err = guc_action_vf_control_cmd(iov_to_guc(iov), vfid, cmd);

	return err;
}

static int pf_trigger_vf_flr_start(struct intel_iov *iov, u32 vfid)
{
	int err;

	err = pf_control_vf(iov, vfid, GUC_PF_TRIGGER_VF_FLR_START);
	if (unlikely(err))
		IOV_ERROR(iov, "Failed to start FLR for VF%u (%pe)\n",
			  vfid, ERR_PTR(err));
	return err;
}

static int pf_trigger_vf_flr_finish(struct intel_iov *iov, u32 vfid)
{
	int err;

	err = pf_control_vf(iov, vfid, GUC_PF_TRIGGER_VF_FLR_FINISH);
	if (unlikely(err))
		IOV_ERROR(iov, "Failed to confirm FLR for VF%u (%pe)\n",
			  vfid, ERR_PTR(err));
	return err;
}

static void pf_clear_vf_ggtt_entries(struct intel_iov *iov, u32 vfid)
{
	struct intel_iov_config *config = &iov->pf.provisioning.configs[vfid];
	struct intel_gt *gt = iov_to_gt(iov);

	GEM_BUG_ON(vfid > pf_get_totalvfs(iov));
	lockdep_assert_held(pf_provisioning_mutex(iov));

	if (!drm_mm_node_allocated(&config->ggtt_region))
		return;

	i915_ggtt_set_space_owner(gt->ggtt, vfid, &config->ggtt_region);
}

static void pf_clear_vf_lmem_obj(struct intel_iov *iov, u32 vfid)
{
	struct drm_i915_gem_object *obj;
	int err;

	lockdep_assert_held(pf_provisioning_mutex(iov));

	obj = iov->pf.provisioning.configs[vfid].lmem_obj;
	if (!obj)
		return;

	err = i915_gem_object_clear_lmem(obj);
	if (unlikely(err))
		IOV_ERROR(iov, "Failed to clear VF%u LMEM (%pe)\n",
			  vfid, ERR_PTR(err));
}

static bool pf_vfs_flr_enabled(struct intel_iov *iov, u32 vfid)
{
	return iov_to_i915(iov)->params.vfs_flr_mask & BIT(vfid);
}

static int pf_process_vf_flr_finish(struct intel_iov *iov, u32 vfid)
{
	if (!pf_vfs_flr_enabled(iov, vfid)) {
		IOV_DEBUG(iov, "VF%u FLR processing skipped\n", vfid);
		goto skip;
	}
	IOV_DEBUG(iov, "processing VF%u FLR\n", vfid);

	intel_iov_event_reset(iov, vfid);

	mutex_lock(pf_provisioning_mutex(iov));
	pf_clear_vf_ggtt_entries(iov, vfid);
	if (HAS_LMEM(iov_to_i915(iov)))
		pf_clear_vf_lmem_obj(iov, vfid);
	mutex_unlock(pf_provisioning_mutex(iov));

	if (iov_is_root(iov))
		i915_sriov_telemetry_pf_reset(iov_to_i915(iov), vfid);

skip:
	return pf_trigger_vf_flr_finish(iov, vfid);
}

/* Return: true if more processing is needed */
static bool pf_process_vf(struct intel_iov *iov, u32 vfid)
{
	unsigned long *state = &iov->pf.state.data[vfid].state;
	int err;

	if (test_and_clear_bit(IOV_VF_NEEDS_FLR_START, state)) {
		err = pf_trigger_vf_flr_start(iov, vfid);
		if (err == -EBUSY) {
			set_bit(IOV_VF_NEEDS_FLR_START, state);
			return true;
		}
		if (err) {
			set_bit(IOV_VF_FLR_FAILED, state);
			clear_bit(IOV_VF_FLR_IN_PROGRESS, state);
			return false;
		}
		clear_bit(IOV_VF_PAUSE_IN_PROGRESS, state);
		return true;
	}

	if (test_and_clear_bit(IOV_VF_FLR_DONE_RECEIVED, state)) {
		set_bit(IOV_VF_NEEDS_FLR_FINISH, state);
		return true;
	}

	if (test_and_clear_bit(IOV_VF_NEEDS_FLR_FINISH, state)) {
		err = pf_process_vf_flr_finish(iov, vfid);
		if (err == -EBUSY) {
			set_bit(IOV_VF_NEEDS_FLR_FINISH, state);
			return true;
		}
		if (err) {
			set_bit(IOV_VF_FLR_FAILED, state);
			clear_bit(IOV_VF_FLR_IN_PROGRESS, state);
			return false;
		}
		clear_bit(IOV_VF_FLR_IN_PROGRESS, state);
		return false;
	}

	return false;
}

static void pf_queue_worker(struct intel_iov *iov)
{
	queue_work(system_unbound_wq, &iov->pf.state.worker);
}

static void pf_process_all_vfs(struct intel_iov *iov)
{
	unsigned int num_vfs = pf_get_totalvfs(iov);
	unsigned int n;
	bool more = false;

	/* only VFs need processing */
	for (n = 1; n <= num_vfs; n++)
		more |= pf_process_vf(iov, n);

	if (more)
		pf_queue_worker(iov);
}

static void pf_state_worker_func(struct work_struct *w)
{
	struct intel_iov *iov = container_of(w, struct intel_iov, pf.state.worker);

	pf_process_all_vfs(iov);
}

/**
 * DOC: VF FLR Flow
 *
 *          PF                        GUC             PCI
 * ========================================================
 *          |                          |               |
 * (1)      |                          |<------- FLR --|
 *          |                          |               :
 * (2)      |<----------- NOTIFY FLR --|
 *         [ ]                         |
 * (3)     [ ]                         |
 *         [ ]                         |
 *          |-- START FLR ------------>|
 *          |                         [ ]
 * (4)      |                         [ ]
 *          |                         [ ]
 *          |<------------- FLR DONE --|
 *         [ ]                         |
 * (5)     [ ]                         |
 *         [ ]                         |
 *          |-- FINISH FLR ----------->|
 *          |                          |
 *
 * Step 1: PCI HW generates interrupt to GuC about VF FLR
 * Step 2: GuC FW sends G2H notification to PF about VF FLR
 * Step 3: PF sends H2G request to GuC to start VF FLR sequence
 * Step 4: GuC FW performs VF FLR cleanups and notifies PF when done
 * Step 5: PF performs VF FLR cleanups and notifies GuC FW when finished
 */

static void pf_init_vf_flr(struct intel_iov *iov, u32 vfid)
{
	unsigned long *state = &iov->pf.state.data[vfid].state;

	if (test_and_set_bit(IOV_VF_FLR_IN_PROGRESS, state)) {
		IOV_DEBUG(iov, "VF%u FLR is already in progress\n", vfid);
		return;
	}

	set_bit(IOV_VF_NEEDS_FLR_START, state);
	pf_queue_worker(iov);
}

static void pf_handle_vf_flr(struct intel_iov *iov, u32 vfid)
{
	struct device *dev = iov_to_dev(iov);
	struct intel_gt *gt;
	unsigned int gtid;

	if (!iov_is_root(iov)) {
		IOV_ERROR(iov, "Unexpected VF%u FLR notification\n", vfid);
		return;
	}

	iov->pf.state.data[vfid].paused = false;
	dev_info(dev, "VF%u FLR\n", vfid);

	for_each_gt(gt, iov_to_i915(iov), gtid)
		pf_init_vf_flr(&gt->iov, vfid);
}

static void pf_handle_vf_flr_done(struct intel_iov *iov, u32 vfid)
{
	unsigned long *state = &iov->pf.state.data[vfid].state;

	set_bit(IOV_VF_FLR_DONE_RECEIVED, state);
	pf_queue_worker(iov);
}

static void pf_handle_vf_pause_done(struct intel_iov *iov, u32 vfid)
{
	struct device *dev = iov_to_dev(iov);
	struct intel_iov_data *data = &iov->pf.state.data[vfid];

	data->paused = true;
	clear_bit(IOV_VF_PAUSE_IN_PROGRESS, &data->state);
	dev_info(dev, "VF%u %s\n", vfid, "paused");
}

static void pf_handle_vf_fixup_done(struct intel_iov *iov, u32 vfid)
{
	struct device *dev = iov_to_dev(iov);

	dev_info(dev, "VF%u %s\n", vfid, "has completed migration");
}

static int pf_handle_vf_event(struct intel_iov *iov, u32 vfid, u32 eventid)
{
	switch (eventid) {
	case GUC_PF_NOTIFY_VF_FLR:
		pf_handle_vf_flr(iov, vfid);
		break;
	case GUC_PF_NOTIFY_VF_FLR_DONE:
		pf_handle_vf_flr_done(iov, vfid);
		break;
	case GUC_PF_NOTIFY_VF_PAUSE_DONE:
		pf_handle_vf_pause_done(iov, vfid);
		break;
	case GUC_PF_NOTIFY_VF_FIXUP_DONE:
		pf_handle_vf_fixup_done(iov, vfid);
		break;
	default:
		return -ENOPKG;
	}

	return 0;
}

static int pf_handle_pf_event(struct intel_iov *iov, u32 eventid)
{
	switch (eventid) {
	case GUC_PF_NOTIFY_VF_ENABLE:
		IOV_DEBUG(iov, "VFs %s/%s\n", str_enabled_disabled(true), str_enabled_disabled(false));
		break;
	default:
		return -ENOPKG;
	}

	return 0;
}

/**
 * intel_iov_state_process_guc2pf - Handle VF state notification from GuC.
 * @iov: the IOV struct
 * @msg: message from the GuC
 * @len: length of the message
 *
 * This function is for PF only.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int intel_iov_state_process_guc2pf(struct intel_iov *iov,
				   const u32 *msg, u32 len)
{
	u32 vfid;
	u32 eventid;

	GEM_BUG_ON(!len);
	GEM_BUG_ON(FIELD_GET(GUC_HXG_MSG_0_ORIGIN, msg[0]) != GUC_HXG_ORIGIN_GUC);
	GEM_BUG_ON(FIELD_GET(GUC_HXG_MSG_0_TYPE, msg[0]) != GUC_HXG_TYPE_EVENT);
	GEM_BUG_ON(FIELD_GET(GUC_HXG_EVENT_MSG_0_ACTION, msg[0]) != GUC_ACTION_GUC2PF_VF_STATE_NOTIFY);

	if (unlikely(!intel_iov_is_pf(iov)))
		return -EPROTO;

	if (unlikely(FIELD_GET(GUC2PF_VF_STATE_NOTIFY_EVENT_MSG_0_MBZ, msg[0])))
		return -EPFNOSUPPORT;

	if (unlikely(len != GUC2PF_VF_STATE_NOTIFY_EVENT_MSG_LEN))
		return -EPROTO;

	vfid = FIELD_GET(GUC2PF_VF_STATE_NOTIFY_EVENT_MSG_1_VFID, msg[1]);
	eventid = FIELD_GET(GUC2PF_VF_STATE_NOTIFY_EVENT_MSG_2_EVENT, msg[2]);

	if (unlikely(vfid > pf_get_totalvfs(iov)))
		return -EINVAL;

	return vfid ? pf_handle_vf_event(iov, vfid, eventid) : pf_handle_pf_event(iov, eventid);
}

/**
 * intel_iov_state_start_flr - Start VF FLR sequence.
 * @iov: the IOV struct
 * @vfid: VF identifier
 *
 * This function is for PF only.
 */
void intel_iov_state_start_flr(struct intel_iov *iov, u32 vfid)
{
	GEM_BUG_ON(!intel_iov_is_pf(iov));
	GEM_BUG_ON(vfid > pf_get_totalvfs(iov));
	GEM_BUG_ON(!vfid);

	pf_init_vf_flr(iov, vfid);
}

/**
 * intel_iov_state_no_flr - Test if VF FLR is not in progress.
 * @iov: the IOV struct
 * @vfid: VF identifier
 *
 * This function is for PF only.
 *
 * Return: true if FLR is not pending or in progress.
 */
bool intel_iov_state_no_flr(struct intel_iov *iov, u32 vfid)
{
	GEM_BUG_ON(!intel_iov_is_pf(iov));
	GEM_BUG_ON(vfid > pf_get_totalvfs(iov));
	GEM_BUG_ON(!vfid);

	return !test_bit(IOV_VF_FLR_IN_PROGRESS, &iov->pf.state.data[vfid].state);
}

/**
 * intel_iov_state_no_pause - Test if VF pause is not pending nor active.
 * @iov: the IOV struct instance
 * @vfid: VF identifier
 *
 * This function is for PF only.
 *
 * Return: true if VF pause is not pending nor active.
 */
bool intel_iov_state_no_pause(struct intel_iov *iov, u32 vfid)
{
	struct intel_iov_data *data = &iov->pf.state.data[vfid];

	GEM_BUG_ON(!intel_iov_is_pf(iov));
	GEM_BUG_ON(vfid > pf_get_totalvfs(iov));
	GEM_BUG_ON(!vfid);

	return !test_bit(IOV_VF_PAUSE_IN_PROGRESS, &data->state) && !data->paused;
}

/**
 * intel_iov_state_pause_vf - Pause VF.
 * @iov: the IOV struct
 * @vfid: VF identifier
 *
 * This function is for PF only.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int intel_iov_state_pause_vf(struct intel_iov *iov, u32 vfid)
{
	struct intel_iov_data *data = &iov->pf.state.data[vfid];
	int err;


	if (!intel_iov_state_no_flr(iov, vfid) || !intel_iov_state_no_pause(iov, vfid)) {
		IOV_ERROR(iov, "VF%u cannot be paused in current state\n", vfid);
		return -EBUSY;
	}

	if (test_and_set_bit(IOV_VF_PAUSE_IN_PROGRESS, &data->state)) {
		IOV_ERROR(iov, "VF%u pause is already in progress\n", vfid);
		return -EBUSY;
	}

	err = pf_control_vf(iov, vfid, GUC_PF_TRIGGER_VF_PAUSE);

	if (unlikely(err < 0)) {
		clear_bit(IOV_VF_PAUSE_IN_PROGRESS, &data->state);
		IOV_ERROR(iov, "Failed to trigger VF%u pause (%pe)\n", vfid, ERR_PTR(err));
		return err;
	}

	return 0;
}

#define I915_VF_PAUSE_TIMEOUT_MS 500

/**
 * intel_iov_state_pause_vf_sync - Pause VF on one GuC, wait until the state settles.
 * @iov: the IOV struct instance linked to target GuC
 * @vfid: VF identifier
 * @inferred: marks if the pause was not requested by user, but by the kernel
 *
 * The function issues a pause command only if the VF is not already paused or
 * in process of pausing. Then it waits for the confirmation of pause completion.
 * This function is for PF only.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int intel_iov_state_pause_vf_sync(struct intel_iov *iov, u32 vfid, bool inferred)
{
	struct intel_iov_data *data = &iov->pf.state.data[vfid];
	unsigned long timeout_ms = I915_VF_PAUSE_TIMEOUT_MS;
	int ret;

	if (intel_iov_state_no_pause(iov, vfid)) {
		ret = intel_iov_state_pause_vf(iov, vfid);
		if (ret) {
			IOV_ERROR(iov, "Failed to pause VF%u: (%pe)", vfid, ERR_PTR(ret));
			return ret;
		}
		if (inferred)
			set_bit(IOV_VF_PAUSE_BY_SUSPEND, &data->state);
	}

	if (!inferred)
		clear_bit(IOV_VF_PAUSE_BY_SUSPEND, &data->state);

	/* FIXME: How long we should wait? */
	if (wait_for(data->paused, timeout_ms)) {
		IOV_ERROR(iov, "VF%u pause didn't complete within %lu ms\n", vfid, timeout_ms);
		return -ETIMEDOUT;
	}

	return 0;
}

/**
 * intel_iov_state_resume_vf - Resume VF.
 * @iov: the IOV struct
 * @vfid: VF identifier
 *
 * This function is for PF only.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int intel_iov_state_resume_vf(struct intel_iov *iov, u32 vfid)
{
	struct intel_iov_data *data = &iov->pf.state.data[vfid];
	int ret;

	ret = pf_control_vf(iov, vfid, GUC_PF_TRIGGER_VF_RESUME);
	if (ret)
		return ret;

	data->paused = false;

	return ret;
}

/**
 * intel_iov_state_stop_vf - Stop VF.
 * @iov: the IOV struct
 * @vfid: VF identifier
 *
 * This function is for PF only.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int intel_iov_state_stop_vf(struct intel_iov *iov, u32 vfid)
{
	return pf_control_vf(iov, vfid, GUC_PF_TRIGGER_VF_STOP);
}

static int guc_action_save_restore_vf(struct intel_guc *guc, u32 vfid, u32 opcode,
				       u64 offset, u32 size)
{
	u32 request[PF2GUC_SAVE_RESTORE_VF_REQUEST_MSG_LEN] = {
		FIELD_PREP(GUC_HXG_MSG_0_ORIGIN, GUC_HXG_ORIGIN_HOST) |
		FIELD_PREP(GUC_HXG_MSG_0_TYPE, GUC_HXG_TYPE_REQUEST) |
		FIELD_PREP(GUC_HXG_REQUEST_MSG_0_ACTION, GUC_ACTION_PF2GUC_SAVE_RESTORE_VF) |
		FIELD_PREP(PF2GUC_SAVE_RESTORE_VF_REQUEST_MSG_0_OPCODE, opcode),
		FIELD_PREP(PF2GUC_SAVE_RESTORE_VF_REQUEST_MSG_1_VFID, vfid),
		FIELD_PREP(PF2GUC_SAVE_RESTORE_VF_REQUEST_MSG_2_BUFF_LO, lower_32_bits(offset)),
		FIELD_PREP(PF2GUC_SAVE_RESTORE_VF_REQUEST_MSG_3_BUFF_HI, upper_32_bits(offset)),
		FIELD_PREP(PF2GUC_SAVE_RESTORE_VF_REQUEST_MSG_4_BUFF_SZ, size) |
		FIELD_PREP(PF2GUC_SAVE_RESTORE_VF_REQUEST_MSG_4_MBZ, 0),
	};
	int ret;

	ret = intel_guc_send(guc, request, ARRAY_SIZE(request));

	return (offset && ret > size) ? -EPROTO : ret;
}

static int pf_save_vf_size(struct intel_iov *iov, u32 vfid)
{
	struct intel_guc *guc = iov_to_guc(iov);
	int ret;

	GEM_BUG_ON(!intel_iov_is_pf(iov));
	GEM_BUG_ON(vfid > pf_get_totalvfs(iov));
	GEM_BUG_ON(!vfid);

	ret = guc_action_save_restore_vf(guc, vfid, GUC_PF_OPCODE_VF_SAVE, 0, 0);

	if (unlikely(ret < 0)) {
		IOV_ERROR(iov, "Failed to query VF%u save state size (%pe)\n", vfid, ERR_PTR(ret));
		return ret;
	}

	return ret * sizeof(u32);
}

static int pf_save_vf(struct intel_iov *iov, u32 vfid, void *buf, u32 size)
{
	struct intel_guc *guc = iov_to_guc(iov);
	struct i915_vma *vma;
	void *blob;
	int ret;
	u32 rsize = 0;

	GEM_BUG_ON(!intel_iov_is_pf(iov));
	GEM_BUG_ON(vfid > pf_get_totalvfs(iov));
	GEM_BUG_ON(!vfid);

	ret = intel_guc_allocate_and_map_vma(guc, size, &vma, (void **)&blob);
	if (unlikely(ret))
		goto failed;

	ret = guc_action_save_restore_vf(guc, vfid, GUC_PF_OPCODE_VF_SAVE,
					 intel_guc_ggtt_offset(guc, vma),
					 size / sizeof(u32));

	if (likely(ret > 0)) {
		memcpy(buf, blob, size);
		rsize = ret * sizeof(u32);

		if (IS_ENABLED(CPTCFG_DRM_I915_SELFTEST) &&
		    memchr_inv(buf + rsize, 0, size - rsize)) {
			pr_err("non-zero state found beyond offset %u!\n", rsize);
		}
	}

	i915_vma_unpin_and_release(&vma, I915_VMA_RELEASE_MAP);

	if (unlikely(ret < 0))
		goto failed;

	IOV_DEBUG(iov, "VF%u: state saved (%u bytes) %*ph ..\n",
		  vfid, rsize, min_t(u32, 16, rsize), buf);
	return rsize;

failed:
	IOV_ERROR(iov, "Failed to save VF%u state (%pe)\n", vfid, ERR_PTR(ret));
	return ret;
}

/*
 * intel_iov_state_save_vf_size - Query VF save state size.
 * @iov: the IOV struct
 * @vfid: VF identifier
 *
 * This function is for PF only.
 *
 * Return: size in bytes on success or a negative error code on failure.
 */
int intel_iov_state_save_vf_size(struct intel_iov *iov, u32 vfid)
{
	intel_wakeref_t wakeref;
	int ret = -ENONET;

	with_intel_gt_pm(iov_to_gt(iov), wakeref)
		ret = pf_save_vf_size(iov, vfid);

	return ret;
}

/**
 * intel_iov_state_save_vf - Save VF state.
 * @iov: the IOV struct
 * @vfid: VF identifier
 * @buf: buffer to save VF state
 * @size: size of the buffer (in bytes)
 *
 * This function is for PF only.
 *
 * Return: saved state size (in bytes) on success or a negative error code on failure.
 */
int intel_iov_state_save_vf(struct intel_iov *iov, u32 vfid, void *buf, size_t size)
{
	intel_wakeref_t wakeref;
	int ret = -ENONET;

	if (size < PF2GUC_SAVE_RESTORE_VF_BUFF_MIN_SIZE)
		return -EINVAL;

	with_intel_gt_pm(iov_to_gt(iov), wakeref)
		ret = pf_save_vf(iov, vfid, buf, size);

	return ret;
}

static int pf_restore_vf(struct intel_iov *iov, u32 vfid, const void *buf, size_t size)
{
	struct intel_guc *guc = iov_to_guc(iov);
	struct i915_vma *vma;
	void *blob;
	int ret;
	u32 rsize = 0;

	GEM_BUG_ON(!intel_iov_is_pf(iov));
	GEM_BUG_ON(vfid > pf_get_totalvfs(iov));
	GEM_BUG_ON(!vfid);

	ret = intel_guc_allocate_and_map_vma(guc, size,
					     &vma, (void **)&blob);
	if (unlikely(ret < 0))
		goto failed;

	memcpy(blob, buf, size);

	ret = guc_action_save_restore_vf(guc, vfid, GUC_PF_OPCODE_VF_RESTORE,
					 intel_guc_ggtt_offset(guc, vma),
					 size / sizeof(u32));

	i915_vma_unpin_and_release(&vma, I915_VMA_RELEASE_MAP);

	if (unlikely(ret < 0))
		goto failed;

	rsize = ret * sizeof(u32);
	IOV_DEBUG(iov, "VF%u: state restored (%u bytes) %*ph\n",
		  vfid, rsize, min_t(u32, 16, rsize), buf);
	return rsize;

failed:
	IOV_ERROR(iov, "Failed to restore VF%u state (%pe) %*ph\n",
		  vfid, ERR_PTR(ret), 16, buf);
	return ret;
}

int intel_iov_state_store_guc_migration_state(struct intel_iov *iov, u32 vfid,
					      const void *buf, size_t size)
{
	int ret;

	if (size < PF2GUC_SAVE_RESTORE_VF_BUFF_MIN_SIZE)
		return -EINVAL;

	mutex_lock(pf_provisioning_mutex(iov));
	ret = intel_iov_state_restore_vf(iov, vfid, buf, size);
	mutex_unlock(pf_provisioning_mutex(iov));

	if (ret < 0)
		return ret;
	return 0;
}

/*
 * intel_iov_state_restore_vf - Restore VF state.
 * @iov: the IOV struct
 * @vfid: VF identifier
 * @buf: buffer with VF state to restore
 * @size: size of the buffer (in bytes)
 *
 * This function is for PF only.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int intel_iov_state_restore_vf(struct intel_iov *iov, u32 vfid, const void *buf, size_t size)
{
	intel_wakeref_t wakeref;
	int err = -ENONET;

	with_intel_gt_pm(iov_to_gt(iov), wakeref)
		err = pf_restore_vf(iov, vfid, buf, size);

	return err;
}

/**
 * intel_iov_state_save_ggtt - Save VF GGTT.
 * @iov: the IOV struct
 * @vfid: VF identifier
 * @buf: buffer to save VF GGTT
 * @size: size of buffer to save VF GGTT
 *
 * This function is for PF only.
 *
 * Return: Size of data written on success or a negative error code on failure.
 */
ssize_t intel_iov_state_save_ggtt(struct intel_iov *iov, u32 vfid, void *buf, size_t size)
{
	struct drm_mm_node *node = &iov->pf.provisioning.configs[vfid].ggtt_region;
	struct i915_ggtt *ggtt = iov_to_gt(iov)->ggtt;
	intel_wakeref_t wakeref;
	ssize_t ret;

	GEM_BUG_ON(!intel_iov_is_pf(iov));

	mutex_lock(pf_provisioning_mutex(iov));

	if (!drm_mm_node_allocated(node)) {
		ret = -EINVAL;
		goto out;
	}

	with_intel_gt_pm(iov_to_gt(iov), wakeref)
		ret = i915_ggtt_save_ptes(ggtt, node, buf, size, I915_GGTT_SAVE_PTES_NO_VFID);

out:
	mutex_unlock(pf_provisioning_mutex(iov));

	return ret;
}

/**
 * intel_iov_state_restore_ggtt - Restore VF GGTT.
 * @iov: the IOV struct
 * @vfid: VF identifier
 * @buf: buffer with VF GGTT to restore
 * @size: size of buffer with VF GGTT
 *
 * This function is for PF only.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int intel_iov_state_restore_ggtt(struct intel_iov *iov, u32 vfid, const void *buf, size_t size)
{
	struct drm_mm_node *node = &iov->pf.provisioning.configs[vfid].ggtt_region;
	struct i915_ggtt *ggtt = iov_to_gt(iov)->ggtt;
	intel_wakeref_t wakeref;
	int ret;

	GEM_BUG_ON(!intel_iov_is_pf(iov));

	mutex_lock(pf_provisioning_mutex(iov));

	with_intel_gt_pm(iov_to_gt(iov), wakeref)
		ret = i915_ggtt_restore_ptes(ggtt, node, buf, size,
					     FIELD_PREP(I915_GGTT_RESTORE_PTES_VFID_MASK, vfid) |
					     I915_GGTT_RESTORE_PTES_NEW_VFID);

	mutex_unlock(pf_provisioning_mutex(iov));

	return ret;
}

void *intel_iov_state_map_lmem(struct intel_iov *iov, u32 vfid)
{
	struct drm_i915_private *i915 = iov_to_i915(iov);
	struct drm_i915_gem_object *obj = iov->pf.provisioning.configs[vfid].lmem_obj;
	void *vaddr;

	GEM_BUG_ON(!intel_iov_is_pf(iov));

	if (!obj)
		return ERR_PTR(-EINVAL);

	vaddr = i915_gem_object_pin_map_unlocked(obj,
						 i915_coherent_map_type(i915, obj, true));
	if (IS_ERR(vaddr))
		return vaddr;

	return vaddr;
}

void intel_iov_state_unmap_lmem(struct intel_iov *iov, u32 vfid)
{
	struct drm_i915_gem_object *obj = iov->pf.provisioning.configs[vfid].lmem_obj;

	GEM_BUG_ON(!intel_iov_is_pf(iov));

	i915_gem_object_unpin_map(obj);
}

static int
save_restore_lmem_chunk(struct intel_iov *iov, u32 vfid, struct drm_i915_gem_object *smem,
			u64 offset, size_t size, bool save)
{
	struct drm_i915_gem_object *lmem;
	struct i915_request *rq;
	int err;

	lmem = iov->pf.provisioning.configs[vfid].lmem_obj;
	if (!lmem)
		return -ENXIO;

	if (!i915_gem_object_trylock(lmem))
		return -EBUSY;
	if (!i915_gem_object_trylock(smem)) {
		err = -EBUSY;
		goto err_lmem_obj_unlock;
	}

	rq = i915_gem_object_copy_lmem(lmem, offset, smem, 0, size, save, false);
	if (IS_ERR(rq)) {
		err = PTR_ERR(rq);
		goto err_smem_obj_unlock;
	}

	if (i915_request_wait(rq, 0, HZ) < 0)
		err = -ETIME;
	else
		err = rq->fence.error;
	i915_request_put(rq);

err_smem_obj_unlock:
	i915_gem_object_unlock(smem);
err_lmem_obj_unlock:
	i915_gem_object_unlock(lmem);

	return err < 0 ? err : 0;
}

/**
 * intel_iov_state_save_lmem_chunk - Save VF LMEM chunk.
 * @iov: the IOV struct
 * @vfid: VF identifier
 * @smem: SMEM object to save VF LMEM chunk
 * @offset: offset of VF LMEM chunk
 * @size: size of chunk to save
 *
 * This function is for PF only.
 *
 * Return: Size of data written on success or a negative error code on failure.
 */
ssize_t intel_iov_state_save_lmem_chunk(struct intel_iov *iov, u32 vfid,
					struct drm_i915_gem_object *smem, u64 offset, size_t size)
{
	int ret;

	ret = save_restore_lmem_chunk(iov, vfid, smem, offset, size, true);
	if (ret < 0)
		return ret;

	return size;
}

/**
 * intel_iov_state_restore_lmem_chunk - Restore VF LMEM chunk.
 * @iov: the IOV struct
 * @vfid: VF identifier
 * @smem: SMEM object with VF LMEM to restore
 * @offset: offset of VF LMEM chunk
 * @size: size of chunk to restore
 *
 * This function is for PF only.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int intel_iov_state_restore_lmem_chunk(struct intel_iov *iov, u32 vfid,
				       struct drm_i915_gem_object *smem, u64 offset, size_t size)
{
	int ret;

	ret = save_restore_lmem_chunk(iov, vfid, smem, offset, size, false);
	if (ret < 0)
		return ret;

	return 0;
}

#define COMPRESSION_RATIO 256

static int
copy_ccs_chunk(struct drm_i915_gem_object *smem, u64 smem_offset,
	       struct drm_i915_gem_object *lmem, u64 lmem_offset,
	       u32 size, bool save)
{
	struct i915_vma *smem_vma, *lmem_vma;
	struct intel_context *ce;
	struct i915_request *rq;
	int err;

	if (size > SZ_64M)
		return -EINVAL;

	if (lmem_offset + size > lmem->base.size)
		return -EINVAL;

	if (smem->base.size < size / COMPRESSION_RATIO)
		return -EINVAL;

	ce = intel_context_create(lmem->mm.region.mem->gt->engine[BCS0]);
	if (IS_ERR(ce))
		return PTR_ERR(ce);

	smem_vma = i915_vma_instance(smem, ce->vm, NULL);
	if (IS_ERR(smem_vma)) {
		err = PTR_ERR(smem_vma);
		goto err_context_put;
	}

	err = i915_vma_pin(smem_vma, 0, 0, PIN_USER | PIN_ZONE_48);
	if (err)
		goto err_context_put;

	lmem_vma = i915_vma_instance(lmem, ce->vm, NULL);
	if (IS_ERR(lmem_vma)) {
		err = PTR_ERR(lmem_vma);
		goto err_smem_vma_unpin;
	}

	err = i915_vma_pin(lmem_vma, 0, 0, PIN_USER | PIN_ZONE_48);
	if (err)
		goto err_smem_vma_unpin;

	rq = intel_context_create_request(ce);
	if (IS_ERR(rq)) {
		err = PTR_ERR(rq);
		goto err_lmem_vma_unpin;
	}

	err = i915_gem_ccs_emit_swap(rq, i915_vma_offset(lmem_vma) + lmem_offset,
					 i915_vma_offset(smem_vma) + smem_offset,
					 size, save);
	if (err)
		goto err_rq_put;

	i915_request_get(rq);
	i915_request_add(rq);

	if (i915_request_wait(rq, 0, HZ) < 0)
		err = -ETIME;
	else
		err = rq->fence.error;

err_rq_put:
	i915_request_put(rq);
err_lmem_vma_unpin:
	i915_vma_unpin(lmem_vma);
err_smem_vma_unpin:
	i915_vma_unpin(smem_vma);
err_context_put:
	intel_context_put(ce);

	return err < 0 ? err : 0;
}

static int copy_ccs(struct drm_i915_gem_object *smem, u64 smem_offset,
		    struct drm_i915_gem_object *lmem, u64 lmem_offset,
		    u64 size, bool save)
{
	u64 chunk_size, chunk_offset = 0;
	int ret;

	if (lmem_offset + size > lmem->base.size)
		return -EINVAL;

	if (smem_offset + (size / COMPRESSION_RATIO) > smem->base.size)
		return -EINVAL;

	while (chunk_offset < size) {
		chunk_size = min_t(u64, size - chunk_offset, SZ_64M);

		ret = copy_ccs_chunk(smem, smem_offset + (chunk_offset / COMPRESSION_RATIO),
				     lmem, lmem_offset + chunk_offset, chunk_size, save);
		if (ret)
			break;

		chunk_offset += chunk_size;
	}

	return ret;
}

/**
 * intel_iov_state_save_ccs - Save VF CCS data.
 * @iov: the IOV struct
 * @vfid: VF identifier
 * @smem: SMEM object to save VF CCS data
 * @offset: offset of VF LMEM chunk
 * @size: size of VF LMEM chunk
 *
 * It saves CCS data corresponding to VF's LMEM chunk described with @offset
 * and @size. For each 64KB of LMEM it copies 256B of CCS data.
 *
 * This function is for PF only.
 *
 * Return: Size of data written on success or a negative error code on failure.
 */
ssize_t intel_iov_state_save_ccs(struct intel_iov *iov, u32 vfid,
				 struct drm_i915_gem_object *smem, u64 offset, size_t size)
{
	struct drm_i915_gem_object *lmem;
	int ret;

	lmem = iov->pf.provisioning.configs[vfid].lmem_obj;
	if (!lmem)
		return -ENXIO;

	ret = copy_ccs(smem, 0, lmem, offset, size, true);
	if (ret < 0)
		return ret;

	return size / COMPRESSION_RATIO;
}

/**
 * intel_iov_state_restore_ccs - Restore VF CCS data.
 * @iov: the IOV struct
 * @vfid: VF identifier
 * @smem: SMEM object with VF CCS data to restore
 * @offset: offset of VF LMEM chunk
 * @size: size of VF LMEM chunk
 *
 * It restores CCS data corresponding to VF's LMEM chunk described with @offset
 * and @size. For each 64KB of LMEM it copies 256B of CCS data.
 *
 * This function is for PF only.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int intel_iov_state_restore_ccs(struct intel_iov *iov, u32 vfid,
				struct drm_i915_gem_object *smem, u64 offset, size_t size)
{
	struct drm_i915_gem_object *lmem;
	int ret;

	lmem = iov->pf.provisioning.configs[vfid].lmem_obj;
	if (!lmem)
		return -ENXIO;

	ret = copy_ccs(smem, 0, lmem, offset, size, false);
	if (ret < 0)
		return ret;

	return 0;
}
