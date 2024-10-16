/* SPDX-License-Identifier: GPL-2.0 */

#ifndef selftest
#define selftest(x, y)
#endif

/*
 * List each unit test as selftest(name, function)
 *
 * The name is used as both an enum and expanded as subtest__name to create
 * a module parameter. It must be unique and legal for a C identifier.
 *
 * The function should be of type int function(void). It may be conditionally
 * compiled using #if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST).
 *
 * Tests are executed in order by igt/i915_selftest
 */
selftest(sanitycheck, i915_mock_sanitycheck) /* keep first (igt selfcheck) */
selftest(shmem, shmem_utils_mock_selftests)
selftest(fence, i915_sw_fence_mock_selftests)
selftest(syncmap, i915_syncmap_mock_selftests)
selftest(tlb, intel_tlb_mock_selftests)
selftest(ring, intel_ring_mock_selftests)
selftest(engine, intel_engine_cs_mock_selftests)
selftest(uncore, intel_uncore_mock_selftests)
selftest(iov_relay, selftest_mock_iov_relay)
selftest(iov_service, selftest_mock_iov_service)
