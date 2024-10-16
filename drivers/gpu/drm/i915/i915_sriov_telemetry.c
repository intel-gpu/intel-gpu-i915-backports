// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include "gt/intel_gt.h"
#include "gt/iov/abi/iov_actions_abi.h"
#include "gt/iov/intel_iov_relay.h"
#include "gt/iov/intel_iov_utils.h"
#include "i915_sriov_telemetry.h"

#define MAKE_SRIOV_TELEMETRY_KLV(__K) \
	(FIELD_PREP(GUC_KLV_0_KEY, IOV_KLV_TELEMETRY_##__K##_KEY) | \
	 FIELD_PREP(GUC_KLV_0_LEN, IOV_KLV_TELEMETRY_##__K##_LEN))

/**
 * i915_sriov_telemetry_is_enabled - Check if telemetry is enabled.
 * @i915: the i915 struct
 *
 * Return: true if telemetry is enabled, false otherwise.
 */
bool i915_sriov_telemetry_is_enabled(struct drm_i915_private *i915)
{
	return i915->params.enable_sriov_telemetry;
}

/**
 * i915_sriov_telemetry_pf_init - Initialize telemetry on PF.
 * @i915: the i915 struct
 *
 * VFs telemetry requires data to be stored on the PF. Allocate flexible
 * structures to hold all required information for every possible VF.
 *
 * This function can only be called on PF.
 */
void i915_sriov_telemetry_pf_init(struct drm_i915_private *i915)
{
	struct i915_sriov_telemetry_pf *telemetry = &i915->sriov.pf.telemetry;
	struct i915_sriov_telemetry_data *data;

	GEM_BUG_ON(!IS_SRIOV_PF(i915));
	GEM_BUG_ON(telemetry->data);

	if (!i915_sriov_telemetry_is_enabled(i915))
		return;

	if (!IS_DG2(i915)) {
		i915->params.enable_sriov_telemetry = false;
		drm_dbg(&i915->drm, "Disabling telemetry, as it's not supported on this platform\n");
		return;
	}

	data = kcalloc(1 + i915_sriov_pf_get_totalvfs(i915), sizeof(*data), GFP_KERNEL);
	if (unlikely(!data)) {
		i915->params.enable_sriov_telemetry = false;
		drm_notice(&i915->drm, "Telemetry initialization failed (%pe)\n",
			   ERR_PTR(-ENOMEM));
		return;
	}

	telemetry->data = data;
}

/**
 * i915_sriov_telemetry_pf_release - Release PF resources used for telemetry.
 * @i915: the i915 struct
 *
 * Release all PF telemetry resources configured during initialization.
 *
 * This function can only be called on PF.
 */
void i915_sriov_telemetry_pf_release(struct drm_i915_private *i915)
{
	GEM_BUG_ON(!IS_SRIOV_PF(i915));

	if (!i915_sriov_telemetry_is_enabled(i915))
		return;

	kfree(fetch_and_zero(&i915->sriov.pf.telemetry.data));
}

static const char *telemetry_key_to_string(u16 key)
{
	switch (key) {
	case IOV_KLV_TELEMETRY_LMEM_ALLOC_KEY:
		return "TELEMETRY_LMEM_ALLOC_KEY";
	default:
		return "<invalid>";
	}
}

static int telemetry_handle_lmem_alloc_key(struct drm_i915_private *i915, u32 vfid,
					   u16 received_klv_len, const u32 *data)
{
	struct i915_sriov_telemetry_data *vf_data = &i915->sriov.pf.telemetry.data[vfid];

	if (received_klv_len != IOV_KLV_TELEMETRY_LMEM_ALLOC_LEN)
		return -EPROTO;

	vf_data->lmem_alloc_size = make_u64(FIELD_GET(GUC_KLV_n_VALUE, data[1]),
					    FIELD_GET(GUC_KLV_n_VALUE, data[0]));

	drm_dbg(&i915->drm, "received %s from VF%u, value: %llu\n",
		telemetry_key_to_string(IOV_KLV_TELEMETRY_LMEM_ALLOC_KEY),
		vfid, vf_data->lmem_alloc_size);

	return 0;
}

/**
 * i915_sriov_telemetry_pf_process_data - Process received telemetry data.
 * @i915: the i915 struct
 * @vfid: VF identifier
 * @count: reported number of KLVs
 * @data: KLV based telemetry data
 * @len: length of the telemetry data (in dwords)
 *
 * Process telemetry data received from VF and save it in internal structures.
 *
 * This function can only be called on PF.
 */
int i915_sriov_telemetry_pf_process_data(struct drm_i915_private *i915, u32 vfid, u16 count,
					 const u32 *data, u32 len)
{
	u16 klv_key, klv_len, pos = 0, received_klvs = 0;
	int err = 0;

	GEM_BUG_ON(!IS_SRIOV_PF(i915));

	if (!i915_sriov_telemetry_is_enabled(i915))
		return 0;

	while (pos < len) {
		klv_key = FIELD_GET(GUC_KLV_0_KEY, data[pos]);
		klv_len = FIELD_GET(GUC_KLV_0_LEN, data[pos++]);

		if (unlikely(pos + klv_len > len))
			return -EPROTO;

		switch (klv_key) {
		case IOV_KLV_TELEMETRY_LMEM_ALLOC_KEY:
			err = telemetry_handle_lmem_alloc_key(i915, vfid, klv_len, &data[pos]);
			break;
		default:
			drm_dbg(&i915->drm, "received unexpected telemetry key from VF%u: %#x\n",
				vfid, klv_key);
			break;
		}

		if (unlikely(err < 0))
			return err;

		pos += klv_len;
		received_klvs++;
	}

	if (count != received_klvs)
		drm_dbg(&i915->drm, "reported number of telemetry KLVs: %u differs from the actually received: %u\n",
			count, received_klvs);

	return 0;
}

/**
 * i915_sriov_telemetry_pf_get_lmem_alloc_size - Get VF LMEM allocated size.
 * @i915: the i915 struct
 * @vfid: VF identifier
 *
 * This function can only be called on PF.
 */
u64 i915_sriov_telemetry_pf_get_lmem_alloc_size(struct drm_i915_private *i915, u32 vfid)
{
	struct i915_sriov_telemetry_pf *telemetry = &i915->sriov.pf.telemetry;

	GEM_BUG_ON(!IS_SRIOV_PF(i915));
	GEM_BUG_ON(!telemetry->data);

	return telemetry->data[vfid].lmem_alloc_size;
}

/**
 * i915_sriov_telemetry_pf_reset - Reset telemetry data for VF.
 * @i915: the i915 struct
 * @vfid: VF identifier
 *
 * This function can only be called on PF.
 */
void i915_sriov_telemetry_pf_reset(struct drm_i915_private *i915, u32 vfid)
{
	struct i915_sriov_telemetry_pf *telemetry = &i915->sriov.pf.telemetry;

	GEM_BUG_ON(!IS_SRIOV_PF(i915));

	if (!i915_sriov_telemetry_is_enabled(i915))
		return;

	GEM_BUG_ON(!telemetry->data);

	telemetry->data[vfid].lmem_alloc_size = 0;
}

static u64 get_lmem_total(struct drm_i915_private *i915)
{
	struct intel_memory_region *mr;
	u64 value = 0;
	int id;

	for_each_memory_region(mr, i915, id)
		if (mr->type == INTEL_MEMORY_LOCAL)
			value += mr->total;

	return value;
}

static void cache_lmem_total(struct drm_i915_private *i915)
{
	GEM_BUG_ON(!IS_SRIOV_VF(i915));

	i915->sriov.vf.telemetry.cached.lmem_total_size	= get_lmem_total(i915);
}

static u64 get_lmem_total_cached(struct drm_i915_private *i915)
{
	GEM_BUG_ON(!IS_SRIOV_VF(i915));

	return i915->sriov.vf.telemetry.cached.lmem_total_size;
}

static u64 get_lmem_avail(struct drm_i915_private *i915)
{
	struct intel_memory_region *mr;
	u64 value = 0;
	int id;

	for_each_memory_region(mr, i915, id)
		if (mr->type == INTEL_MEMORY_LOCAL)
			value += atomic64_read(&mr->avail);

	return value;
}

static u64 get_lmem_allocated(struct drm_i915_private *i915)
{
	GEM_BUG_ON(!IS_SRIOV_VF(i915));

	if (!get_lmem_total_cached(i915))
		cache_lmem_total(i915);

	return get_lmem_total_cached(i915) - get_lmem_avail(i915);
}

static int vf_telemetry_send(struct intel_iov *iov)
{
	struct drm_i915_private *i915 = iov_to_i915(iov);
	u32 msg[VF2PF_TELEMETRY_REPORT_EVENT_MSG_MIN_LEN + 1];
	u32 n = 0, klvs_count = 0;
	u64 data;

	msg[n++] = FIELD_PREP(GUC_HXG_MSG_0_ORIGIN, GUC_HXG_ORIGIN_HOST) |
		   FIELD_PREP(GUC_HXG_MSG_0_TYPE, GUC_HXG_TYPE_EVENT) |
		   FIELD_PREP(GUC_HXG_REQUEST_MSG_0_ACTION, IOV_ACTION_VF2PF_TELEMETRY_REPORT);

	data = get_lmem_allocated(i915);

	msg[n++] = MAKE_SRIOV_TELEMETRY_KLV(LMEM_ALLOC);
	msg[n++] = lower_32_bits(data);
	msg[n++] = upper_32_bits(data);

	klvs_count++;

	msg[0] |= FIELD_PREP(VF2PF_TELEMETRY_REPORT_EVENT_MSG_0_COUNT, klvs_count);

	return intel_iov_relay_send_to_pf(&iov->relay, msg, n, NULL, 0);
}

static void vf_telemetry_timer_callback(struct timer_list *timer)
{
	struct i915_sriov_telemetry_vf *telemetry = from_timer(telemetry, timer, timer);

	queue_work(system_unbound_wq, &telemetry->worker);
	mod_timer(&telemetry->timer, jiffies + msecs_to_jiffies(telemetry->rate));
}

static void telemetry_worker_func(struct work_struct *w)
{
	struct drm_i915_private *i915 = container_of(w, struct drm_i915_private,
						     sriov.vf.telemetry.worker);
	int ret;

	ret = vf_telemetry_send(&to_gt(i915)->iov);
	if (ret < 0)
		dev_dbg(i915->drm.dev, "Error during telemetry data sending (%pe)\n",
			ERR_PTR(ret));
}

#define I915_SRIOV_TELEMETRY_RATE	1000UL

/**
 * i915_sriov_telemetry_vf_init - Initialize telemetry on VF.
 * @i915: the i915 struct
 *
 * Initialize resources needed to provide telemetry data to PF periodically.
 *
 * This function can only be called on VF.
 */
void i915_sriov_telemetry_vf_init(struct drm_i915_private *i915)
{
	struct i915_sriov_telemetry_vf *telemetry;

	GEM_BUG_ON(!IS_SRIOV_VF(i915));

	if (!i915_sriov_telemetry_is_enabled(i915))
		return;

	if (!IS_DG2(i915)) {
		i915->params.enable_sriov_telemetry = false;
		return;
	}

	telemetry = &i915->sriov.vf.telemetry;

	telemetry->rate = I915_SRIOV_TELEMETRY_RATE;
	INIT_WORK(&telemetry->worker, telemetry_worker_func);
	timer_setup(&telemetry->timer, vf_telemetry_timer_callback, 0);
}

/**
 * i915_sriov_telemetry_vf_fini - Release VF resources used for telemetry.
 * @i915: the i915 struct
 *
 * Release all VF telemetry resources configured during initialization.
 *
 * This function can only be called on VF.
 */
void i915_sriov_telemetry_vf_fini(struct drm_i915_private *i915)
{
	struct i915_sriov_telemetry_vf *telemetry;

	GEM_BUG_ON(!IS_SRIOV_VF(i915));

	if (!i915_sriov_telemetry_is_enabled(i915))
		return;

	telemetry = &i915->sriov.vf.telemetry;

	del_timer_sync(&telemetry->timer);
	flush_work(&telemetry->worker);
}

/**
 * i915_sriov_telemetry_vf_start - Start periodic telemetry data sending.
 * @i915: the i915 struct
 *
 * This function can only be called on VF.
 */
void i915_sriov_telemetry_vf_start(struct drm_i915_private *i915)
{
	struct i915_sriov_telemetry_vf *telemetry;

	GEM_BUG_ON(!IS_SRIOV_VF(i915));

	if (!i915_sriov_telemetry_is_enabled(i915))
		return;

	telemetry = &i915->sriov.vf.telemetry;

	telemetry->timer.expires = jiffies + msecs_to_jiffies(telemetry->rate);
	add_timer(&telemetry->timer);
}

/**
 * i915_sriov_telemetry_vf_stop - Stop telemetry data sending.
 * @i915: the i915 struct
 *
 * This function can only be called on VF.
 */
void i915_sriov_telemetry_vf_stop(struct drm_i915_private *i915)
{
	struct i915_sriov_telemetry_vf *telemetry;

	GEM_BUG_ON(!IS_SRIOV_VF(i915));

	if (!i915_sriov_telemetry_is_enabled(i915))
		return;

	telemetry = &i915->sriov.vf.telemetry;

	del_timer_sync(&telemetry->timer);
}
