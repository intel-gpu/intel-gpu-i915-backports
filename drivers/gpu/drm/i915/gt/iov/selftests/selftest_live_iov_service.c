// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2022 Intel Corporation. All rights reserved.
 */

#include "gt/iov/abi/iov_actions_abi.h"
#include "gt/iov/abi/iov_version_abi.h"
#include "gt/iov/intel_iov_provisioning.h"

static int handshake(struct intel_iov *iov, u32 major, u32 minor,
		     u32 major_expect, u32 minor_expect, bool ignore_vers_match)
{
	u32 request[VF2PF_HANDSHAKE_REQUEST_MSG_LEN] = {
		FIELD_PREP(GUC_HXG_MSG_0_ORIGIN, GUC_HXG_ORIGIN_HOST) |
		FIELD_PREP(GUC_HXG_MSG_0_TYPE, GUC_HXG_TYPE_REQUEST) |
		FIELD_PREP(GUC_HXG_REQUEST_MSG_0_ACTION, IOV_ACTION_VF2PF_HANDSHAKE),
		FIELD_PREP(VF2PF_HANDSHAKE_REQUEST_MSG_1_MAJOR, major) |
		FIELD_PREP(VF2PF_HANDSHAKE_REQUEST_MSG_1_MINOR, minor),
	};
	u32 response[VF2PF_HANDSHAKE_RESPONSE_MSG_LEN];
	u32 major_resp, minor_resp;
	int ret = 0;

	IOV_DEBUG(iov, "try handshaking %u.%u => %u.%u\n", major, minor, major_expect,
		  minor_expect);

	ret = intel_iov_relay_send_to_pf(&iov->relay, request, ARRAY_SIZE(request), response,
					 ARRAY_SIZE(response));
	if (ret < 0) {
		IOV_SELFTEST_ERROR(iov, "%s %u.%u failed (%pe)", __func__, major, minor,
				   ERR_PTR(ret));
		goto out;
	}

	if (ret != VF2PF_HANDSHAKE_RESPONSE_MSG_LEN) {
		IOV_SELFTEST_ERROR(iov, "%s %u.%u unexpected reply msg len (%d != %u)", __func__,
				   major, minor, ret, VF2PF_HANDSHAKE_RESPONSE_MSG_LEN);
		ret = -EPROTO;
		goto out;
	}

	if (FIELD_GET(VF2PF_HANDSHAKE_RESPONSE_MSG_0_MBZ, response[0])) {
		IOV_SELFTEST_ERROR(iov, "%s %u.%u unexpected reply data (%u != 0)", __func__,
				   major, minor, FIELD_GET(VF2PF_HANDSHAKE_RESPONSE_MSG_0_MBZ,
				   response[0]));
		ret = -EPROTO;
		goto out;
	}

	if (ignore_vers_match)
		return VF2PF_HANDSHAKE_RESPONSE_MSG_LEN;

	major_resp = FIELD_GET(VF2PF_HANDSHAKE_RESPONSE_MSG_1_MAJOR, response[1]);
	minor_resp = FIELD_GET(VF2PF_HANDSHAKE_RESPONSE_MSG_1_MINOR, response[1]);

	if (major != major_expect && minor != minor_expect) {
		IOV_SELFTEST_ERROR(iov, "%s %u.%u unexpected reply version (%u.%u != %u.%u)",
				   __func__, major, minor, major_resp, minor_resp,
				   major_expect, minor_expect);
		ret = -ERANGE;
		goto out;
	}
out:
	return ret < 0 ? ret : 0;
}

static int pf_loopback_handshake_baseline(void *arg)
{
	struct intel_iov *iov = arg;
	u32 ret = 0;

	iov->relay.selftest.disable_strict = 1;
	iov->relay.selftest.enable_loopback = 1;

	ret = handshake(iov, 1, 0, 1, 0, false);

	iov->relay.selftest.disable_strict = 0;
	iov->relay.selftest.enable_loopback = 0;

	return ret;
}

static int pf_loopback_handshake_latest(void *arg)
{
	struct intel_iov *iov = arg;
	u32 ret = 0;

	iov->relay.selftest.disable_strict = 1;
	iov->relay.selftest.enable_loopback = 1;

	ret = handshake(iov, IOV_VERSION_LATEST_MAJOR, IOV_VERSION_LATEST_MINOR,
			IOV_VERSION_LATEST_MAJOR, IOV_VERSION_LATEST_MINOR, false);

	iov->relay.selftest.disable_strict = 0;
	iov->relay.selftest.enable_loopback = 0;

	return ret;
}

static int vf_handshake_query(void *arg)
{
	struct intel_iov *iov = arg;
	int ret;

	ret = handshake(iov, 0, 0, IOV_VERSION_LATEST_MAJOR, IOV_VERSION_LATEST_MINOR, true);

	return ret;
}

static int vf_handshake_fallback_minor(void *arg)
{
	struct intel_iov *iov = arg;
	int ret;

	ret = handshake(iov, IOV_VERSION_LATEST_MAJOR, IOV_VERSION_LATEST_MINOR + 1,
			IOV_VERSION_LATEST_MAJOR, IOV_VERSION_LATEST_MINOR, false);

	if (!ret) {
		IOV_SELFTEST_ERROR(iov, "handshake failed %pe\n", ERR_PTR(ret));
		return -ENODATA;
	}

	return ret;
}

static int vf_handshake_fallback_major_minor(void *arg)
{
	struct intel_iov *iov = arg;
	int ret;

	ret = handshake(iov, IOV_VERSION_LATEST_MAJOR + 1, IOV_VERSION_LATEST_MINOR + 1,
			IOV_VERSION_LATEST_MAJOR, IOV_VERSION_LATEST_MINOR, false);

	if (!ret) {
		IOV_SELFTEST_ERROR(iov, "handshake failed %pe\n", ERR_PTR(ret));
		return -ENODATA;
	}

	return ret;
}

int selftest_live_iov_service(struct drm_i915_private *i915)
{
	static const struct i915_subtest pf_tests[] = {
		SUBTEST(pf_loopback_handshake_baseline),
		SUBTEST(pf_loopback_handshake_latest),
	};
	static const struct i915_subtest vf_tests[] = {
		SUBTEST(vf_handshake_query),
		SUBTEST(vf_handshake_fallback_minor),
		SUBTEST(vf_handshake_fallback_major_minor),
	};
	intel_wakeref_t wakeref;
	int err = 0;

	if (!IS_SRIOV(i915))
		return 0;

	if (IS_SRIOV_PF(i915) && i915_sriov_pf_status(i915) < 0)
		return -EHOSTDOWN;

	with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
		struct intel_gt *gt;
		unsigned int id;

		for_each_gt(gt, i915, id) {
			struct intel_iov *iov = &gt->iov;

			if (IS_SRIOV_PF(i915))
				err = intel_iov_live_subtests(pf_tests, iov);
			else if (IS_SRIOV_VF(i915))
				err = intel_iov_live_subtests(vf_tests, iov);
			if (err)
				break;
		}
	}

	return err;
}
