/*
 * Copyright © 2015-2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *   Robert Bragg <robert@sixbynine.org>
 */


/**
 * DOC: i915 Perf Overview
 *
 * Gen graphics supports a large number of performance counters that can help
 * driver and application developers understand and optimize their use of the
 * GPU.
 *
 * This i915 perf interface enables userspace to configure and open a file
 * descriptor representing a stream of GPU metrics which can then be read() as
 * a stream of sample records.
 *
 * The interface is particularly suited to exposing buffered metrics that are
 * captured by DMA from the GPU, unsynchronized with and unrelated to the CPU.
 *
 * Streams representing a single context are accessible to applications with a
 * corresponding drm file descriptor, such that OpenGL can use the interface
 * without special privileges. Access to system-wide metrics requires root
 * privileges by default, unless changed via the dev.i915.perf_event_paranoid
 * sysctl option.
 *
 */

/**
 * DOC: i915 Perf History and Comparison with Core Perf
 *
 * The interface was initially inspired by the core Perf infrastructure but
 * some notable differences are:
 *
 * i915 perf file descriptors represent a "stream" instead of an "event"; where
 * a perf event primarily corresponds to a single 64bit value, while a stream
 * might sample sets of tightly-coupled counters, depending on the
 * configuration.  For example the Gen OA unit isn't designed to support
 * orthogonal configurations of individual counters; it's configured for a set
 * of related counters. Samples for an i915 perf stream capturing OA metrics
 * will include a set of counter values packed in a compact HW specific format.
 * The OA unit supports a number of different packing formats which can be
 * selected by the user opening the stream. Perf has support for grouping
 * events, but each event in the group is configured, validated and
 * authenticated individually with separate system calls.
 *
 * i915 perf stream configurations are provided as an array of u64 (key,value)
 * pairs, instead of a fixed struct with multiple miscellaneous config members,
 * interleaved with event-type specific members.
 *
 * i915 perf doesn't support exposing metrics via an mmap'd circular buffer.
 * The supported metrics are being written to memory by the GPU unsynchronized
 * with the CPU, using HW specific packing formats for counter sets. Sometimes
 * the constraints on HW configuration require reports to be filtered before it
 * would be acceptable to expose them to unprivileged applications - to hide
 * the metrics of other processes/contexts. For these use cases a read() based
 * interface is a good fit, and provides an opportunity to filter data as it
 * gets copied from the GPU mapped buffers to userspace buffers.
 *
 *
 * Issues hit with first prototype based on Core Perf
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * The first prototype of this driver was based on the core perf
 * infrastructure, and while we did make that mostly work, with some changes to
 * perf, we found we were breaking or working around too many assumptions baked
 * into perf's currently cpu centric design.
 *
 * In the end we didn't see a clear benefit to making perf's implementation and
 * interface more complex by changing design assumptions while we knew we still
 * wouldn't be able to use any existing perf based userspace tools.
 *
 * Also considering the Gen specific nature of the Observability hardware and
 * how userspace will sometimes need to combine i915 perf OA metrics with
 * side-band OA data captured via MI_REPORT_PERF_COUNT commands; we're
 * expecting the interface to be used by a platform specific userspace such as
 * OpenGL or tools. This is to say; we aren't inherently missing out on having
 * a standard vendor/architecture agnostic interface by not using perf.
 *
 *
 * For posterity, in case we might re-visit trying to adapt core perf to be
 * better suited to exposing i915 metrics these were the main pain points we
 * hit:
 *
 * - The perf based OA PMU driver broke some significant design assumptions:
 *
 *   Existing perf pmus are used for profiling work on a cpu and we were
 *   introducing the idea of _IS_DEVICE pmus with different security
 *   implications, the need to fake cpu-related data (such as user/kernel
 *   registers) to fit with perf's current design, and adding _DEVICE records
 *   as a way to forward device-specific status records.
 *
 *   The OA unit writes reports of counters into a circular buffer, without
 *   involvement from the CPU, making our PMU driver the first of a kind.
 *
 *   Given the way we were periodically forward data from the GPU-mapped, OA
 *   buffer to perf's buffer, those bursts of sample writes looked to perf like
 *   we were sampling too fast and so we had to subvert its throttling checks.
 *
 *   Perf supports groups of counters and allows those to be read via
 *   transactions internally but transactions currently seem designed to be
 *   explicitly initiated from the cpu (say in response to a userspace read())
 *   and while we could pull a report out of the OA buffer we can't
 *   trigger a report from the cpu on demand.
 *
 *   Related to being report based; the OA counters are configured in HW as a
 *   set while perf generally expects counter configurations to be orthogonal.
 *   Although counters can be associated with a group leader as they are
 *   opened, there's no clear precedent for being able to provide group-wide
 *   configuration attributes (for example we want to let userspace choose the
 *   OA unit report format used to capture all counters in a set, or specify a
 *   GPU context to filter metrics on). We avoided using perf's grouping
 *   feature and forwarded OA reports to userspace via perf's 'raw' sample
 *   field. This suited our userspace well considering how coupled the counters
 *   are when dealing with normalizing. It would be inconvenient to split
 *   counters up into separate events, only to require userspace to recombine
 *   them. For Mesa it's also convenient to be forwarded raw, periodic reports
 *   for combining with the side-band raw reports it captures using
 *   MI_REPORT_PERF_COUNT commands.
 *
 *   - As a side note on perf's grouping feature; there was also some concern
 *     that using PERF_FORMAT_GROUP as a way to pack together counter values
 *     would quite drastically inflate our sample sizes, which would likely
 *     lower the effective sampling resolutions we could use when the available
 *     memory bandwidth is limited.
 *
 *     With the OA unit's report formats, counters are packed together as 32
 *     or 40bit values, with the largest report size being 256 bytes.
 *
 *     PERF_FORMAT_GROUP values are 64bit, but there doesn't appear to be a
 *     documented ordering to the values, implying PERF_FORMAT_ID must also be
 *     used to add a 64bit ID before each value; giving 16 bytes per counter.
 *
 *   Related to counter orthogonality; we can't time share the OA unit, while
 *   event scheduling is a central design idea within perf for allowing
 *   userspace to open + enable more events than can be configured in HW at any
 *   one time.  The OA unit is not designed to allow re-configuration while in
 *   use. We can't reconfigure the OA unit without losing internal OA unit
 *   state which we can't access explicitly to save and restore. Reconfiguring
 *   the OA unit is also relatively slow, involving ~100 register writes. From
 *   userspace Mesa also depends on a stable OA configuration when emitting
 *   MI_REPORT_PERF_COUNT commands and importantly the OA unit can't be
 *   disabled while there are outstanding MI_RPC commands lest we hang the
 *   command streamer.
 *
 *   The contents of sample records aren't extensible by device drivers (i.e.
 *   the sample_type bits). As an example; Sourab Gupta had been looking to
 *   attach GPU timestamps to our OA samples. We were shoehorning OA reports
 *   into sample records by using the 'raw' field, but it's tricky to pack more
 *   than one thing into this field because events/core.c currently only lets a
 *   pmu give a single raw data pointer plus len which will be copied into the
 *   ring buffer. To include more than the OA report we'd have to copy the
 *   report into an intermediate larger buffer. I'd been considering allowing a
 *   vector of data+len values to be specified for copying the raw data, but
 *   it felt like a kludge to being using the raw field for this purpose.
 *
 * - It felt like our perf based PMU was making some technical compromises
 *   just for the sake of using perf:
 *
 *   perf_event_open() requires events to either relate to a pid or a specific
 *   cpu core, while our device pmu related to neither.  Events opened with a
 *   pid will be automatically enabled/disabled according to the scheduling of
 *   that process - so not appropriate for us. When an event is related to a
 *   cpu id, perf ensures pmu methods will be invoked via an inter process
 *   interrupt on that core. To avoid invasive changes our userspace opened OA
 *   perf events for a specific cpu. This was workable but it meant the
 *   majority of the OA driver ran in atomic context, including all OA report
 *   forwarding, which wasn't really necessary in our case and seems to make
 *   our locking requirements somewhat complex as we handled the interaction
 *   with the rest of the i915 driver.
 */

#include <linux/anon_inodes.h>
#include <linux/mman.h>
#include <linux/nospec.h>
#include <linux/sizes.h>
#include <linux/uuid.h>

#include "gem/i915_gem_context.h"
#include "gem/i915_gem_internal.h"
#include "gem/i915_gem_mman.h"
#include "gem/i915_gem_region.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_engine_regs.h"
#include "gt/intel_engine_user.h"
#include "gt/intel_execlists_submission.h"
#include "gt/intel_gpu_commands.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_clock_utils.h"
#include "gt/intel_gt_mcr.h"
#include "gt/intel_gt_regs.h"
#include "gt/intel_lrc.h"
#include "gt/intel_lrc_reg.h"
#include "gt/intel_ring.h"
#include "gt/uc/intel_guc_slpc.h"

#include "i915_drv.h"
#include "i915_mm.h"
#include "i915_perf.h"
#include "i915_perf_oa_regs.h"
#include "i915_perf_stall_cntr.h"

#define OA_TAKEN(tail, head)	(((tail) - (head)) & (stream->oa_buffer.vma->size - 1))
#define OAC_ENABLED(s) (HAS_OAC(s->perf->i915) && s->engine->class == COMPUTE_CLASS)

/**
 * DOC: OA Tail Pointer Race
 *
 * There's a HW race condition between OA unit tail pointer register updates and
 * writes to memory whereby the tail pointer can sometimes get ahead of what's
 * been written out to the OA buffer so far (in terms of what's visible to the
 * CPU).
 *
 * Although this can be observed explicitly while copying reports to userspace
 * by checking for a zeroed report-id field in tail reports, we want to account
 * for this earlier, as part of the oa_buffer_check_unlocked to avoid lots of
 * redundant read() attempts.
 *
 * We workaround this issue in oa_buffer_check_unlocked() by reading the reports
 * in the OA buffer, starting from the tail reported by the HW until we find a
 * report with its first 2 dwords not 0 meaning its previous report is
 * completely in memory and ready to be read. Those dwords are also set to 0
 * once read and the whole buffer is cleared upon OA buffer initialization. The
 * first dword is the reason for this report while the second is the timestamp,
 * making the chances of having those 2 fields at 0 fairly unlikely. A more
 * detailed explanation is available in oa_buffer_check_unlocked().
 *
 * Most of the implementation details for this workaround are in
 * oa_buffer_check_unlocked() and _append_oa_reports()
 *
 * Note for posterity: previously the driver used to define an effective tail
 * pointer that lagged the real pointer by a 'tail margin' measured in bytes
 * derived from %OA_TAIL_MARGIN_NSEC and the configured sampling frequency.
 * This was flawed considering that the OA unit may also automatically generate
 * non-periodic reports (such as on context switch) or the OA unit may be
 * enabled without any periodic sampling.
 */
#define OA_TAIL_MARGIN_NSEC	100000ULL
#define INVALID_TAIL_PTR	0xffffffff

/* The default frequency for checking whether the OA unit has written new
 * reports to the circular OA buffer...
 */
#define DEFAULT_POLL_FREQUENCY_HZ 200
#define DEFAULT_POLL_PERIOD_NS (NSEC_PER_SEC / DEFAULT_POLL_FREQUENCY_HZ)

/* for sysctl proc_dointvec_minmax of dev.i915.perf_stream_paranoid */
u32 i915_perf_stream_paranoid = true;

/* The maximum exponent the hardware accepts is 63 (essentially it selects one
 * of the 64bit timestamp bits to trigger reports from) but there's currently
 * no known use case for sampling as infrequently as once per 47 thousand years.
 *
 * Since the timestamps included in OA reports are only 32bits it seems
 * reasonable to limit the OA exponent where it's still possible to account for
 * overflow in OA report timestamps.
 */
#define OA_EXPONENT_MAX 31

#define INVALID_CTX_ID 0xffffffff

/* On Gen8+ automatically triggered OA reports include a 'reason' field... */
#define OAREPORT_REASON_MASK           0x3f
#define OAREPORT_REASON_MASK_EXTENDED  0x7f
#define OAREPORT_REASON_SHIFT          19
#define OAREPORT_REASON_TIMER          (1<<0)
#define OAREPORT_REASON_CTX_SWITCH     (1<<3)
#define OAREPORT_REASON_CLK_RATIO      (1<<5)

#define HAS_MI_SET_PREDICATE(i915) (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 50))

/* For sysctl proc_dointvec_minmax of i915_oa_max_sample_rate
 *
 * The highest sampling frequency we can theoretically program the OA unit
 * with is always half the timestamp frequency: E.g. 6.25Mhz for Haswell.
 *
 * Initialized just before we register the sysctl parameter.
 */
static int oa_sample_rate_hard_limit;

/* Theoretically we can program the OA unit to sample every 160ns but don't
 * allow that by default unless root...
 *
 * The default threshold of 100000Hz is based on perf's similar
 * kernel.perf_event_max_sample_rate sysctl parameter.
 */
static u32 i915_oa_max_sample_rate = 100000;

/* XXX: beware if future OA HW adds new report formats that the current
 * code assumes all reports have a power-of-two size and ~(size - 1) can
 * be used as a mask to align the OA tail pointer. In some of the
 * formats, R is used to denote reserved field.
 */
static struct i915_oa_format oa_formats[PRELIM_I915_OA_FORMAT_MAX] = {
	[I915_OA_FORMAT_A13]	    = { 0, 64 },
	[I915_OA_FORMAT_A29]	    = { 1, 128 },
	[I915_OA_FORMAT_A13_B8_C8]  = { 2, 128 },
	/* A29_B8_C8 Disallowed as 192 bytes doesn't factor into buffer size */
	[I915_OA_FORMAT_B4_C8]	    = { 4, 64 },
	[I915_OA_FORMAT_A45_B8_C8]  = { 5, 256 },
	[I915_OA_FORMAT_B4_C8_A16]  = { 6, 128 },
	[I915_OA_FORMAT_C4_B8]	    = { 7, 64 },
	[I915_OA_FORMAT_A12]		    = { 0, 64 },
	[I915_OA_FORMAT_A12_B8_C8]	    = { 2, 128 },
	[I915_OA_FORMAT_A32u40_A4u32_B8_C8] = { 5, 256 },
	[I915_OAR_FORMAT_A32u40_A4u32_B8_C8]    = { 5, 256 },
	[I915_OA_FORMAT_A24u40_A14u32_B8_C8]    = { 5, 256 },
	[PRELIM_I915_OAR_FORMAT_A32u40_A4u32_B8_C8]    = { 5, 256 },
	[PRELIM_I915_OA_FORMAT_A24u40_A14u32_B8_C8]    = { 5, 256 },
	[PRELIM_I915_OAM_FORMAT_A2u64_B8_C8]           = { 5, 128, TYPE_OAM },
	[PRELIM_I915_OAR_FORMAT_A36u64_B8_C8]		= { 1, 384, 0, HDR_64_BIT },
	[PRELIM_I915_OAC_FORMAT_A24u64_B8_C8]		= { 1, 320, 0, HDR_64_BIT },
	[PRELIM_I915_OA_FORMAT_A38u64_R2u64_B8_C8]	= { 1, 448, 0, HDR_64_BIT },
	[PRELIM_I915_OAM_FORMAT_A2u64_R2u64_B8_C8]	= { 1, 128, TYPE_OAM, HDR_64_BIT },
	[PRELIM_I915_OAC_FORMAT_A22u32_R2u32_B8_C8]	= { 2, 192, 0, HDR_64_BIT },
	[PRELIM_I915_OAM_FORMAT_MPEC8u64_B8_C8]	= { 1, 192, TYPE_OAM, HDR_64_BIT },
	[PRELIM_I915_OAM_FORMAT_MPEC8u32_B8_C8]	= { 2, 128, TYPE_OAM, HDR_64_BIT },
};

static const u32 dg2_oa_base[] = {
	[PERF_GROUP_OAG] = 0,
	[PERF_GROUP_OAM_0] = 0x13000,
	[PERF_GROUP_OAM_1] = 0x13200,
};

static const u32 pvc_oa_base[] = {
	[PERF_GROUP_OAG] = 0,
	[PERF_GROUP_OAM_0] = 0x13000,
	[PERF_GROUP_OAM_1] = 0x13200,
	[PERF_GROUP_OAM_2] = 0x13400,
};

/* PERF_GROUP_OAG is unused for oa_base, drop it for mtl */
static const u32 mtl_oa_base[] = {
	[PERF_GROUP_OAM_SAMEDIA_0] = 0x393000,
};

#define SAMPLE_OA_REPORT      (1<<0)

/**
 * struct perf_open_properties - for validated properties given to open a stream
 * @sample_flags: `DRM_I915_PERF_PROP_SAMPLE_*` properties are tracked as flags
 * @single_context: Whether a single or all gpu contexts should be monitored
 * @hold_preemption: Whether the preemption is disabled for the filtered
 *                   context
 * @ctx_handle: A gem ctx handle for use with @single_context
 * @metrics_set: An ID for an OA unit metric set advertised via sysfs
 * @oa_format: An OA unit HW report format
 * @oa_periodic: Whether to enable periodic OA unit sampling
 * @oa_period_exponent: The OA unit sampling period is derived from this
 * @engine: The engine (typically rcs0) being monitored by the OA unit
 * @has_sseu: Whether @sseu was specified by userspace
 * @sseu: internal SSEU configuration computed either from the userspace
 *        specified configuration in the opening parameters or a default value
 *        (see get_default_sseu_config())
 * @poll_oa_period: The period in nanoseconds at which the CPU will check for OA
 * data availability
 * @oa_buffer_size_exponent: The OA buffer size is derived from this
 * @notify_num_reports: The poll or read is unblocked when these many reports
 * are captured
 *
 * As read_properties_unlocked() enumerates and validates the properties given
 * to open a stream of metrics the configuration is built up in the structure
 * which starts out zero initialized.
 */
struct perf_open_properties {
	u32 sample_flags;

	u64 single_context:1;
	u64 hold_preemption:1;
	u64 ctx_handle;

	/* OA sampling state */
	int metrics_set;
	int oa_format;
	bool oa_periodic;
	int oa_period_exponent;
	u32 oa_buffer_size_exponent;

	struct intel_engine_cs *engine;

	bool has_sseu;
	struct intel_sseu sseu;

	u64 poll_oa_period;
	u32 notify_num_reports;
};

struct i915_oa_config_bo {
	struct llist_node node;

	struct i915_oa_config *oa_config;
	struct i915_vma *vma;
};

static struct ctl_table_header *sysctl_header;

static enum hrtimer_restart oa_poll_check_timer_cb(struct hrtimer *hrtimer);

static inline u32 _oa_taken(struct i915_perf_stream * stream,
			       u32 tail, u32 head)
{
	u32 size = stream->oa_buffer.vma->size;

	return tail >= head ? tail - head : size - (head - tail);
}

static inline u32 _rewind_tail(struct i915_perf_stream * stream,
			       u32 relative_hw_tail, u32 rewind_delta)
{
	return rewind_delta > relative_hw_tail ?
	       stream->oa_buffer.vma->size - (rewind_delta - relative_hw_tail) :
	       relative_hw_tail - rewind_delta;
}

static size_t max_oa_buffer_size(struct drm_i915_private *i915)
{
	return HAS_OA_BUF_128M(i915) ? SZ_128M : SZ_16M;
}

void i915_oa_config_release(struct kref *ref)
{
	struct i915_oa_config *oa_config =
		container_of(ref, typeof(*oa_config), ref);

	kfree(oa_config->flex_regs);
	kfree(oa_config->b_counter_regs);
	kfree(oa_config->mux_regs);

	kfree_rcu(oa_config, rcu);
}

struct i915_oa_config *
i915_perf_get_oa_config(struct i915_perf *perf, int metrics_set)
{
	struct i915_oa_config *oa_config;

	rcu_read_lock();
	oa_config = idr_find(&perf->metrics_idr, metrics_set);
	if (oa_config)
		oa_config = i915_oa_config_get(oa_config);
	rcu_read_unlock();

	return oa_config;
}

static void free_oa_config_bo(struct i915_oa_config_bo *oa_bo)
{
	i915_oa_config_put(oa_bo->oa_config);
	i915_vma_put(oa_bo->vma);
	kfree(oa_bo);
}

static inline const
struct i915_perf_regs *__oa_regs(struct i915_perf_stream *stream)
{
	return &(stream->oa_buffer.group->regs);
}

static u32 gen12_oa_hw_tail_read(struct i915_perf_stream *stream)
{
	struct intel_uncore *uncore = stream->uncore;

	return intel_uncore_read(uncore, __oa_regs(stream)->oa_tail_ptr) &
	       GEN12_OAG_OATAILPTR_MASK;
}

#define oa_report_header_64bit(__s) \
	((__s)->oa_buffer.format->header == HDR_64_BIT)

static u64 oa_report_id(struct i915_perf_stream *stream, void *report)
{
	return oa_report_header_64bit(stream) ? *(u64 *)report : *(u32 *)report;
}

static u64 oa_report_reason(struct i915_perf_stream *stream, void *report)
{
	return (oa_report_id(stream, report) >> OAREPORT_REASON_SHIFT) & OAREPORT_REASON_MASK_EXTENDED;
}

static void oa_report_id_clear(struct i915_perf_stream *stream, u32 *report)
{
	if (oa_report_header_64bit(stream))
		*(u64 *)report = 0;
	else
		*report = 0;
}

static bool oa_report_ctx_invalid(struct i915_perf_stream *stream, void *report)
{
	return !(oa_report_id(stream, report) &
	       stream->perf->gen8_valid_ctx_bit);
}

static u64 oa_timestamp(struct i915_perf_stream *stream, void *report)
{
	return oa_report_header_64bit(stream) ?
		*((u64 *)report + 1) :
		*((u32 *)report + 1);
}

static void oa_timestamp_clear(struct i915_perf_stream *stream, u32 *report)
{
	if (oa_report_header_64bit(stream))
		*(u64 *)&report[2] = 0;
	else
		report[1] = 0;
}

static u32 oa_context_id(struct i915_perf_stream *stream, u32 *report)
{
	u32 ctx_id = oa_report_header_64bit(stream) ? report[4] : report[2];

	return ctx_id & stream->specific_ctx_id_mask;
}

static void oa_context_id_squash(struct i915_perf_stream *stream, u32 *report)
{
	if (oa_report_header_64bit(stream))
		report[4] = INVALID_CTX_ID;
	else
		report[2] = INVALID_CTX_ID;
}

/**
 * oa_buffer_check_unlocked - check for data and update tail ptr state
 * @stream: i915 stream instance
 *
 * This is either called via fops (for blocking reads in user ctx) or the poll
 * check hrtimer (atomic ctx) to check the OA buffer tail pointer and check
 * if there is data available for userspace to read.
 *
 * This function is central to providing a workaround for the OA unit tail
 * pointer having a race with respect to what data is visible to the CPU.
 * It is responsible for reading tail pointers from the hardware and giving
 * the pointers time to 'age' before they are made available for reading.
 * (See description of OA_TAIL_MARGIN_NSEC above for further details.)
 *
 * Besides returning true when there is data available to read() this function
 * also updates the tail in the oa_buffer object.
 *
 * Note: It's safe to read OA config state here unlocked, assuming that this is
 * only called while the stream is enabled, while the global OA configuration
 * can't be modified.
 *
 * Returns: %true if the OA buffer contains data, else %false
 */
static bool oa_buffer_check_unlocked(struct i915_perf_stream *stream)
{
	u32 gtt_offset = i915_ggtt_offset(stream->oa_buffer.vma);
	int report_size = stream->oa_buffer.format->size;
	u32 head, tail, read_tail;
	unsigned long flags;
	u32 available;
	bool pollin;
	u32 hw_tail;
	u32 partial_report_size;

	/* We have to consider the (unlikely) possibility that read() errors
	 * could result in an OA buffer reset which might reset the head and
	 * tail state.
	 */
	spin_lock_irqsave(&stream->oa_buffer.ptr_lock, flags);

	hw_tail = stream->perf->ops.oa_hw_tail_read(stream);

	/* The tail pointer increases in 64 byte increments, not in report_size
	 * steps. Also the report size may not be a power of 2. Compute
	 * potentially partially landed report in the OA buffer
	 */
	partial_report_size =
		_oa_taken(stream, hw_tail, stream->oa_buffer.tail);
	partial_report_size %= report_size;

	/* Subtract partial amount off the tail */
	hw_tail = _rewind_tail(stream,
			       hw_tail - gtt_offset,
			       partial_report_size);

	/* NB: The head we observe here might effectively be a little
	 * out of date. If a read() is in progress, the head could be
	 * anywhere between this head and stream->oa_buffer.tail.
	 */
	head = stream->oa_buffer.head - gtt_offset;
	read_tail = stream->oa_buffer.tail - gtt_offset;

	tail = hw_tail;

	/* Walk the stream backward until we find a report with report
	 * id and timestmap not at 0. Since the circular buffer pointers
	 * progress by increments of 64 bytes and that reports can be up
	 * to 256 bytes long, we can't tell whether a report has fully
	 * landed in memory before the report id and timestamp of the
	 * following report have effectively landed.
	 *
	 * This is assuming that the writes of the OA unit land in
	 * memory in the order they were written to.
	 * If not : (╯°□°）╯︵ ┻━┻
	 */
	while (_oa_taken(stream, tail, read_tail) >= report_size) {
		void *report = stream->oa_buffer.vaddr + tail;

		if (oa_report_id(stream, report) ||
		    oa_timestamp(stream, report))
			break;

		tail = _rewind_tail(stream, tail, report_size);
	}

	if (_oa_taken(stream, hw_tail, tail) > report_size &&
	    __ratelimit(&stream->perf->tail_pointer_race))
		DRM_NOTE("unlanded report(s) head=0x%x "
			 "tail=0x%x hw_tail=0x%x\n",
			 head, tail, hw_tail);

	stream->oa_buffer.tail = gtt_offset + tail;

	available = _oa_taken(stream, stream->oa_buffer.tail,
			      stream->oa_buffer.head);
	pollin = available >= stream->notify_num_reports * report_size;

	spin_unlock_irqrestore(&stream->oa_buffer.ptr_lock, flags);

	return pollin;
}

/**
 * append_oa_status - Appends a status record to a userspace read() buffer.
 * @stream: An i915-perf stream opened for OA metrics
 * @buf: destination buffer given by userspace
 * @count: the number of bytes userspace wants to read
 * @offset: (inout): the current position for writing into @buf
 * @type: The kind of status to report to userspace
 *
 * Writes a status record (such as `DRM_I915_PERF_RECORD_OA_REPORT_LOST`)
 * into the userspace read() buffer.
 *
 * The @buf @offset will only be updated on success.
 *
 * Returns: 0 on success, negative error code on failure.
 */
static int append_oa_status(struct i915_perf_stream *stream,
			    char __user *buf,
			    size_t count,
			    size_t *offset,
			    unsigned int type)
{
	struct drm_i915_perf_record_header header = { type, 0, sizeof(header) };

	if ((count - *offset) < header.size)
		return -ENOSPC;

	if (copy_to_user(buf + *offset, &header, sizeof(header)))
		return -EFAULT;

	(*offset) += header.size;

	return 0;
}

/**
 * append_oa_sample - Copies single OA report into userspace read() buffer.
 * @stream: An i915-perf stream opened for OA metrics
 * @buf: destination buffer given by userspace
 * @count: the number of bytes userspace wants to read
 * @offset: (inout): the current position for writing into @buf
 * @report: A single OA report to (optionally) include as part of the sample
 *
 * The contents of a sample are configured through `DRM_I915_PERF_PROP_SAMPLE_*`
 * properties when opening a stream, tracked as `stream->sample_flags`. This
 * function copies the requested components of a single sample to the given
 * read() @buf.
 *
 * The @buf @offset will only be updated on success.
 *
 * Returns: 0 on success, negative error code on failure.
 */
static int append_oa_sample(struct i915_perf_stream *stream,
			    char __user *buf,
			    size_t count,
			    size_t *offset,
			    const u8 *report)
{
	int report_size = stream->oa_buffer.format->size;
	struct drm_i915_perf_record_header header;
	int report_size_partial;
	u8 *oa_buf_end;

	header.type = DRM_I915_PERF_RECORD_SAMPLE;
	header.pad = 0;
	header.size = stream->sample_size;

	if ((count - *offset) < header.size)
		return -ENOSPC;

	buf += *offset;
	if (copy_to_user(buf, &header, sizeof(header)))
		return -EFAULT;
	buf += sizeof(header);

	oa_buf_end = stream->oa_buffer.vaddr + stream->oa_buffer.vma->size;
	report_size_partial = oa_buf_end - report;

	if (report_size_partial < report_size) {
		if(copy_to_user(buf, report, report_size_partial))
			return -EFAULT;
		buf += report_size_partial;

		if(copy_to_user(buf, stream->oa_buffer.vaddr,
				report_size - report_size_partial))
			return -EFAULT;
	} else if (copy_to_user(buf, report, report_size)) {
		return -EFAULT;
	}

	(*offset) += header.size;

	return 0;
}

/**
 * gen8_append_oa_reports - Copies all buffered OA reports into
 *			    userspace read() buffer.
 * @stream: An i915-perf stream opened for OA metrics
 * @buf: destination buffer given by userspace
 * @count: the number of bytes userspace wants to read
 * @offset: (inout): the current position for writing into @buf
 *
 * Notably any error condition resulting in a short read (-%ENOSPC or
 * -%EFAULT) will be returned even though one or more records may
 * have been successfully copied. In this case it's up to the caller
 * to decide if the error should be squashed before returning to
 * userspace.
 *
 * Note: reports are consumed from the head, and appended to the
 * tail, so the tail chases the head?... If you think that's mad
 * and back-to-front you're not alone, but this follows the
 * Gen PRM naming convention.
 *
 * Returns: 0 on success, negative error code on failure.
 */
static int gen8_append_oa_reports(struct i915_perf_stream *stream,
				  char __user *buf,
				  size_t count,
				  size_t *offset)
{
	struct intel_uncore *uncore = stream->uncore;
	int report_size = stream->oa_buffer.format->size;
	u8 *oa_buf_base = stream->oa_buffer.vaddr;
	u32 gtt_offset = i915_ggtt_offset(stream->oa_buffer.vma);
	size_t start_offset = *offset;
	unsigned long flags;
	u32 head, tail, size;
	int ret = 0;

	if (drm_WARN_ON(&uncore->i915->drm, !stream->enabled))
		return -EIO;

	spin_lock_irqsave(&stream->oa_buffer.ptr_lock, flags);

	head = stream->oa_buffer.head;
	tail = stream->oa_buffer.tail;
	size = stream->oa_buffer.vma->size;

	spin_unlock_irqrestore(&stream->oa_buffer.ptr_lock, flags);

	/*
	 * NB: oa_buffer.head/tail include the gtt_offset which we don't want
	 * while indexing relative to oa_buf_base.
	 */
	head -= gtt_offset;
	tail -= gtt_offset;

	/*
	 * An out of bounds or misaligned head or tail pointer implies a driver
	 * bug since we validate + align the tail pointers we read from the
	 * hardware and we are in full control of the head pointer which should
	 * only be incremented by multiples of the report size.
	 */
	if (drm_WARN_ONCE(&uncore->i915->drm,
			  head > size || tail > size,
			  "Inconsistent OA buffer pointers: head = %u, tail = %u\n",
			  head, tail))
		return -EIO;


	for (/* none */;
	     _oa_taken(stream, tail, head);
	     head = (head + report_size) % size) {
		u8 *report = oa_buf_base + head;
		u32 *report32 = (void *)report;
		u32 ctx_id;
		u64 reason;

		/*
		 * The reason field includes flags identifying what
		 * triggered this specific report (mostly timer
		 * triggered or e.g. due to a context switch).
		 */
		reason = oa_report_reason(stream, report);
		ctx_id = oa_context_id(stream, report32);

		/*
		 * Squash whatever is in the CTX_ID field if it's marked as
		 * invalid to be sure we avoid false-positive, single-context
		 * filtering below...
		 *
		 * Note: that we don't clear the valid_ctx_bit so userspace can
		 * understand that the ID has been squashed by the kernel.
		 *
		 * Update:
		 *
		 * On XEHP platforms the behavior of context id valid bit has
		 * changed compared to prior platforms. To describe this, we
		 * define a few terms:
		 *
		 * context-switch-report: This is a report with the reason type
		 * being context-switch. It is generated when a context switches
		 * out.
		 *
		 * context-valid-bit: A bit that is set in the report ID field
		 * to indicate that a valid context has been loaded.
		 *
		 * gpu-idle: A condition characterized by a
		 * context-switch-report with context-valid-bit set to 0.
		 *
		 * On prior platforms, context-id-valid bit is set to 0 only
		 * when GPU goes idle. In all other reports, it is set to 1.
		 *
		 * On XEHP platforms, context-valid-bit is set to 1 in a context
		 * switch report if a new context switched in. For all other
		 * reports it is set to 0.
		 *
		 * This change in behavior causes an issue with MMIO triggered
		 * reports. MMIO triggered reports have the markers in the
		 * context ID field and the context-valid-bit is 0. The logic
		 * below to squash the context ID would render the report
		 * useless since the user will not be able to find it in the OA
		 * buffer. Since MMIO triggered reports exist only on XEHP,
		 * we should avoid squashing these for XEHP platforms.
		 */

		if (oa_report_ctx_invalid(stream, report) &&
		    GRAPHICS_VER_FULL(stream->engine->i915) < IP_VER(12, 50)) {
			ctx_id = INVALID_CTX_ID;
			oa_context_id_squash(stream, report32);
		}

		/*
		 * NB: For Gen 8 the OA unit no longer supports clock gating
		 * off for a specific context and the kernel can't securely
		 * stop the counters from updating as system-wide / global
		 * values.
		 *
		 * Automatic reports now include a context ID so reports can be
		 * filtered on the cpu but it's not worth trying to
		 * automatically subtract/hide counter progress for other
		 * contexts while filtering since we can't stop userspace
		 * issuing MI_REPORT_PERF_COUNT commands which would still
		 * provide a side-band view of the real values.
		 *
		 * To allow userspace (such as Mesa/GL_INTEL_performance_query)
		 * to normalize counters for a single filtered context then it
		 * needs be forwarded bookend context-switch reports so that it
		 * can track switches in between MI_REPORT_PERF_COUNT commands
		 * and can itself subtract/ignore the progress of counters
		 * associated with other contexts. Note that the hardware
		 * automatically triggers reports when switching to a new
		 * context which are tagged with the ID of the newly active
		 * context. To avoid the complexity (and likely fragility) of
		 * reading ahead while parsing reports to try and minimize
		 * forwarding redundant context switch reports (i.e. between
		 * other, unrelated contexts) we simply elect to forward them
		 * all.
		 *
		 * We don't rely solely on the reason field to identify context
		 * switches since it's not-uncommon for periodic samples to
		 * identify a switch before any 'context switch' report.
		 */
		if (!stream->ctx ||
		    stream->specific_ctx_id == ctx_id ||
		    stream->oa_buffer.last_ctx_id == stream->specific_ctx_id ||
		    reason & OAREPORT_REASON_CTX_SWITCH) {

			/*
			 * While filtering for a single context we avoid
			 * leaking the IDs of other contexts.
			 */
			if (stream->ctx &&
			    stream->specific_ctx_id != ctx_id) {
				oa_context_id_squash(stream, report32);
			}

			ret = append_oa_sample(stream, buf, count, offset,
					       report);
			if (ret)
				break;

			stream->oa_buffer.last_ctx_id = ctx_id;
		}

		if (is_power_of_2(report_size)) {
			/*
			 * Clear out the report id and timestamp as a means
			 * to detect unlanded reports.
			 */
			oa_report_id_clear(stream, report32);
			oa_timestamp_clear(stream, report32);
		} else {
			u8 *oa_buf_end = stream->oa_buffer.vaddr +
					 stream->oa_buffer.vma->size;
			u32 part = (u32) ((void *)oa_buf_end - (void *)report32);

			/* Zero out the entire report */
			if (report_size <= part) {
				memset(report32, 0, report_size);
			} else {
				memset(report32, 0, part);
				memset(oa_buf_base, 0, report_size - part);
			}
		}
	}

	if (start_offset != *offset) {
		i915_reg_t oaheadptr = __oa_regs(stream)->oa_head_ptr;

		spin_lock_irqsave(&stream->oa_buffer.ptr_lock, flags);

		/*
		 * We removed the gtt_offset for the copy loop above, indexing
		 * relative to oa_buf_base so put back here...
		 */
		head += gtt_offset;
		intel_uncore_write(uncore, oaheadptr,
				   head & GEN12_OAG_OAHEADPTR_MASK);
		stream->oa_buffer.head = head;

		spin_unlock_irqrestore(&stream->oa_buffer.ptr_lock, flags);
	}

	return ret;
}

/**
 * gen8_oa_read - copy status records then buffered OA reports
 * @stream: An i915-perf stream opened for OA metrics
 * @buf: destination buffer given by userspace
 * @count: the number of bytes userspace wants to read
 * @offset: (inout): the current position for writing into @buf
 *
 * Checks OA unit status registers and if necessary appends corresponding
 * status records for userspace (such as for a buffer full condition) and then
 * initiate appending any buffered OA reports.
 *
 * Updates @offset according to the number of bytes successfully copied into
 * the userspace buffer.
 *
 * NB: some data may be successfully copied to the userspace buffer
 * even if an error is returned, and this is reflected in the
 * updated @offset.
 *
 * Returns: zero on success or a negative error code
 */
static int gen8_oa_read(struct i915_perf_stream *stream,
			char __user *buf,
			size_t count,
			size_t *offset)
{
	struct intel_uncore *uncore = stream->uncore;
	u32 oastatus;
	i915_reg_t oastatus_reg;
	int ret;

	if (drm_WARN_ON(&uncore->i915->drm, !stream->oa_buffer.vaddr))
		return -EIO;

	oastatus_reg = __oa_regs(stream)->oa_status;
	oastatus = intel_uncore_read(uncore, oastatus_reg);

	/*
	 * We treat OABUFFER_OVERFLOW as a significant error:
	 *
	 * Although theoretically we could handle this more gracefully
	 * sometimes, some Gens don't correctly suppress certain
	 * automatically triggered reports in this condition and so we
	 * have to assume that old reports are now being trampled
	 * over.
	 */
	if (oastatus & GEN8_OASTATUS_OABUFFER_OVERFLOW) {
		ret = append_oa_status(stream, buf, count, offset,
				       DRM_I915_PERF_RECORD_OA_BUFFER_LOST);
		if (ret)
			return ret;

		drm_dbg(&stream->perf->i915->drm,
			"OA buffer overflow (exponent = %d): force restart\n",
			stream->period_exponent);

		stream->perf->ops.oa_disable(stream);
		stream->perf->ops.oa_enable(stream);

		/*
		 * Note: .oa_enable() is expected to re-init the oabuffer and
		 * reset GEN8_OASTATUS for us
		 */
		oastatus = intel_uncore_read(uncore, oastatus_reg);
	}

	if (HAS_OA_MMIO_TRIGGER(stream->perf->i915) &&
	    oastatus & XEHPSDV_OAG_OASTATUS_MMIO_TRG_Q_FULL) {
		ret = append_oa_status(stream, buf, count, offset,
				       PRELIM_DRM_I915_PERF_RECORD_OA_MMIO_TRG_Q_FULL);
		if (ret)
			return ret;

		intel_uncore_write(uncore, oastatus_reg, oastatus &
				   ~XEHPSDV_OAG_OASTATUS_MMIO_TRG_Q_FULL);
	}

	if (oastatus & GEN8_OASTATUS_REPORT_LOST) {
		ret = append_oa_status(stream, buf, count, offset,
				       DRM_I915_PERF_RECORD_OA_REPORT_LOST);
		if (ret)
			return ret;

		intel_uncore_rmw(uncore, oastatus_reg,
				 GEN8_OASTATUS_COUNTER_OVERFLOW |
				 GEN8_OASTATUS_REPORT_LOST,
				 0);
	}

	return gen8_append_oa_reports(stream, buf, count, offset);
}

/**
 * i915_oa_wait_unlocked - handles blocking IO until OA data available
 * @stream: An i915-perf stream opened for OA metrics
 *
 * Called when userspace tries to read() from a blocking stream FD opened
 * for OA metrics. It waits until the hrtimer callback finds a non-empty
 * OA buffer and wakes us.
 *
 * Note: it's acceptable to have this return with some false positives
 * since any subsequent read handling will return -EAGAIN if there isn't
 * really data ready for userspace yet.
 *
 * Returns: zero on success or a negative error code
 */
static int i915_oa_wait_unlocked(struct i915_perf_stream *stream)
{
	/* We would wait indefinitely if periodic sampling is not enabled */
	if (!stream->periodic)
		return -EIO;

	return wait_event_interruptible(stream->poll_wq,
					oa_buffer_check_unlocked(stream));
}

/**
 * i915_oa_poll_wait - call poll_wait() for an OA stream poll()
 * @stream: An i915-perf stream opened for OA metrics
 * @file: An i915 perf stream file
 * @wait: poll() state table
 *
 * For handling userspace polling on an i915 perf stream opened for OA metrics,
 * this starts a poll_wait with the wait queue that our hrtimer callback wakes
 * when it sees data ready to read in the circular OA buffer.
 */
static void i915_oa_poll_wait(struct i915_perf_stream *stream,
			      struct file *file,
			      poll_table *wait)
{
	poll_wait(file, &stream->poll_wq, wait);
}

/**
 * i915_oa_read - just calls through to &i915_oa_ops->read
 * @stream: An i915-perf stream opened for OA metrics
 * @buf: destination buffer given by userspace
 * @count: the number of bytes userspace wants to read
 * @offset: (inout): the current position for writing into @buf
 *
 * Updates @offset according to the number of bytes successfully copied into
 * the userspace buffer.
 *
 * Returns: zero on success or a negative error code
 */
static int i915_oa_read(struct i915_perf_stream *stream,
			char __user *buf,
			size_t count,
			size_t *offset)
{
	return stream->perf->ops.read(stream, buf, count, offset);
}

static struct intel_context *oa_pin_context(struct i915_perf_stream *stream)
{
	struct i915_gem_engines_iter it;
	struct i915_gem_context *ctx = stream->ctx;
	struct intel_context *ce;
	struct i915_gem_ww_ctx ww;
	int err = -ENODEV;

	for_each_gem_engine(ce, i915_gem_context_lock_engines(ctx), it) {
		if (ce->engine != stream->engine) /* first match! */
			continue;

		err = 0;
		break;
	}
	i915_gem_context_unlock_engines(ctx);

	if (err)
		return ERR_PTR(err);

	i915_gem_ww_ctx_init(&ww, true);
retry:
	/*
	 * As the ID is the gtt offset of the context's vma we
	 * pin the vma to ensure the ID remains fixed.
	 */
	err = intel_context_pin_ww(ce, &ww);
	if (err == -EDEADLK) {
		err = i915_gem_ww_ctx_backoff(&ww);
		if (!err)
			goto retry;
	}
	i915_gem_ww_ctx_fini(&ww);

	if (err)
		return ERR_PTR(err);

	stream->pinned_ctx = ce;
	return stream->pinned_ctx;
}

static int
__store_reg_to_mem(struct i915_request *rq, i915_reg_t reg, u32 ggtt_offset)
{
	u32 *cs;

	/* GGTT address cannot be transferred unlocked on VF */
	GEM_BUG_ON(IS_SRIOV_VF(rq->i915));

	cs = intel_ring_begin(rq, 4);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	*cs++ = MI_STORE_REGISTER_MEM_GEN8 | MI_SRM_LRM_GLOBAL_GTT;
	*cs++ = i915_mmio_reg_offset(reg);
	*cs++ = ggtt_offset;
	*cs++ = 0;

	intel_ring_advance(rq, cs);

	return 0;
}

static int
__read_reg(struct intel_context *ce, i915_reg_t reg, u32 ggtt_offset)
{
	struct i915_request *rq;
	int err;

	rq = i915_request_create(ce);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	i915_request_get(rq);

	err = __store_reg_to_mem(rq, reg, ggtt_offset);

	i915_request_add(rq);
	if (!err && i915_request_wait(rq, 0, HZ / 2) < 0)
		err = -ETIME;

	i915_request_put(rq);

	return err;
}

static int
gen12_guc_sw_ctx_id(struct intel_context *ce, u32 *ctx_id)
{
	struct i915_vma *scratch;
	u32 *val;
	int err;

	scratch = __vm_create_scratch_for_read_pinned(&ce->engine->gt->ggtt->vm, 4);
	if (IS_ERR(scratch))
		return PTR_ERR(scratch);

	err = i915_vma_sync(scratch);
	if (err)
		goto err_scratch;

	err = __read_reg(ce, RING_EXECLIST_STATUS_HI(ce->engine->mmio_base),
			 i915_ggtt_offset(scratch));
	if (err)
		goto err_scratch;

	val = i915_gem_object_pin_map_unlocked(scratch->obj, I915_MAP_WB);
	if (IS_ERR(val)) {
		err = PTR_ERR(val);
		goto err_scratch;
	}

	*ctx_id = *val;
	i915_gem_object_unpin_map(scratch->obj);

err_scratch:
	i915_vma_unpin_and_release(&scratch, 0);
	return err;
}

/*
 * For execlist mode of submission, pick an unused context id
 * 0 - (NUM_CONTEXT_TAG -1) are used by other contexts
 * XXX_MAX_CONTEXT_HW_ID is used by idle context
 *
 * For GuC mode of submission read context id from the upper dword of the
 * EXECLIST_STATUS register. Note that we read this value only once and expect
 * that the value stays fixed for the entire OA use case. There are cases where
 * GuC KMD implementation may deregister a context to reuse it's context id, but
 * we prevent that from happening to the OA context by pinning it.
 */
static int gen12_get_render_context_id(struct i915_perf_stream *stream)
{
	u32 ctx_id, mask;
	int ret;

	ret = gen12_guc_sw_ctx_id(stream->pinned_ctx, &ctx_id);
	if (ret)
		return ret;

	mask = ((1U << GEN12_GUC_SW_CTX_ID_WIDTH) - 1) << (GEN12_GUC_SW_CTX_ID_SHIFT - 32);
	stream->specific_ctx_id = ctx_id & mask;
	stream->specific_ctx_id_mask = mask;

	return 0;
}

static bool oa_find_reg_in_lri(u32 *state, u32 reg, u32 *offset, u32 end)
{
	u32 idx = *offset;
	u32 len = min(MI_LRI_LEN(state[idx]) + idx, end);
	bool found = false;

	idx++;
	for (; idx < len; idx += 2) {
		if (state[idx] == reg) {
			found = true;
			break;
		}
	}

	*offset = idx;
	return found;
}

static u32 oa_context_image_offset(struct intel_context *ce, u32 reg)
{
	u32 offset, len = (ce->engine->context_size - PAGE_SIZE) / 4;
	u32 *state = ce->lrc_reg_state;

	for (offset = 0; offset < len; ) {
		if (IS_MI_LRI_CMD(state[offset])) {
			/*
			 * We expect reg-value pairs in MI_LRI command, so
			 * MI_LRI_LEN() should be even, if not, issue a warning.
			 */
			drm_WARN_ON(&ce->engine->i915->drm,
				    MI_LRI_LEN(state[offset]) & 0x1);

			if (oa_find_reg_in_lri(state, reg, &offset, len))
				break;
		} else {
			offset++;
		}
	}

	return offset < len ? offset : U32_MAX;
}

static int set_oa_ctx_ctrl_offset(struct intel_context *ce)
{
	i915_reg_t reg = GEN12_OACTXCONTROL(ce->engine->mmio_base);
	struct i915_perf *perf = &ce->engine->i915->perf;
	u16 idx = ce->engine->uabi_class;
	u32 offset = perf->ctx_oactxctrl_offset[idx];

	/* Do this only once. Failure is stored as offset of U32_MAX */
	if (offset)
		goto exit;

	offset = oa_context_image_offset(ce, i915_mmio_reg_offset(reg));
	perf->ctx_oactxctrl_offset[idx] = offset;

	drm_dbg(&ce->engine->i915->drm,
		"%s oa ctx control at 0x%08x dword offset\n",
		ce->engine->name, offset);

exit:
	return offset && offset != U32_MAX ? 0 : -ENODEV;
}

static bool engine_supports_mi_query(struct intel_engine_cs *engine)
{
	return engine->class == RENDER_CLASS ||
	       (engine->class == COMPUTE_CLASS && HAS_OAC(engine->i915));
}

/**
 * oa_get_render_ctx_id - determine and hold ctx hw id
 * @stream: An i915-perf stream opened for OA metrics
 *
 * Determine the render context hw id, and ensure it remains fixed for the
 * lifetime of the stream. This ensures that we don't have to worry about
 * updating the context ID in OACONTROL on the fly.
 *
 * Returns: zero on success or a negative error code
 */
static int oa_get_render_ctx_id(struct i915_perf_stream *stream)
{
	struct intel_context *ce;
	int ret = 0;

	ce = oa_pin_context(stream);
	if (IS_ERR(ce))
		return PTR_ERR(ce);

	if (engine_supports_mi_query(stream->engine)) {
		/*
		 * We are enabling perf query here. If we don't find the context
		 * offset here, just return an error.
		 */
		ret = set_oa_ctx_ctrl_offset(ce);
		if (ret) {
			intel_context_unpin(ce);
			drm_err(&stream->perf->i915->drm,
				"Enabling perf query failed for %s\n",
				stream->engine->name);
			return ret;
		}
	}

	ret = gen12_get_render_context_id(stream);
	ce->tag = stream->specific_ctx_id;

	drm_dbg(&stream->perf->i915->drm,
		"filtering on ctx_id=0x%x ctx_id_mask=0x%x\n",
		stream->specific_ctx_id,
		stream->specific_ctx_id_mask);

	return ret;
}

/**
 * oa_put_render_ctx_id - counterpart to oa_get_render_ctx_id releases hold
 * @stream: An i915-perf stream opened for OA metrics
 *
 * In case anything needed doing to ensure the context HW ID would remain valid
 * for the lifetime of the stream, then that can be undone here.
 */
static void oa_put_render_ctx_id(struct i915_perf_stream *stream)
{
	struct intel_context *ce;

	ce = fetch_and_zero(&stream->pinned_ctx);
	if (ce) {
		ce->tag = 0; /* recomputed on next submission after parking */
		intel_context_unpin(ce);
	}

	stream->specific_ctx_id = INVALID_CTX_ID;
	stream->specific_ctx_id_mask = 0;
}

static void
free_oa_buffer(struct i915_perf_stream *stream)
{
	i915_vma_unpin_and_release(&stream->oa_buffer.vma,
				   I915_VMA_RELEASE_MAP);

	stream->oa_buffer.vaddr = NULL;
}

static void
free_oa_configs(struct i915_perf_stream *stream)
{
	struct i915_oa_config_bo *oa_bo, *tmp;

	i915_oa_config_put(stream->oa_config);
	llist_for_each_entry_safe(oa_bo, tmp, stream->oa_config_bos.first, node)
		free_oa_config_bo(oa_bo);
}

static void
free_noa_wait(struct i915_perf_stream *stream)
{
	i915_vma_unpin_and_release(&stream->noa_wait, 0);
}

/*
 * intel_engine_lookup_user ensures that most of engine specific checks are
 * taken care of, however, we can run into a case where the OA unit catering to
 * the engine passed by the user is disabled for some reason. In such cases,
 * ensure oa unit corresponding to an engine is functional. If there are no
 * engines in the group, the unit is disabled.
 */
static bool oa_unit_functional(const struct intel_engine_cs *engine)
{
	return engine->oa_group && engine->oa_group->num_engines;
}

static bool engine_supports_oa(struct drm_i915_private *i915,
			       const struct intel_engine_cs *engine)
{
	enum intel_platform platform = INTEL_INFO(i915)->platform;

	if (intel_engine_is_virtual(engine))
		return false;

	switch (platform) {
	case INTEL_XEHPSDV:
		return engine->class == COMPUTE_CLASS ||
		       engine->class == VIDEO_DECODE_CLASS ||
		       engine->class == VIDEO_ENHANCEMENT_CLASS;
	case INTEL_DG2:
		return engine->class == RENDER_CLASS ||
		       engine->class == COMPUTE_CLASS ||
		       engine->class == VIDEO_DECODE_CLASS ||
		       engine->class == VIDEO_ENHANCEMENT_CLASS;
	case INTEL_PONTEVECCHIO:
		return engine->class == COMPUTE_CLASS ||
		       engine->class == VIDEO_DECODE_CLASS;
	case INTEL_METEORLAKE:
		return engine->class == RENDER_CLASS;
	default:
		return engine->class == RENDER_CLASS;
	}
}

static bool engine_class_supports_oa_format(struct intel_engine_cs *engine, int type)
{
	switch (engine->class) {
	case RENDER_CLASS:
	case COMPUTE_CLASS:
		return type == TYPE_OAG;
	case VIDEO_DECODE_CLASS:
	case VIDEO_ENHANCEMENT_CLASS:
		return type == TYPE_OAM;
	default:
		return false;
	}
}

static struct i915_whitelist_reg gen12_oa_wl_regs[] = {
	{ GEN12_OAG_OAREPORTTRIG2, RING_FORCE_TO_NONPRIV_ACCESS_RW },
	{ GEN12_OAG_OAREPORTTRIG6, RING_FORCE_TO_NONPRIV_ACCESS_RW },
	{ GEN12_OAG_PERF_COUNTER_A(18), RING_FORCE_TO_NONPRIV_ACCESS_RW |
					RING_FORCE_TO_NONPRIV_RANGE_4 },
	{ GEN12_OAG_OASTATUS, RING_FORCE_TO_NONPRIV_ACCESS_RD |
			      RING_FORCE_TO_NONPRIV_RANGE_4 },
	{ GEN12_OAG_PERF_COUNTER_B(0), RING_FORCE_TO_NONPRIV_ACCESS_RD |
				       RING_FORCE_TO_NONPRIV_RANGE_16 },
};

static struct i915_whitelist_reg xehpsdv_oa_wl_regs[] = {
	{ XEHPSDV_OAG_MMIOTRIGGER, RING_FORCE_TO_NONPRIV_ACCESS_RW },
	{ GEN12_OAG_OASTATUS, RING_FORCE_TO_NONPRIV_ACCESS_RD |
			      RING_FORCE_TO_NONPRIV_RANGE_4 },
	{ GEN12_OAG_PERF_COUNTER_B(0), RING_FORCE_TO_NONPRIV_ACCESS_RD |
				       RING_FORCE_TO_NONPRIV_RANGE_16 },
};

static void __apply_oam_whitelist(struct intel_engine_cs *engine)
{
	struct i915_perf_group *g = engine->oa_group;
	u32 base = g->regs.base;
	struct i915_whitelist_reg oam_wl_regs[] = {
		{ GEN12_OAM_MMIO_TRG(base), RING_FORCE_TO_NONPRIV_ACCESS_RW },
		{ GEN12_OAM_STATUS(base), RING_FORCE_TO_NONPRIV_ACCESS_RD |
					  RING_FORCE_TO_NONPRIV_RANGE_4 },
		{ GEN12_OAM_PERF_COUNTER_B(base, 0), RING_FORCE_TO_NONPRIV_ACCESS_RD |
						     RING_FORCE_TO_NONPRIV_RANGE_16 },
	};

	intel_engine_allow_user_register_access(engine,
						oam_wl_regs,
						ARRAY_SIZE(oam_wl_regs));
}

static void __apply_mmio_trg_whitelist(struct intel_engine_cs *engine)
{
	struct i915_perf_group *g = engine->oa_group;

	if (g->type == TYPE_OAG)
		intel_engine_allow_user_register_access(engine,
							xehpsdv_oa_wl_regs,
							ARRAY_SIZE(xehpsdv_oa_wl_regs));
	else
		__apply_oam_whitelist(engine);
}

static void intel_engine_apply_oa_whitelist(struct intel_engine_cs *engine)
{
	struct drm_i915_private *i915 = engine->i915;
	struct i915_whitelist_reg ctx_id = {
		RING_EXECLIST_STATUS_HI(engine->mmio_base),
		RING_FORCE_TO_NONPRIV_ACCESS_RD
	};

	intel_engine_allow_user_register_access(engine, &ctx_id, 1);

	/*
	 * XEHPSDV_OAG_MMIOTRIGGER need not be added to the SW whitelist
	 * for platforms with graphics version 12.60 and higher(except PVC A0)
	 * as it is added to the built in HW whitelist. Leaving the code as
	 * is for simplicity.
	 */
	if (HAS_OA_MMIO_TRIGGER(i915))
		__apply_mmio_trg_whitelist(engine);
	else
		intel_engine_allow_user_register_access(engine,
							gen12_oa_wl_regs,
							ARRAY_SIZE(gen12_oa_wl_regs));
}

static void perf_group_apply_oa_whitelist(struct i915_perf_group *g)
{
	struct intel_engine_cs *engine;
	intel_engine_mask_t tmp;

	for_each_engine_masked(engine, g->gt, g->engine_mask, tmp)
		intel_engine_apply_oa_whitelist(engine);
}

static void __remove_oam_whitelist(struct intel_engine_cs *engine)
{
	struct i915_perf_group *g = engine->oa_group;
	u32 base = g->regs.base;
	struct i915_whitelist_reg oam_wl_regs[] = {
		{ GEN12_OAM_MMIO_TRG(base), RING_FORCE_TO_NONPRIV_ACCESS_RW },
		{ GEN12_OAM_STATUS(base), RING_FORCE_TO_NONPRIV_ACCESS_RD |
					  RING_FORCE_TO_NONPRIV_RANGE_4 },
		{ GEN12_OAM_PERF_COUNTER_B(base, 0), RING_FORCE_TO_NONPRIV_ACCESS_RD |
						     RING_FORCE_TO_NONPRIV_RANGE_16 },
	};

	intel_engine_deny_user_register_access(engine,
					       oam_wl_regs,
					       ARRAY_SIZE(oam_wl_regs));
}

static void __remove_mmio_trg_whitelist(struct intel_engine_cs *engine)
{
	struct i915_perf_group *g = engine->oa_group;

	if (g->type == TYPE_OAG)
		intel_engine_deny_user_register_access(engine,
						       xehpsdv_oa_wl_regs,
						       ARRAY_SIZE(xehpsdv_oa_wl_regs));
	else
		__remove_oam_whitelist(engine);
}

static void intel_engine_remove_oa_whitelist(struct intel_engine_cs *engine)
{
	struct drm_i915_private *i915 = engine->i915;
	struct i915_whitelist_reg ctx_id = {
		RING_EXECLIST_STATUS_HI(engine->mmio_base),
		RING_FORCE_TO_NONPRIV_ACCESS_RD
	};

	intel_engine_deny_user_register_access(engine, &ctx_id, 1);

	if (HAS_OA_MMIO_TRIGGER(i915))
		__remove_mmio_trg_whitelist(engine);
	else
		intel_engine_deny_user_register_access(engine,
						       gen12_oa_wl_regs,
						       ARRAY_SIZE(gen12_oa_wl_regs));
}

static void perf_group_remove_oa_whitelist(struct i915_perf_group *g)
{
	struct intel_engine_cs *engine;
	intel_engine_mask_t tmp;

	for_each_engine_masked(engine, g->gt, g->engine_mask, tmp)
		intel_engine_remove_oa_whitelist(engine);
}

static void i915_oa_stream_destroy(struct i915_perf_stream *stream)
{
	struct i915_perf *perf = stream->perf;
	struct intel_gt *gt = stream->engine->gt;
	struct i915_perf_group *g = stream->engine->oa_group;

	if (WARN_ON(stream != g->exclusive_stream))
		return;

	if (stream->oa_whitelisted)
		perf_group_remove_oa_whitelist(g);

	/*
	 * Unset exclusive_stream first, it will be checked while disabling
	 * the metric set on gen8+.
	 *
	 * See i915_oa_init_reg_state() and lrc_configure_all_contexts()
	 */
	WRITE_ONCE(g->exclusive_stream, NULL);
	synchronize_rcu(); /* Serialise with i915_oa_init_reg_state */
	perf->ops.disable_metric_set(stream);

	free_oa_buffer(stream);

	/*
	 * Wa_16011777198:dg2: Wa_1509372804:pvc:
	 * Unset the override of GUCRC mode to enable rc6.
	 */
	if (stream->override_gucrc)
		drm_WARN_ON(&gt->i915->drm,
			    intel_guc_slpc_unset_gucrc_mode(&gt->uc.guc.slpc));

	intel_uncore_forcewake_put(stream->uncore, g->fw_domains);
	intel_engine_pm_put(stream->engine);

	if (stream->ctx)
		oa_put_render_ctx_id(stream);

	free_oa_configs(stream);
	free_noa_wait(stream);

	if (perf->spurious_report_rs.missed) {
		DRM_NOTE("%d spurious OA report notices suppressed due to ratelimiting\n",
			 perf->spurious_report_rs.missed);
	}
}

static void gen12_init_oa_buffer(struct i915_perf_stream *stream)
{
	struct intel_uncore *uncore = stream->uncore;
	u32 gtt_offset = i915_ggtt_offset(stream->oa_buffer.vma);
	u32 size_exponent;
	unsigned long flags;

	spin_lock_irqsave(&stream->oa_buffer.ptr_lock, flags);

	intel_uncore_write(uncore, __oa_regs(stream)->oa_status, 0);
	intel_uncore_write(uncore, __oa_regs(stream)->oa_head_ptr,
			   gtt_offset & GEN12_OAG_OAHEADPTR_MASK);
	stream->oa_buffer.head = gtt_offset;

	/*
	 * PRM says:
	 *
	 *  "This MMIO must be set before the OATAILPTR
	 *  register and after the OAHEADPTR register. This is
	 *  to enable proper functionality of the overflow
	 *  bit."
	 *
	 * On XEHPSDV OA buffer size goes up to 128Mb by toggling a bit in the
	 * OAG_OA_DEBUG register meaning multiple base value by 8.
	 */
	size_exponent = (stream->oa_buffer.size_exponent > 24) ?
		(stream->oa_buffer.size_exponent - 20) :
		(stream->oa_buffer.size_exponent - 17);

	intel_uncore_write(uncore, __oa_regs(stream)->oa_buffer, gtt_offset |
			   (size_exponent << GEN12_OAG_OABUFFER_BUFFER_SIZE_SHIFT) |
			   GEN8_OABUFFER_MEM_SELECT_GGTT | GEN7_OABUFFER_EDGE_TRIGGER);
	intel_uncore_write(uncore, __oa_regs(stream)->oa_tail_ptr,
			   gtt_offset & GEN12_OAG_OATAILPTR_MASK);

	/* Mark that we need updated tail pointers to read from... */
	stream->oa_buffer.tail = gtt_offset;

	/*
	 * Reset state used to recognise context switches, affecting which
	 * reports we will forward to userspace while filtering for a single
	 * context.
	 */
	stream->oa_buffer.last_ctx_id = INVALID_CTX_ID;

	spin_unlock_irqrestore(&stream->oa_buffer.ptr_lock, flags);

	/*
	 * NB: although the OA buffer will initially be allocated
	 * zeroed via shmfs (and so this memset is redundant when
	 * first allocating), we may re-init the OA buffer, either
	 * when re-enabling a stream or in error/reset paths.
	 *
	 * The reason we clear the buffer for each re-init is for the
	 * sanity check in gen8_append_oa_reports() that looks at the
	 * reason field to make sure it's non-zero which relies on
	 * the assumption that new reports are being written to zeroed
	 * memory...
	 */
	memset(stream->oa_buffer.vaddr, 0,
	       stream->oa_buffer.vma->size);
}

static int alloc_oa_buffer(struct i915_perf_stream *stream, int size_exponent)
{
	struct intel_gt *gt = stream->engine->gt;
	struct drm_i915_gem_object *bo;
	struct i915_vma *vma;
	size_t size = 1U << size_exponent;
	size_t adjust = 0;
	int ret;

	if (drm_WARN_ON(&gt->i915->drm, stream->oa_buffer.vma))
		return -ENODEV;

	if (WARN_ON(size < SZ_128K || size > max_oa_buffer_size(gt->i915)))
		return -EINVAL;

	bo = i915_gem_object_create_shmem(gt->i915, size - adjust);
	if (IS_ERR(bo)) {
		drm_err(&gt->i915->drm, "Failed to allocate OA buffer\n");
		return PTR_ERR(bo);
	}

	i915_gem_object_set_cache_coherency(bo, I915_CACHE_LLC);

	vma = i915_vma_instance(bo, &gt->ggtt->vm, NULL);
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto err_unref;
	}

	/*
	 * PreHSW required 512K alignment.
	 * HSW and onwards, align to requested size of OA buffer.
	 */
	ret = i915_vma_pin(vma, 0, size, PIN_GLOBAL);
	if (ret) {
		drm_err(&gt->i915->drm, "Failed to pin OA buffer %d\n", ret);
		goto err_unref;
	}

	if (__test_and_clear_bit(GUC_INVALIDATE_TLB, &gt->uc.guc.flags))
		intel_guc_invalidate_tlb_guc(&gt->uc.guc, INTEL_GUC_TLB_INVAL_MODE_HEAVY);

	stream->oa_buffer.vma = vma;
	stream->oa_buffer.size_exponent = size_exponent;

	stream->oa_buffer.vaddr =
		i915_gem_object_pin_map_unlocked(bo, I915_MAP_WB);
	if (IS_ERR(stream->oa_buffer.vaddr)) {
		ret = PTR_ERR(stream->oa_buffer.vaddr);
		goto err_unpin;
	}

	return 0;

err_unpin:
	__i915_vma_unpin(vma);

err_unref:
	i915_gem_object_put(bo);

	stream->oa_buffer.vaddr = NULL;
	stream->oa_buffer.vma = NULL;

	return ret;
}

static u32 *save_restore_register(struct i915_perf_stream *stream, u32 *cs,
				  bool save, i915_reg_t reg, u32 offset,
				  u32 dword_count)
{
	u32 cmd;
	u32 d;

	cmd = save ? MI_STORE_REGISTER_MEM_GEN8 : MI_LOAD_REGISTER_MEM_GEN8;
	cmd |= MI_SRM_LRM_GLOBAL_GTT;

	for (d = 0; d < dword_count; d++) {
		*cs++ = cmd;
		*cs++ = i915_mmio_reg_offset(reg) + 4 * d;
		*cs++ = i915_ggtt_offset(stream->noa_wait) + offset + 4 * d;
		*cs++ = 0;
	}

	return cs;
}

static int alloc_noa_wait(struct i915_perf_stream *stream)
{
	struct drm_i915_private *i915 = stream->perf->i915;
	struct intel_gt *gt = stream->engine->gt;
	struct drm_i915_gem_object *bo;
	struct i915_vma *vma;
	const u64 delay_ticks = 0xffffffffffffffff -
		intel_gt_ns_to_clock_interval(gt,
		atomic64_read(&stream->perf->noa_programming_delay));
	const u32 base = stream->engine->mmio_base;
#define CS_GPR(x) GEN8_RING_CS_GPR(base, x)
	u32 *batch, *ts0, *cs, *jump;
	struct i915_gem_ww_ctx ww;
	int ret, i;
	enum i915_map_type type;
	enum {
		START_TS,
		NOW_TS,
		DELTA_TS,
		JUMP_PREDICATE,
		DELTA_TARGET,
		N_CS_GPR
	};
	intel_wakeref_t wf;
	i915_reg_t mi_predicate_result = HAS_MI_SET_PREDICATE(i915) ?
					  MI_PREDICATE_RESULT_2_ENGINE(base) :
					  MI_PREDICATE_RESULT_1(RENDER_RING_BASE);

	/*
	 * On 2T PVC, iaf driver init puts pressure on the PCIe bus. When
	 * noa wait bo is allocated outside the gt, the batch below runs much
	 * slower and the delay is more than double the intended
	 * noa_programming_delay. Using LMEM in such cases resolves the issue.
	 *
	 * gt->scratch was being used to save/restore the GPR registers, but on
	 * some platforms the scratch used stolen lmem. An MI_SRM to this memory
	 * region caused an engine hang. Instead allocate and additioanl page
	 * here to save/restore GPR registers
	 */
	type = I915_MAP_WC;
	bo = intel_gt_object_create_lmem(gt, 8192, 0);
	if (IS_ERR(bo)) {
		bo = i915_gem_object_create_internal(i915, 8192);
		type = I915_MAP_WB;
	}
	if (IS_ERR(bo)) {
		drm_err(&i915->drm,
			"Failed to allocate NOA wait batchbuffer\n");
		return PTR_ERR(bo);
	}

	wf = intel_gt_pm_get(gt);

	i915_gem_ww_ctx_init(&ww, true);
retry:
	ret = i915_gem_object_lock(bo, &ww);
	if (ret)
		goto out_ww;

	/*
	 * We pin in GGTT because we jump into this buffer now because
	 * multiple OA config BOs will have a jump to this address and it
	 * needs to be fixed during the lifetime of the i915/perf stream.
	 */
	vma = i915_vma_instance(bo, &gt->ggtt->vm, NULL);
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto out_ww;
	}

	ret = i915_vma_pin_ww(vma, 0, 0, PIN_GLOBAL);
	if (ret)
		goto out_ww;

	batch = cs = i915_gem_object_pin_map(bo, type);
	if (IS_ERR(batch)) {
		ret = PTR_ERR(batch);
		goto err_unpin;
	}

	stream->noa_wait = vma;

#define GPR_SAVE_OFFSET 4096
#define PREDICATE_SAVE_OFFSET 4160

	/* Save registers. */
	for (i = 0; i < N_CS_GPR; i++)
		cs = save_restore_register(
			stream, cs, true /* save */, CS_GPR(i),
			GPR_SAVE_OFFSET + 8 * i, 2);
	cs = save_restore_register(
		stream, cs, true /* save */, mi_predicate_result,
		PREDICATE_SAVE_OFFSET, 1);

	/* First timestamp snapshot location. */
	ts0 = cs;

	/*
	 * Initial snapshot of the timestamp register to implement the wait.
	 * We work with 32b values, so clear out the top 32b bits of the
	 * register because the ALU works 64bits.
	 */
	*cs++ = MI_LOAD_REGISTER_IMM(1);
	*cs++ = i915_mmio_reg_offset(CS_GPR(START_TS)) + 4;
	*cs++ = 0;
	*cs++ = MI_LOAD_REGISTER_REG | (3 - 2);
	*cs++ = i915_mmio_reg_offset(RING_TIMESTAMP(base));
	*cs++ = i915_mmio_reg_offset(CS_GPR(START_TS));

	/*
	 * This is the location we're going to jump back into until the
	 * required amount of time has passed.
	 */
	jump = cs;

	/*
	 * Take another snapshot of the timestamp register. Take care to clear
	 * up the top 32bits of CS_GPR(1) as we're using it for other
	 * operations below.
	 */
	*cs++ = MI_LOAD_REGISTER_IMM(1);
	*cs++ = i915_mmio_reg_offset(CS_GPR(NOW_TS)) + 4;
	*cs++ = 0;
	*cs++ = MI_LOAD_REGISTER_REG | (3 - 2);
	*cs++ = i915_mmio_reg_offset(RING_TIMESTAMP(base));
	*cs++ = i915_mmio_reg_offset(CS_GPR(NOW_TS));

	/*
	 * Do a diff between the 2 timestamps and store the result back into
	 * CS_GPR(1).
	 */
	*cs++ = MI_MATH(5);
	*cs++ = MI_MATH_LOAD(MI_MATH_REG_SRCA, MI_MATH_REG(NOW_TS));
	*cs++ = MI_MATH_LOAD(MI_MATH_REG_SRCB, MI_MATH_REG(START_TS));
	*cs++ = MI_MATH_SUB;
	*cs++ = MI_MATH_STORE(MI_MATH_REG(DELTA_TS), MI_MATH_REG_ACCU);
	*cs++ = MI_MATH_STORE(MI_MATH_REG(JUMP_PREDICATE), MI_MATH_REG_CF);

	/*
	 * Transfer the carry flag (set to 1 if ts1 < ts0, meaning the
	 * timestamp have rolled over the 32bits) into the predicate register
	 * to be used for the predicated jump.
	 */
	*cs++ = MI_LOAD_REGISTER_REG | (3 - 2);
	*cs++ = i915_mmio_reg_offset(CS_GPR(JUMP_PREDICATE));
	*cs++ = i915_mmio_reg_offset(mi_predicate_result);

	if (HAS_MI_SET_PREDICATE(i915))
		*cs++ = MI_SET_PREDICATE | 1;

	/* Restart from the beginning if we had timestamps roll over. */
	*cs++ = MI_BATCH_BUFFER_START_GEN8 | MI_BATCH_PREDICATE;
	*cs++ = i915_ggtt_offset(vma) + (ts0 - batch) * 4;
	*cs++ = 0;

	if (HAS_MI_SET_PREDICATE(i915))
		*cs++ = MI_SET_PREDICATE;

	/*
	 * Now add the diff between to previous timestamps and add it to :
	 *      (((1 * << 64) - 1) - delay_ns)
	 *
	 * When the Carry Flag contains 1 this means the elapsed time is
	 * longer than the expected delay, and we can exit the wait loop.
	 */
	*cs++ = MI_LOAD_REGISTER_IMM(2);
	*cs++ = i915_mmio_reg_offset(CS_GPR(DELTA_TARGET));
	*cs++ = lower_32_bits(delay_ticks);
	*cs++ = i915_mmio_reg_offset(CS_GPR(DELTA_TARGET)) + 4;
	*cs++ = upper_32_bits(delay_ticks);

	*cs++ = MI_MATH(4);
	*cs++ = MI_MATH_LOAD(MI_MATH_REG_SRCA, MI_MATH_REG(DELTA_TS));
	*cs++ = MI_MATH_LOAD(MI_MATH_REG_SRCB, MI_MATH_REG(DELTA_TARGET));
	*cs++ = MI_MATH_ADD;
	*cs++ = MI_MATH_STOREINV(MI_MATH_REG(JUMP_PREDICATE), MI_MATH_REG_CF);

	*cs++ = MI_ARB_CHECK;

	/*
	 * Transfer the result into the predicate register to be used for the
	 * predicated jump.
	 */
	*cs++ = MI_LOAD_REGISTER_REG | (3 - 2);
	*cs++ = i915_mmio_reg_offset(CS_GPR(JUMP_PREDICATE));
	*cs++ = i915_mmio_reg_offset(mi_predicate_result);

	if (HAS_MI_SET_PREDICATE(i915))
		*cs++ = MI_SET_PREDICATE | 1;

	/* Predicate the jump.  */
	*cs++ = MI_BATCH_BUFFER_START_GEN8 | MI_BATCH_PREDICATE;
	*cs++ = i915_ggtt_offset(vma) + (jump - batch) * 4;
	*cs++ = 0;

	if (HAS_MI_SET_PREDICATE(i915))
		*cs++ = MI_SET_PREDICATE;

	/* Restore registers. */
	for (i = 0; i < N_CS_GPR; i++)
		cs = save_restore_register(
			stream, cs, false /* restore */, CS_GPR(i),
			GPR_SAVE_OFFSET + 8 * i, 2);
	cs = save_restore_register(
		stream, cs, false /* restore */, mi_predicate_result,
		PREDICATE_SAVE_OFFSET, 1);

	/* And return to the ring. */
	*cs++ = MI_BATCH_BUFFER_END;

	GEM_BUG_ON(cs - batch > PAGE_SIZE / sizeof(*batch));

	i915_gem_object_flush_map(bo);
	__i915_gem_object_release_map(bo);

	goto out_ww;

err_unpin:
	i915_vma_unpin_and_release(&vma, 0);
out_ww:
	if (ret == -EDEADLK) {
		ret = i915_gem_ww_ctx_backoff(&ww);
		if (!ret)
			goto retry;
	}
	i915_gem_ww_ctx_fini(&ww);
	intel_gt_pm_put(gt, wf);
	if (ret)
		i915_gem_object_put(bo);
	return ret;
}

static u32 *write_cs_mi_lri(u32 *cs,
			    const struct i915_oa_reg *reg_data,
			    u32 n_regs)
{
	u32 i;

	for (i = 0; i < n_regs; i++) {
		if ((i % MI_LOAD_REGISTER_IMM_MAX_REGS) == 0) {
			u32 n_lri = min_t(u32,
					  n_regs - i,
					  MI_LOAD_REGISTER_IMM_MAX_REGS);

			*cs++ = MI_LOAD_REGISTER_IMM(n_lri);
		}
		*cs++ = i915_mmio_reg_offset(reg_data[i].addr);
		*cs++ = reg_data[i].value;
	}

	return cs;
}

static int num_lri_dwords(int num_regs)
{
	int count = 0;

	if (num_regs > 0) {
		count += DIV_ROUND_UP(num_regs, MI_LOAD_REGISTER_IMM_MAX_REGS);
		count += num_regs * 2;
	}

	return count;
}

static struct i915_oa_config_bo *
alloc_oa_config_buffer(struct i915_perf_stream *stream,
		       struct i915_oa_config *oa_config)
{
	struct drm_i915_gem_object *obj;
	struct i915_oa_config_bo *oa_bo;
	struct i915_gem_ww_ctx ww;
	size_t config_length = 0;
	u32 *cs;
	int err;

	oa_bo = kzalloc(sizeof(*oa_bo), GFP_KERNEL);
	if (!oa_bo)
		return ERR_PTR(-ENOMEM);

	config_length += num_lri_dwords(oa_config->mux_regs_len);
	config_length += num_lri_dwords(oa_config->b_counter_regs_len);
	config_length += num_lri_dwords(oa_config->flex_regs_len);
	config_length += 3; /* MI_BATCH_BUFFER_START */
	config_length = ALIGN(sizeof(u32) * config_length, I915_GTT_PAGE_SIZE);

	obj = i915_gem_object_create_shmem(stream->perf->i915, config_length);
	if (IS_ERR(obj)) {
		err = PTR_ERR(obj);
		goto err_free;
	}

	i915_gem_ww_ctx_init(&ww, true);
retry:
	err = i915_gem_object_lock(obj, &ww);
	if (err)
		goto out_ww;

	cs = i915_gem_object_pin_map(obj, I915_MAP_WB);
	if (IS_ERR(cs)) {
		err = PTR_ERR(cs);
		goto out_ww;
	}

	cs = write_cs_mi_lri(cs,
			     oa_config->mux_regs,
			     oa_config->mux_regs_len);
	cs = write_cs_mi_lri(cs,
			     oa_config->b_counter_regs,
			     oa_config->b_counter_regs_len);
	cs = write_cs_mi_lri(cs,
			     oa_config->flex_regs,
			     oa_config->flex_regs_len);

	/* Jump into the active wait. */
	*cs++ = MI_BATCH_BUFFER_START_GEN8;
	*cs++ = i915_ggtt_offset(stream->noa_wait);
	*cs++ = 0;

	i915_gem_object_flush_map(obj);
	__i915_gem_object_release_map(obj);

	oa_bo->vma = i915_vma_instance(obj,
				       &stream->engine->gt->ggtt->vm,
				       NULL);
	if (IS_ERR(oa_bo->vma)) {
		err = PTR_ERR(oa_bo->vma);
		goto out_ww;
	}

	oa_bo->oa_config = i915_oa_config_get(oa_config);
	llist_add(&oa_bo->node, &stream->oa_config_bos);

out_ww:
	if (err == -EDEADLK) {
		err = i915_gem_ww_ctx_backoff(&ww);
		if (!err)
			goto retry;
	}
	i915_gem_ww_ctx_fini(&ww);

	if (err)
		i915_gem_object_put(obj);
err_free:
	if (err) {
		kfree(oa_bo);
		return ERR_PTR(err);
	}
	return oa_bo;
}

static struct i915_vma *
get_oa_vma(struct i915_perf_stream *stream, struct i915_oa_config *oa_config)
{
	struct i915_oa_config_bo *oa_bo;

	/*
	 * Look for the buffer in the already allocated BOs attached
	 * to the stream.
	 */
	llist_for_each_entry(oa_bo, stream->oa_config_bos.first, node) {
		if (oa_bo->oa_config == oa_config &&
		    memcmp(oa_bo->oa_config->uuid,
			   oa_config->uuid,
			   sizeof(oa_config->uuid)) == 0)
			goto out;
	}

	oa_bo = alloc_oa_config_buffer(stream, oa_config);
	if (IS_ERR(oa_bo))
		return ERR_CAST(oa_bo);

out:
	return i915_vma_get(oa_bo->vma);
}

static int
emit_oa_config(struct i915_perf_stream *stream,
	       struct i915_oa_config *oa_config,
	       struct intel_context *ce,
	       struct i915_active *active)
{
	struct i915_request *rq;
	struct i915_vma *vma;
	struct i915_gem_ww_ctx ww;
	int err;

	vma = get_oa_vma(stream, oa_config);
	if (IS_ERR(vma))
		return PTR_ERR(vma);

	i915_gem_ww_ctx_init(&ww, true);
retry:
	err = i915_gem_object_lock(vma->obj, &ww);
	if (err)
		goto err;

	err = i915_vma_pin_ww(vma, 0, 0, PIN_GLOBAL);
	if (err)
		goto err;

	intel_engine_pm_get(ce->engine);
	rq = i915_request_create(ce);
	intel_engine_pm_put(ce->engine);
	if (IS_ERR(rq)) {
		err = PTR_ERR(rq);
		goto err_vma_unpin;
	}

	if (!IS_ERR_OR_NULL(active)) {
		/* After all individual context modifications */
		err = i915_request_await_active(rq, active,
						I915_ACTIVE_AWAIT_ACTIVE);
		if (err)
			goto err_add_request;

		err = i915_active_add_request(active, rq);
		if (err)
			goto err_add_request;
	}

	err = i915_request_await_object(rq, vma->obj, 0);
	if (!err)
		err = i915_vma_move_to_active(vma, rq, 0);
	if (err)
		goto err_add_request;

	err = rq->engine->emit_bb_start(rq,
					i915_vma_offset(vma), 0,
					I915_DISPATCH_SECURE);
	if (err)
		goto err_add_request;

err_add_request:
	i915_request_add(rq);
err_vma_unpin:
	i915_vma_unpin(vma);
err:
	if (err == -EDEADLK) {
		err = i915_gem_ww_ctx_backoff(&ww);
		if (!err)
			goto retry;
	}

	i915_gem_ww_ctx_fini(&ww);
	i915_vma_put(vma);
	return err;
}

static struct intel_context *oa_context(struct i915_perf_stream *stream)
{
	return stream->pinned_ctx ?: stream->engine->kernel_context;
}

struct flex {
	i915_reg_t reg;
	u32 offset;
	u32 value;
};

static int
gen8_store_flex(struct i915_request *rq,
		struct intel_context *ce,
		const struct flex *flex, unsigned int count)
{
	u32 offset;
	u32 *cs;

	cs = intel_ring_begin(rq, 4 * count);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	offset = i915_ggtt_offset(ce->state) + LRC_STATE_OFFSET;
	do {
		*cs++ = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT;
		*cs++ = offset + flex->offset * sizeof(u32);
		*cs++ = 0;
		*cs++ = flex->value;
	} while (flex++, --count);

	intel_ring_advance(rq, cs);

	return 0;
}

static int
gen8_load_flex(struct i915_request *rq,
	       struct intel_context *ce,
	       const struct flex *flex, unsigned int count)
{
	u32 *cs;

	GEM_BUG_ON(!count || count > 63);

	cs = intel_ring_begin(rq, 2 * count + 2);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	*cs++ = MI_LOAD_REGISTER_IMM(count);
	do {
		*cs++ = i915_mmio_reg_offset(flex->reg);
		*cs++ = flex->value;
	} while (flex++, --count);
	*cs++ = MI_NOOP;

	intel_ring_advance(rq, cs);

	return 0;
}

static int gen8_modify_context(struct intel_context *ce,
			       const struct flex *flex, unsigned int count)
{
	struct i915_request *rq;
	int err;

	rq = intel_engine_create_kernel_request(ce->engine);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	/* Serialise with the remote context */
	err = 0;
	if (!intel_engine_has_preemption(ce->engine))
		err = intel_context_prepare_remote_request(ce, rq);
	if (err == 0)
		err = gen8_store_flex(rq, ce, flex, count);

	i915_request_add(rq);
	return err;
}

static int
gen8_modify_self(struct intel_context *ce,
		 const struct flex *flex, unsigned int count,
		 struct i915_active *active)
{
	struct i915_request *rq;
	int err;

	intel_engine_pm_get(ce->engine);
	rq = i915_request_create(ce);
	intel_engine_pm_put(ce->engine);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	if (!IS_ERR_OR_NULL(active)) {
		err = i915_active_add_request(active, rq);
		if (err)
			goto err_add_request;
	}

	err = gen8_load_flex(rq, ce, flex, count);
	if (err)
		goto err_add_request;

err_add_request:
	i915_request_set_priority(rq, I915_PRIORITY_BARRIER);
	i915_request_add(rq);
	return err;
}

static u32 gen12_ring_context_control(struct i915_perf_stream *stream,
				      struct i915_active *active)
{
	u32 ring_context_control =
		_MASKED_FIELD(GEN12_CTX_CTRL_OAR_CONTEXT_ENABLE,
			      active ?
			      GEN12_CTX_CTRL_OAR_CONTEXT_ENABLE :
			      0);

	ring_context_control |= HAS_OAC(stream->perf->i915) ?
		_MASKED_FIELD(CTX_CTRL_RUN_ALONE,
			      active ?
			      CTX_CTRL_RUN_ALONE :
			      0) : 0;

	return ring_context_control;
}

static int oa_configure_context(struct intel_context *ce,
				struct flex *regs_ctx, int num_regs_ctx,
				struct flex *regs_lri, int num_regs_lri,
				struct i915_active *active)
{
	int err;

	/* Modify the context image of pinned context with regs_context */
	err = intel_context_lock_pinned(ce);
	if (err)
		return err;

	err = gen8_modify_context(ce, regs_ctx, num_regs_ctx);
	intel_context_unlock_pinned(ce);
	if (err)
		return err;

	/* Apply regs_lri using LRI with pinned context */
	return gen8_modify_self(ce, regs_lri, num_regs_lri, active);
}

static int gen12_configure_oa_render_context(struct i915_perf_stream *stream,
					     struct i915_active *active)
{
	struct intel_context *ce = stream->pinned_ctx;
	u32 format = stream->oa_buffer.format->format;
	u32 offset = stream->perf->ctx_oactxctrl_offset[ce->engine->uabi_class];
	u32 oacontrol = (format << GEN12_OAR_OACONTROL_COUNTER_FORMAT_SHIFT) |
			(active ? GEN12_OAR_OACONTROL_COUNTER_ENABLE : 0);
	struct flex regs_context[] = {
		{
			GEN12_OACTXCONTROL(stream->engine->mmio_base),
			offset + 1,
			active ? GEN8_OA_COUNTER_RESUME : 0,
		},
	};
	struct flex regs_lri[] = {
		{
			GEN12_OAR_OACONTROL, 0,
			oacontrol
		},
		{
			RING_CONTEXT_CONTROL(ce->engine->mmio_base), 0,
			gen12_ring_context_control(stream, active)
		},
	};

	return oa_configure_context(ce,
				    regs_context, ARRAY_SIZE(regs_context),
				    regs_lri, ARRAY_SIZE(regs_lri),
				    active);
}

static u32 __oa_ccs_select(struct i915_perf_stream *stream)
{
	struct intel_engine_cs *engine = stream->engine;

	if (!OAC_ENABLED(stream))
		return 0;

	GEM_BUG_ON(engine->instance > GEN12_OAG_OACONTROL_OA_CCS_SELECT_MASK);

	return engine->instance << GEN12_OAG_OACONTROL_OA_CCS_SELECT_SHIFT;
}

static int gen12_configure_oa_compute_context(struct i915_perf_stream *stream,
					      struct i915_active *active)
{
	struct intel_context *ce = stream->pinned_ctx;
	u32 format = stream->oa_buffer.format->format;
	u32 offset = stream->perf->ctx_oactxctrl_offset[ce->engine->uabi_class];
	u32 oacontrol = (format << GEN12_OAR_OACONTROL_COUNTER_FORMAT_SHIFT) |
			(active ? GEN12_OAR_OACONTROL_COUNTER_ENABLE : 0);
	struct flex regs_context[] = {
		{
			GEN12_OACTXCONTROL(stream->engine->mmio_base),
			offset + 1,
			active ? GEN8_OA_COUNTER_RESUME : 0,
		},
	};
	struct flex regs_lri[] = {
		{
			GEN12_OAC_OACONTROL, 0,
			oacontrol
		},
		{
			RING_CONTEXT_CONTROL(ce->engine->mmio_base), 0,
			gen12_ring_context_control(stream, active)
		},
	};

	/* Set ccs select to enable programming of GEN12_OAC_OACONTROL */
	intel_uncore_write(stream->uncore,
			   __oa_regs(stream)->oa_ctrl,
			   __oa_ccs_select(stream));

	return oa_configure_context(ce,
				    regs_context, ARRAY_SIZE(regs_context),
				    regs_lri, ARRAY_SIZE(regs_lri),
				    active);
}

static int gen12_configure_oa_context(struct i915_perf_stream *stream,
				      struct i915_active *active)
{
	switch (stream->engine->class) {
	case RENDER_CLASS:
		return gen12_configure_oa_render_context(stream, active);
	case COMPUTE_CLASS:
		return HAS_OAC(stream->perf->i915) ?
			gen12_configure_oa_compute_context(stream, active) :
			0;
	default:
		return 0;
	}
}

static u32 oag_configure_mmio_trigger(const struct i915_perf_stream *stream)
{
	if (!HAS_OA_MMIO_TRIGGER(stream->perf->i915))
		return 0;

	return _MASKED_FIELD(XEHPSDV_OAG_OA_DEBUG_DISABLE_MMIO_TRG,
			     (stream->sample_flags & SAMPLE_OA_REPORT) ?
			     0 : XEHPSDV_OAG_OA_DEBUG_DISABLE_MMIO_TRG);
}

static u32 oag_buffer_size_select(const struct i915_perf_stream *stream)
{
	return _MASKED_FIELD(XEHPSDV_OAG_OA_DEBUG_BUFFER_SIZE_SELECT,
			     stream->oa_buffer.size_exponent > 24 ?
			     XEHPSDV_OAG_OA_DEBUG_BUFFER_SIZE_SELECT : 0);
}

static u32 oag_report_ctx_switches(const struct i915_perf_stream *stream)
{
	return _MASKED_FIELD(GEN12_OAG_OA_DEBUG_DISABLE_CTX_SWITCH_REPORTS,
			     (stream->sample_flags & SAMPLE_OA_REPORT) ?
			     0 : GEN12_OAG_OA_DEBUG_DISABLE_CTX_SWITCH_REPORTS);
}

static int
gen12_enable_metric_set(struct i915_perf_stream *stream,
			struct i915_active *active)
{
	struct drm_i915_private *i915 = stream->perf->i915;
	struct intel_uncore *uncore = stream->uncore;
	bool periodic = stream->periodic;
	u32 period_exponent = stream->period_exponent;
	u32 sqcnt1;
	int ret;

	/*
	 * Wa_1508761755:xehpsdv, dg2, pvc
	 * EU NOA signals behave incorrectly if EU clock gating is enabled.
	 * Disable thread stall DOP gating and EU DOP gating.
	 */
	if (IS_PVC_CT_STEP(i915, STEP_A0, STEP_B0) || IS_DG2(i915)) {
		intel_gt_mcr_multicast_write(uncore->gt, GEN8_ROW_CHICKEN,
					     _MASKED_BIT_ENABLE(STALL_DOP_GATING_DISABLE));
		intel_uncore_write(uncore, GEN7_ROW_CHICKEN2,
				   _MASKED_BIT_ENABLE(GEN12_DISABLE_DOP_GATING));
	}

	intel_uncore_write(uncore, __oa_regs(stream)->oa_debug,
			   /* Disable clk ratio reports, like previous Gens. */
			   _MASKED_BIT_ENABLE(GEN12_OAG_OA_DEBUG_DISABLE_CLK_RATIO_REPORTS |
					      GEN12_OAG_OA_DEBUG_INCLUDE_CLK_RATIO) |
			   /*
			    * If the user didn't require OA reports, instruct
			    * the hardware not to emit ctx switch reports.
			    */
			   oag_report_ctx_switches(stream) |

			   /*
			    * Need to set a special bit for OA buffer
			    * sizes > 16Mb on XEHPSDV.
			    */
			   oag_buffer_size_select(stream) |
			   oag_configure_mmio_trigger(stream));

	intel_uncore_write(uncore, __oa_regs(stream)->oa_ctx_ctrl, periodic ?
			   (GEN12_OAG_OAGLBCTXCTRL_COUNTER_RESUME |
			    GEN12_OAG_OAGLBCTXCTRL_TIMER_ENABLE |
			    (period_exponent << GEN12_OAG_OAGLBCTXCTRL_TIMER_PERIOD_SHIFT))
			    : 0);

	/*
	 * Initialize Super Queue Internal Cnt Register
	 * Set PMON Enable in order to collect valid metrics.
	 * Enable byets per clock reporting in OA for XEHPSDV onward.
	 */
	sqcnt1 = GEN12_SQCNT1_PMON_ENABLE |
		 (HAS_OA_BPC_REPORTING(i915) ? GEN12_SQCNT1_OABPC : 0);

	intel_uncore_rmw(uncore, GEN12_SQCNT1, 0, sqcnt1);

	/*
	 * For Gen12, performance counters are context
	 * saved/restored. Only enable it for the context that
	 * requested this.
	 */
	if (stream->ctx) {
		ret = gen12_configure_oa_context(stream, active);
		if (ret)
			return ret;
	}

	return emit_oa_config(stream,
			      stream->oa_config, oa_context(stream),
			      active);
}

static void gen12_disable_metric_set(struct i915_perf_stream *stream)
{
	struct intel_uncore *uncore = stream->uncore;
	struct drm_i915_private *i915 = stream->perf->i915;
	u32 sqcnt1;

	/*
	 * Wa_1508761755:xehpsdv, dg2, pvc
	 * Enable thread stall DOP gating and EU DOP gating.
	 */
	if (IS_PVC_CT_STEP(i915, STEP_A0, STEP_B0) || IS_DG2(i915)) {
		intel_gt_mcr_multicast_write(uncore->gt, GEN8_ROW_CHICKEN,
					     _MASKED_BIT_DISABLE(STALL_DOP_GATING_DISABLE));
		intel_uncore_write(uncore, GEN7_ROW_CHICKEN2,
				   _MASKED_BIT_DISABLE(GEN12_DISABLE_DOP_GATING));
	}

	/* disable the context save/restore or OAR counters */
	if (stream->ctx)
		gen12_configure_oa_context(stream, NULL);

	/* Make sure we disable noa to save power. */
	intel_uncore_rmw(uncore, RPM_CONFIG1, GEN10_GT_NOA_ENABLE, 0);

	sqcnt1 = GEN12_SQCNT1_PMON_ENABLE |
		 (HAS_OA_BPC_REPORTING(i915) ? GEN12_SQCNT1_OABPC : 0);

	/* Reset PMON Enable to save power. */
	intel_uncore_rmw(uncore, GEN12_SQCNT1, sqcnt1, 0);
}

static void gen12_oa_enable(struct i915_perf_stream *stream)
{
	const struct i915_perf_regs *regs = __oa_regs(stream);
	u32 report_format = stream->oa_buffer.format->format;
	u32 val;

	/*
	 * BSpec: 46822
	 * Correct values for OAR counters are still dependent on enabling the
	 * GEN12_OAG_OACONTROL_OA_COUNTER_ENABLE in OAG_OACONTROL. Enabling this
	 * bit means OAG unit will write reports to the OAG buffer, so
	 * initialize the OAG buffer correctly.
	 */
	gen12_init_oa_buffer(stream);

	/*
	 * If OAC is being used, then ccs_select is already programmed. Instead
	 * of a rmw, we reprogram it here with the same value.
	 */
	val = (report_format << regs->oa_ctrl_counter_format_shift) |
	      __oa_ccs_select(stream) | GEN12_OAG_OACONTROL_OA_COUNTER_ENABLE;

	intel_uncore_write(stream->uncore, regs->oa_ctrl, val);
}

/**
 * i915_oa_stream_enable - handle `I915_PERF_IOCTL_ENABLE` for OA stream
 * @stream: An i915 perf stream opened for OA metrics
 *
 * [Re]enables hardware periodic sampling according to the period configured
 * when opening the stream. This also starts a hrtimer that will periodically
 * check for data in the circular OA buffer for notifying userspace (e.g.
 * during a read() or poll()).
 */
static void i915_oa_stream_enable(struct i915_perf_stream *stream)
{
	stream->pollin = false;

	stream->perf->ops.oa_enable(stream);

	if (stream->sample_flags & SAMPLE_OA_REPORT)
		hrtimer_start(&stream->poll_check_timer,
			      ns_to_ktime(stream->poll_oa_period),
			      HRTIMER_MODE_REL_PINNED);
}

static void gen12_oa_disable(struct i915_perf_stream *stream)
{
	struct intel_uncore *uncore = stream->uncore;

	intel_uncore_write(uncore, __oa_regs(stream)->oa_ctrl, 0);
	if (intel_wait_for_register(uncore,
				    __oa_regs(stream)->oa_ctrl,
				    GEN12_OAG_OACONTROL_OA_COUNTER_ENABLE, 0,
				    50))
		drm_err(&stream->perf->i915->drm,
			"wait for OA to be disabled timed out\n");

	if (!HAS_ASID_TLB_INVALIDATION(stream->perf->i915)) {
		intel_uncore_write(uncore, GEN12_OA_TLB_INV_CR, 1);
		if (intel_wait_for_register(uncore,
					    GEN12_OA_TLB_INV_CR,
					    1, 0,
					    50))
			DRM_ERROR("wait for OA tlb invalidate timed out\n");
	}
}

/**
 * i915_oa_stream_disable - handle `I915_PERF_IOCTL_DISABLE` for OA stream
 * @stream: An i915 perf stream opened for OA metrics
 *
 * Stops the OA unit from periodically writing counter reports into the
 * circular OA buffer. This also stops the hrtimer that periodically checks for
 * data in the circular OA buffer, for notifying userspace.
 */
static void i915_oa_stream_disable(struct i915_perf_stream *stream)
{
	stream->perf->ops.oa_disable(stream);

	if (stream->sample_flags & SAMPLE_OA_REPORT)
		hrtimer_cancel(&stream->poll_check_timer);
}

static const struct i915_perf_stream_ops i915_oa_stream_ops = {
	.destroy = i915_oa_stream_destroy,
	.enable = i915_oa_stream_enable,
	.disable = i915_oa_stream_disable,
	.wait_unlocked = i915_oa_wait_unlocked,
	.poll_wait = i915_oa_poll_wait,
	.read = i915_oa_read,
};

static int i915_perf_stream_enable_sync(struct i915_perf_stream *stream)
{
	struct i915_active *active;
	int err;

	active = i915_active_create();
	if (!active)
		return -ENOMEM;

	err = stream->perf->ops.enable_metric_set(stream, active);
	if (err == 0)
		err = __i915_active_wait(active, TASK_KILLABLE);

	i915_active_put(active);
	return err;
}

static void
get_default_sseu_config(struct intel_sseu *out_sseu,
			struct intel_engine_cs *engine)
{
	const struct sseu_dev_info *devinfo_sseu = &engine->gt->info.sseu;

	*out_sseu = intel_sseu_from_device_info(devinfo_sseu);
}

static int
get_sseu_config(struct intel_sseu *out_sseu,
		struct intel_engine_cs *engine,
		const struct drm_i915_gem_context_param_sseu *drm_sseu)
{
	if (drm_sseu->engine.engine_class != engine->uabi_class ||
	    drm_sseu->engine.engine_instance != engine->uabi_instance)
		return -EINVAL;

	return i915_gem_user_to_context_sseu(engine->gt, drm_sseu, out_sseu);
}

/*
 * OA timestamp frequency = CS timestamp frequency in most platforms. On some
 * platforms OA unit ignores the CTC_SHIFT and the 2 timestamps differ. In such
 * cases, return the adjusted CS timestamp frequency to the user.
 */
u32 i915_perf_oa_timestamp_frequency(struct drm_i915_private *i915)
{
	/*
	 * Wa_18013179988:dg2
	 * Wa_14015568240:pvc
	 * Wa_<FIXME>:mtl:
	 */
	if (IS_DG2(i915) || IS_PONTEVECCHIO(i915) || IS_METEORLAKE(i915)) {
		intel_wakeref_t wakeref;
		u32 reg, shift;

		with_intel_runtime_pm(to_gt(i915)->uncore->rpm, wakeref)
			reg = intel_uncore_read(to_gt(i915)->uncore, RPM_CONFIG0);

		shift = REG_FIELD_GET(GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_MASK,
				      reg);

		return to_gt(i915)->clock_frequency << (3 - shift);
	}

	return to_gt(i915)->clock_frequency;
}

/**
 * i915_oa_stream_init - validate combined props for OA stream and init
 * @stream: An i915 perf stream
 * @param: The open parameters passed to `DRM_I915_PERF_OPEN`
 * @props: The property state that configures stream (individually validated)
 *
 * While read_properties_unlocked() validates properties in isolation it
 * doesn't ensure that the combination necessarily makes sense.
 *
 * At this point it has been determined that userspace wants a stream of
 * OA metrics, but still we need to further validate the combined
 * properties are OK.
 *
 * If the configuration makes sense then we can allocate memory for
 * a circular OA buffer and apply the requested metric set configuration.
 *
 * Returns: zero on success or a negative error code.
 */
static int i915_oa_stream_init(struct i915_perf_stream *stream,
			       struct drm_i915_perf_open_param *param,
			       struct perf_open_properties *props)
{
	struct drm_i915_private *i915 = stream->perf->i915;
	struct i915_perf *perf = stream->perf;
	struct i915_perf_group *g;
	struct intel_gt *gt;
	int ret;

	if (!props->engine) {
		drm_dbg(&stream->perf->i915->drm,
			"OA engine not specified\n");
		return -EINVAL;
	}
	gt = props->engine->gt;

	g = props->engine->oa_group;
	if (!g) {
		DRM_DEBUG("Perf group invalid\n");
		return -EINVAL;
	}

	/*
	 * If the sysfs metrics/ directory wasn't registered for some
	 * reason then don't let userspace try their luck with config
	 * IDs
	 */
	if (!perf->metrics_kobj) {
		drm_dbg(&stream->perf->i915->drm,
			"OA metrics weren't advertised via sysfs\n");
		return -EINVAL;
	}

	if (!(props->sample_flags & SAMPLE_OA_REPORT) && !stream->ctx) {
		drm_dbg(&stream->perf->i915->drm,
			"Only OA report sampling supported\n");
		return -EINVAL;
	}

	if (!perf->ops.enable_metric_set) {
		drm_dbg(&stream->perf->i915->drm,
			"OA unit not supported\n");
		return -ENODEV;
	}

	/*
	 * To avoid the complexity of having to accurately filter
	 * counter reports and marshal to the appropriate client
	 * we currently only allow exclusive access
	 */
	if (g->exclusive_stream) {
		drm_dbg(&stream->perf->i915->drm,
			"OA unit already in use\n");
		return -EBUSY;
	}

	stream->notify_num_reports = props->notify_num_reports;
	stream->engine = props->engine;
	stream->uncore = stream->engine->gt->uncore;

	stream->sample_size = sizeof(struct drm_i915_perf_record_header);

	stream->oa_buffer.group = g;
	stream->oa_buffer.format = &perf->oa_formats[props->oa_format];
	if (drm_WARN_ON(&i915->drm, stream->oa_buffer.format->size == 0))
		return -EINVAL;

	stream->sample_flags = props->sample_flags;
	stream->sample_size += stream->oa_buffer.format->size;

	stream->hold_preemption = props->hold_preemption;

	stream->periodic = props->oa_periodic;
	if (stream->periodic)
		stream->period_exponent = props->oa_period_exponent;

	if (stream->ctx) {
		ret = oa_get_render_ctx_id(stream);
		if (ret) {
			drm_dbg(&stream->perf->i915->drm,
				"Invalid context id to filter with\n");
			return ret;
		}
	}

	ret = alloc_noa_wait(stream);
	if (ret) {
		drm_dbg(&stream->perf->i915->drm,
			"Unable to allocate NOA wait batch buffer\n");
		goto err_noa_wait_alloc;
	}

	stream->oa_config = i915_perf_get_oa_config(perf, props->metrics_set);
	if (!stream->oa_config) {
		drm_dbg(&stream->perf->i915->drm,
			"Invalid OA config id=%i\n", props->metrics_set);
		ret = -EINVAL;
		goto err_config;
	}

	/* PRM - observability performance counters:
	 *
	 *   OACONTROL, performance counter enable, note:
	 *
	 *   "When this bit is set, in order to have coherent counts,
	 *   RC6 power state and trunk clock gating must be disabled.
	 *   This can be achieved by programming MMIO registers as
	 *   0xA094=0 and 0xA090[31]=1"
	 *
	 *   In our case we are expecting that taking pm + FORCEWAKE
	 *   references will effectively disable RC6.
	 */
	intel_engine_pm_get(stream->engine);
	intel_uncore_forcewake_get(stream->uncore, g->fw_domains);

	/*
	 * Wa_16011777198:dg2: GuC resets render as part of the Wa. This causes
	 * OA to lose the configuration state. Prevent this by overriding GUCRC
	 * mode.
	 *
	 * Wa_1509372804:pvc: Another bug causes GuC to reset an engine and OA
	 * loses state. Add PVC to the check below.
	 */
	if (intel_uc_uses_guc_rc(&gt->uc) &&
	    (IS_DG2_GRAPHICS_STEP(gt->i915, G10, STEP_A0, STEP_C0) ||
	     IS_DG2_GRAPHICS_STEP(gt->i915, G11, STEP_A0, STEP_B0) ||
	     IS_PVC_CT_STEP(gt->i915, STEP_A0, STEP_C0))) {
		ret = intel_guc_slpc_override_gucrc_mode(&gt->uc.guc.slpc,
							 SLPC_GUCRC_MODE_GUCRC_NO_RC6);
		if (ret) {
			drm_dbg(&stream->perf->i915->drm,
				"Unable to override gucrc mode\n");
			goto err_fw;
		}

		stream->override_gucrc = true;
	}

	ret = alloc_oa_buffer(stream, props->oa_buffer_size_exponent);
	if (ret)
		goto err_gucrc;

	stream->ops = &i915_oa_stream_ops;

	stream->engine->gt->perf.sseu = props->sseu;
	WRITE_ONCE(g->exclusive_stream, stream);

	ret = i915_perf_stream_enable_sync(stream);
	if (ret) {
		drm_dbg(&stream->perf->i915->drm,
			"Unable to enable metric set\n");
		goto err_enable;
	}

	drm_dbg(&stream->perf->i915->drm,
		"opening stream oa config uuid=%s\n",
		  stream->oa_config->uuid);

	hrtimer_init(&stream->poll_check_timer,
		     CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	stream->poll_check_timer.function = oa_poll_check_timer_cb;
	init_waitqueue_head(&stream->poll_wq);
	spin_lock_init(&stream->oa_buffer.ptr_lock);
	mutex_init(&stream->lock);

	return 0;

err_enable:
	WRITE_ONCE(g->exclusive_stream, NULL);
	perf->ops.disable_metric_set(stream);

	free_oa_buffer(stream);

err_gucrc:
	if (stream->override_gucrc)
		intel_guc_slpc_unset_gucrc_mode(&gt->uc.guc.slpc);

err_fw:
	intel_uncore_forcewake_put(stream->uncore, g->fw_domains);
	intel_engine_pm_put(stream->engine);

	free_oa_configs(stream);

err_config:
	free_noa_wait(stream);

err_noa_wait_alloc:
	if (stream->ctx)
		oa_put_render_ctx_id(stream);

	return ret;
}

void i915_oa_init_reg_state(const struct intel_context *ce,
			    const struct intel_engine_cs *engine)
{
}

/**
 * i915_perf_read - handles read() FOP for i915 perf stream FDs
 * @file: An i915 perf stream file
 * @buf: destination buffer given by userspace
 * @count: the number of bytes userspace wants to read
 * @ppos: (inout) file seek position (unused)
 *
 * The entry point for handling a read() on a stream file descriptor from
 * userspace. Most of the work is left to the i915_perf_read_locked() and
 * &i915_perf_stream_ops->read but to save having stream implementations (of
 * which we might have multiple later) we handle blocking read here.
 *
 * We can also consistently treat trying to read from a disabled stream
 * as an IO error so implementations can assume the stream is enabled
 * while reading.
 *
 * Returns: The number of bytes copied or a negative error code on failure.
 */
static ssize_t i915_perf_read(struct file *file,
			      char __user *buf,
			      size_t count,
			      loff_t *ppos)
{
	struct i915_perf_stream *stream = file->private_data;
	size_t offset = 0;
	int ret;

	/* To ensure it's handled consistently we simply treat all reads of a
	 * disabled stream as an error. In particular it might otherwise lead
	 * to a deadlock for blocking file descriptors...
	 */
	if (!stream->enabled || !(stream->sample_flags & SAMPLE_OA_REPORT))
		return -EIO;

	if (!(file->f_flags & O_NONBLOCK)) {
		/* There's the small chance of false positives from
		 * stream->ops->wait_unlocked.
		 *
		 * E.g. with single context filtering since we only wait until
		 * oabuffer has >= 1 report we don't immediately know whether
		 * any reports really belong to the current context
		 */
		do {
			ret = stream->ops->wait_unlocked(stream);
			if (ret)
				return ret;

			mutex_lock(&stream->lock);
			ret = stream->ops->read(stream, buf, count, &offset);
			mutex_unlock(&stream->lock);
		} while (!offset && !ret);
	} else {
		mutex_lock(&stream->lock);
		ret = stream->ops->read(stream, buf, count, &offset);
		mutex_unlock(&stream->lock);
	}

	/* We allow the poll checking to sometimes report false positive EPOLLIN
	 * events where we might actually report EAGAIN on read() if there's
	 * not really any data available. In this situation though we don't
	 * want to enter a busy loop between poll() reporting a EPOLLIN event
	 * and read() returning -EAGAIN. Clearing the oa.pollin state here
	 * effectively ensures we back off until the next hrtimer callback
	 * before reporting another EPOLLIN event.
	 * The exception to this is if ops->read() returned -ENOSPC which means
	 * that more OA data is available than could fit in the user provided
	 * buffer. In this case we want the next poll() call to not block.
	 */
	if (ret != -ENOSPC)
		stream->pollin = false;

	/* Possible values for ret are 0, -EFAULT, -ENOSPC, -EIO, ... */
	return offset ?: (ret ?: -EAGAIN);
}

static enum hrtimer_restart oa_poll_check_timer_cb(struct hrtimer *hrtimer)
{
	struct i915_perf_stream *stream =
		container_of(hrtimer, typeof(*stream), poll_check_timer);

	if (oa_buffer_check_unlocked(stream)) {
		stream->pollin = true;
		wake_up(&stream->poll_wq);
	}

	hrtimer_forward_now(hrtimer,
			    ns_to_ktime(stream->poll_oa_period));

	return HRTIMER_RESTART;
}

/**
 * i915_perf_poll_locked - poll_wait() with a suitable wait queue for stream
 * @stream: An i915 perf stream
 * @file: An i915 perf stream file
 * @wait: poll() state table
 *
 * For handling userspace polling on an i915 perf stream, this calls through to
 * &i915_perf_stream_ops->poll_wait to call poll_wait() with a wait queue that
 * will be woken for new stream data.
 *
 * Returns: any poll events that are ready without sleeping
 */
static __poll_t i915_perf_poll_locked(struct i915_perf_stream *stream,
				      struct file *file,
				      poll_table *wait)
{
	__poll_t events = 0;

	stream->ops->poll_wait(stream, file, wait);

	/* Note: we don't explicitly check whether there's something to read
	 * here since this path may be very hot depending on what else
	 * userspace is polling, or on the timeout in use. We rely solely on
	 * the hrtimer/oa_poll_check_timer_cb to notify us when there are
	 * samples to read.
	 */
	if (stream->pollin)
		events |= EPOLLIN;

	return events;
}

/**
 * i915_perf_poll - call poll_wait() with a suitable wait queue for stream
 * @file: An i915 perf stream file
 * @wait: poll() state table
 *
 * For handling userspace polling on an i915 perf stream, this ensures
 * poll_wait() gets called with a wait queue that will be woken for new stream
 * data.
 *
 * Note: Implementation deferred to i915_perf_poll_locked()
 *
 * Returns: any poll events that are ready without sleeping
 */
static __poll_t i915_perf_poll(struct file *file, poll_table *wait)
{
	struct i915_perf_stream *stream = file->private_data;
	__poll_t ret;

	mutex_lock(&stream->lock);
	ret = i915_perf_poll_locked(stream, file, wait);
	mutex_unlock(&stream->lock);

	return ret;
}

/**
 * i915_perf_enable_locked - handle `I915_PERF_IOCTL_ENABLE` ioctl
 * @stream: A disabled i915 perf stream
 *
 * [Re]enables the associated capture of data for this stream.
 *
 * If a stream was previously enabled then there's currently no intention
 * to provide userspace any guarantee about the preservation of previously
 * buffered data.
 */
static void i915_perf_enable_locked(struct i915_perf_stream *stream)
{
	if (stream->enabled)
		return;

	/* Allow stream->ops->enable() to refer to this */
	stream->enabled = true;

	if (stream->ops->enable)
		stream->ops->enable(stream);

	if (stream->hold_preemption)
		intel_context_set_nopreempt(stream->pinned_ctx);
}

/**
 * i915_perf_disable_locked - handle `I915_PERF_IOCTL_DISABLE` ioctl
 * @stream: An enabled i915 perf stream
 *
 * Disables the associated capture of data for this stream.
 *
 * The intention is that disabling an re-enabling a stream will ideally be
 * cheaper than destroying and re-opening a stream with the same configuration,
 * though there are no formal guarantees about what state or buffered data
 * must be retained between disabling and re-enabling a stream.
 *
 * Note: while a stream is disabled it's considered an error for userspace
 * to attempt to read from the stream (-EIO).
 */
static void i915_perf_disable_locked(struct i915_perf_stream *stream)
{
	if (!stream->enabled)
		return;

	/* Allow stream->ops->disable() to refer to this */
	stream->enabled = false;

	if (stream->hold_preemption)
		intel_context_clear_nopreempt(stream->pinned_ctx);

	if (stream->ops->disable)
		stream->ops->disable(stream);
}

static long i915_perf_config_locked(struct i915_perf_stream *stream,
				    unsigned long metrics_set)
{
	struct i915_oa_config *config;
	long ret = stream->oa_config->id;

	config = i915_perf_get_oa_config(stream->perf, metrics_set);
	if (!config)
		return -EINVAL;

	if (config != stream->oa_config) {
		int err;

		/*
		 * If OA is bound to a specific context, emit the
		 * reconfiguration inline from that context. The update
		 * will then be ordered with respect to submission on that
		 * context.
		 *
		 * When set globally, we use a low priority kernel context,
		 * so it will effectively take effect when idle.
		 */
		err = emit_oa_config(stream, config, oa_context(stream), NULL);
		if (!err)
			config = xchg(&stream->oa_config, config);
		else
			ret = err;
	}

	i915_oa_config_put(config);

	return ret;
}

#define I915_PERF_OA_BUFFER_MMAP_OFFSET 1

/**
 * i915_perf_oa_buffer_info_locked - size and offset of the OA buffer
 * @stream: i915 perf stream
 * @cmd: ioctl command
 * @arg: pointer to oa buffer info filled by this function.
 */
static int i915_perf_oa_buffer_info_locked(struct i915_perf_stream *stream,
					   unsigned int cmd,
					   unsigned long arg)
{
	struct prelim_drm_i915_perf_oa_buffer_info info;
	void __user *output = (void __user *)arg;

	if (i915_perf_stream_paranoid && !perfmon_capable()) {
		DRM_DEBUG("Insufficient privileges to access OA buffer info\n");
		return -EACCES;
	}

	if (_IOC_SIZE(cmd) != sizeof(info))
		return -EINVAL;

	if (copy_from_user(&info, output, sizeof(info)))
		return -EFAULT;

	if (info.type || info.flags || info.rsvd)
		return -EINVAL;

	info.size = stream->oa_buffer.vma->size;
	info.offset = I915_PERF_OA_BUFFER_MMAP_OFFSET * PAGE_SIZE;

	if (copy_to_user(output, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

/**
 * i915_perf_ioctl_locked - support ioctl() usage with i915 perf stream FDs
 * @stream: An i915 perf stream
 * @cmd: the ioctl request
 * @arg: the ioctl data
 *
 * Returns: zero on success or a negative error code. Returns -EINVAL for
 * an unknown ioctl request.
 */
static long i915_perf_ioctl_locked(struct i915_perf_stream *stream,
				   unsigned int cmd,
				   unsigned long arg)
{
	switch (cmd) {
	case I915_PERF_IOCTL_ENABLE:
		i915_perf_enable_locked(stream);
		return 0;
	case I915_PERF_IOCTL_DISABLE:
		i915_perf_disable_locked(stream);
		return 0;
	case I915_PERF_IOCTL_CONFIG:
		return i915_perf_config_locked(stream, arg);
	case PRELIM_I915_PERF_IOCTL_GET_OA_BUFFER_INFO:
		return i915_perf_oa_buffer_info_locked(stream, cmd, arg);
	}

	return -EINVAL;
}

/**
 * i915_perf_ioctl - support ioctl() usage with i915 perf stream FDs
 * @file: An i915 perf stream file
 * @cmd: the ioctl request
 * @arg: the ioctl data
 *
 * Implementation deferred to i915_perf_ioctl_locked().
 *
 * Returns: zero on success or a negative error code. Returns -EINVAL for
 * an unknown ioctl request.
 */
static long i915_perf_ioctl(struct file *file,
			    unsigned int cmd,
			    unsigned long arg)
{
	struct i915_perf_stream *stream = file->private_data;
	long ret;

	mutex_lock(&stream->lock);
	ret = i915_perf_ioctl_locked(stream, cmd, arg);
	mutex_unlock(&stream->lock);

	return ret;
}

/**
 * i915_perf_destroy_locked - destroy an i915 perf stream
 * @stream: An i915 perf stream
 *
 * Frees all resources associated with the given i915 perf @stream, disabling
 * any associated data capture in the process.
 *
 * Note: The &gt->perf.lock mutex has been taken to serialize
 * with any non-file-operation driver hooks.
 */
static void i915_perf_destroy_locked(struct i915_perf_stream *stream)
{
	if (stream->enabled)
		i915_perf_disable_locked(stream);

	if (stream->ops->destroy)
		stream->ops->destroy(stream);

	if (stream->ctx)
		i915_gem_context_put(stream->ctx);

	kfree(stream);
}

/**
 * i915_perf_release - handles userspace close() of a stream file
 * @inode: anonymous inode associated with file
 * @file: An i915 perf stream file
 *
 * Cleans up any resources associated with an open i915 perf stream file.
 *
 * NB: close() can't really fail from the userspace point of view.
 *
 * Returns: zero on success or a negative error code.
 */
static int i915_perf_release(struct inode *inode, struct file *file)
{
	struct i915_perf_stream *stream = file->private_data;
	struct i915_perf *perf = stream->perf;
	struct intel_gt *gt = stream->engine->gt;

	/*
	 * unmap_mapping_range() was being called in i915_perf_release() to
	 * account for any mmapped vmas that the user did not unmap, either
	 * intentionally or by user task exiting before unmapping. Note that we
	 * do not need to unmap the OA buffer when closing the perf fd. If user
	 * did not unmap the buffer, then i915_perf_release will never get
	 * called because mmap holds a reference to the vma->vm_file which is
	 * the stream. If the user task exited, then kernel's do_exit() will
	 * take care of unmapping the vmas and eventually calling close on this
	 * FD.
	 *
	 * While unmap_mapping_range() is not needed, it's existence actually
	 * caused other issues. The stream FD is backed up by a static
	 * anon_inode_inode in the kernel that is shared by kernel and other
	 * susbsystems. The only differentiating factor is the address space
	 * used by each consumer of this inode. Each user of this inode would
	 * just unmap specific range in it's own address space. What OA was
	 * doing instead was zapping all the address spaces belonging to this
	 * inode. This resulted in zapping PTEs for an unrelated consumer
	 * altogether - the KVM, because KVM uses anon_inode_inode for a few
	 * things. This was crashing the Guest VM when we ran an OA use case!!
	 */

	/*
	 * Within this call, we know that the fd is being closed and we have no
	 * other user of stream->lock. Use the perf lock to destroy the stream
	 * here.
	 */
	mutex_lock(&gt->perf.lock);
	i915_perf_destroy_locked(stream);
	mutex_unlock(&gt->perf.lock);

	/* Release the reference the perf stream kept on the driver. */
	drm_dev_put(&perf->i915->drm);

	return 0;
}

static vm_fault_t vm_fault_oa(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct i915_perf_stream *stream = vma->vm_private_data;
	int err;

	err = remap_io_sg(vma,
			  vma->vm_start, vma->vm_end - vma->vm_start,
			  stream->oa_buffer.vma->pages, 0, -1);

	return i915_error_to_vmf_fault(err);
}

static const struct vm_operations_struct vm_ops_oa = {
	.fault = vm_fault_oa,
};

static int i915_perf_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct i915_perf_stream *stream = file->private_data;

	/* mmap-ing OA buffer to user space MUST absolutely be privileged */
	if (i915_perf_stream_paranoid && !perfmon_capable()) {
		DRM_DEBUG("Insufficient privileges to map OA buffer\n");
		return -EACCES;
	}

	switch (vma->vm_pgoff) {
	/*
	 * A non-zero offset ensures that we are mapping the right object. Also
	 * leaves room for future objects added to this implementation.
	 */
	case I915_PERF_OA_BUFFER_MMAP_OFFSET:
		if (!(stream->sample_flags & SAMPLE_OA_REPORT))
			return -EINVAL;

		if (vma->vm_end - vma->vm_start > stream->oa_buffer.vma->size)
			return -EINVAL;

		/*
		 * Only support VM_READ. Enforce MAP_PRIVATE by checking for
		 * VM_MAYSHARE.
		 */
		if (vma->vm_flags & (VM_WRITE | VM_EXEC |
				     VM_SHARED | VM_MAYSHARE))
			return -EINVAL;

#ifdef BPM_VM_FLAGS_IS_READ_ONLY_FLAG
		vm_flags_clear(vma, VM_MAYWRITE | VM_MAYEXEC);
#else
		vma->vm_flags &= ~(VM_MAYWRITE | VM_MAYEXEC);
#endif

		/*
		 * If the privileged parent forks and child drops root
		 * privilege, we do not want the child to retain access to the
		 * mapped OA buffer. Explicitly set VM_DONTCOPY to avoid such
		 * cases.
		 */
#ifdef BPM_VM_FLAGS_IS_READ_ONLY_FLAG
		vm_flags_set(vma, VM_PFNMAP | VM_DONTEXPAND |
				VM_DONTDUMP | VM_DONTCOPY);
#else
		vma->vm_flags |= VM_PFNMAP | VM_DONTEXPAND |
				 VM_DONTDUMP | VM_DONTCOPY;
#endif
		break;

	default:
		return -EINVAL;
	}

	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	vma->vm_private_data = stream;
	vma->vm_ops = &vm_ops_oa;

	return 0;
}

static loff_t i915_perf_llseek(struct file *file, loff_t offset, int whence)
{
	struct i915_perf_stream *stream = file->private_data;
	i915_reg_t oaheadptr = __oa_regs(stream)->oa_head_ptr;
	loff_t ret = -EINVAL;
	unsigned long flags;

	if (offset || !(stream->sample_flags & SAMPLE_OA_REPORT))
		goto end;

	switch (whence) {
	case SEEK_END:
		spin_lock_irqsave(&stream->oa_buffer.ptr_lock, flags);

		ret = _oa_taken(stream,
				stream->oa_buffer.tail,
				stream->oa_buffer.head);
		intel_uncore_write(stream->uncore, oaheadptr,
				stream->oa_buffer.tail &
				GEN12_OAG_OAHEADPTR_MASK);
		stream->oa_buffer.head = stream->oa_buffer.tail;

		spin_unlock_irqrestore(&stream->oa_buffer.ptr_lock, flags);
		break;

	default:
		break;
	}

end:
	return ret;
}

static const struct file_operations fops = {
	.owner		= THIS_MODULE,
	.llseek		= i915_perf_llseek,
	.release	= i915_perf_release,
	.poll		= i915_perf_poll,
	.read		= i915_perf_read,
	.unlocked_ioctl	= i915_perf_ioctl,
	/* Our ioctl have no arguments, so it's safe to use the same function
	 * to handle 32bits compatibility.
	 */
	.compat_ioctl   = i915_perf_ioctl,
	.mmap		= i915_perf_mmap,
};

static int
oa_stream_fd(struct i915_perf_stream *stream, const char *name,
	     const struct file_operations *fops, int flags)
{
	int ret, fd = get_unused_fd_flags(flags);
	struct file *file;

	file = anon_inode_getfile(name, fops, stream, flags);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto err_fd;
	}

	file->f_mode |= FMODE_LSEEK;
	fd_install(fd, file);

	return fd;

err_fd:
	put_unused_fd(fd);

	return ret;
}

/**
 * i915_perf_open_ioctl_locked - DRM ioctl() for userspace to open a stream FD
 * @perf: i915 perf instance
 * @param: The open parameters passed to 'DRM_I915_PERF_OPEN`
 * @props: individually validated u64 property value pairs
 * @file: drm file
 *
 * See i915_perf_ioctl_open() for interface details.
 *
 * Implements further stream config validation and stream initialization on
 * behalf of i915_perf_open_ioctl() with the &gt->perf.lock mutex
 * taken to serialize with any non-file-operation driver hooks.
 *
 * Note: at this point the @props have only been validated in isolation and
 * it's still necessary to validate that the combination of properties makes
 * sense.
 *
 * In the case where userspace is interested in OA unit metrics then further
 * config validation and stream initialization details will be handled by
 * i915_oa_stream_init(). The code here should only validate config state that
 * will be relevant to all stream types / backends.
 *
 * Returns: zero on success or a negative error code.
 */
static int
i915_perf_open_ioctl_locked(struct i915_perf *perf,
			    struct drm_i915_perf_open_param *param,
			    struct perf_open_properties *props,
			    struct drm_file *file)
{
	struct i915_gem_context *specific_ctx = NULL;
	struct i915_perf_stream *stream = NULL;
	unsigned long f_flags = 0;
	bool privileged_op = true;
	bool sample_oa;
	int stream_fd;
	int ret;

	if (props->single_context) {
		u32 ctx_handle = props->ctx_handle;
		struct drm_i915_file_private *file_priv = file->driver_priv;

		specific_ctx = i915_gem_context_lookup(file_priv, ctx_handle);
		if (!specific_ctx) {
			drm_dbg(&perf->i915->drm,
				"Failed to look up context with ID %u for opening perf stream\n",
				ctx_handle);
			ret = -ENOENT;
			goto err;
		}
	}

	sample_oa = !!(props->sample_flags & SAMPLE_OA_REPORT);

	/*
	 * Wa_1608137851:dg2:a0
	 *
	 * A gem_context passed in the perf interface serves 2 purposes:
	 *
	 * 1) Enables OAR/OAC functionality to supprot MI_RPC command
	 * 2) Filters OA buffer reports for context id specific to the
	 *    class:instance in this gem_context.
	 *
	 * OAC will only work on CCS0 on DG2 A0. Leave a note here when use case 1
	 * is not supported on A0.
	 */
	if ((IS_DG2_GRAPHICS_STEP(perf->i915, G10, STEP_A0, STEP_B0) ||
	     IS_DG2_GRAPHICS_STEP(perf->i915, G11, STEP_A0, STEP_B0)) &&
	    specific_ctx && props->engine->class == COMPUTE_CLASS &&
	    props->engine->instance != 0) {
		DRM_NOTE("OAC is incompatible with the compute engine instance %d\n",
			 props->engine->instance);

		if (!sample_oa)
			return -ENODEV;
	}

	/*
	 * On Haswell the OA unit supports clock gating off for a specific
	 * context and in this mode there's no visibility of metrics for the
	 * rest of the system, which we consider acceptable for a
	 * non-privileged client.
	 *
	 * For Gen8->11 the OA unit no longer supports clock gating off for a
	 * specific context and the kernel can't securely stop the counters
	 * from updating as system-wide / global values. Even though we can
	 * filter reports based on the included context ID we can't block
	 * clients from seeing the raw / global counter values via
	 * MI_REPORT_PERF_COUNT commands and so consider it a privileged op to
	 * enable the OA unit by default.
	 *
	 * For Gen12+ we gain a new OAR unit that only monitors the RCS on a
	 * per context basis. So we can relax requirements there if the user
	 * doesn't request global stream access (i.e. query based sampling
	 * using MI_RECORD_PERF_COUNT.
	 */
	if (specific_ctx && !sample_oa)
		privileged_op = false;

	if (props->hold_preemption) {
		if (!props->single_context) {
			drm_dbg(&perf->i915->drm,
				"preemption disable with no context\n");
			ret = -EINVAL;
			goto err;
		}
		privileged_op = true;
	}

	/*
	 * Asking for SSEU configuration is a priviliged operation.
	 */
	if (props->has_sseu)
		privileged_op = true;
	else
		get_default_sseu_config(&props->sseu, props->engine);

	/* Similar to perf's kernel.perf_paranoid_cpu sysctl option
	 * we check a dev.i915.perf_stream_paranoid sysctl option
	 * to determine if it's ok to access system wide OA counters
	 * without CAP_PERFMON or CAP_SYS_ADMIN privileges.
	 */
	if (privileged_op &&
	    i915_perf_stream_paranoid && !perfmon_capable()) {
		drm_dbg(&perf->i915->drm,
			"Insufficient privileges to open i915 perf stream\n");
		ret = -EACCES;
		goto err_ctx;
	}

	stream = kzalloc(sizeof(*stream), GFP_KERNEL);
	if (!stream) {
		ret = -ENOMEM;
		goto err_ctx;
	}

	stream->perf = perf;
	stream->ctx = specific_ctx;
	stream->poll_oa_period = props->poll_oa_period;

	ret = i915_oa_stream_init(stream, param, props);
	if (ret)
		goto err_alloc;

	/* we avoid simply assigning stream->sample_flags = props->sample_flags
	 * to have _stream_init check the combination of sample flags more
	 * thoroughly, but still this is the expected result at this point.
	 */
	if (WARN_ON(stream->sample_flags != props->sample_flags)) {
		ret = -ENODEV;
		goto err_flags;
	}

	if (param->flags & I915_PERF_FLAG_FD_CLOEXEC)
		f_flags |= O_CLOEXEC;
	if (param->flags & I915_PERF_FLAG_FD_NONBLOCK)
		f_flags |= O_NONBLOCK;

	stream_fd = oa_stream_fd(stream, "[i915_perf]", &fops, f_flags);
	if (stream_fd < 0) {
		ret = stream_fd;
		goto err_flags;
	}

	if (!(param->flags & I915_PERF_FLAG_DISABLED))
		i915_perf_enable_locked(stream);

	/*
	 * OA whitelist allows non-privileged access to some OA counters for
	 * triggering reports into the OA buffer. This is only allowed if
	 * perf_stream_paranoid is set to 0 by the sysadmin.
	 *
	 * We want to make sure this is almost the last thing we do before
	 * returning the stream fd. If we do end up checking for errors in code
	 * that follows this, we MUST call perf_group_remove_oa_whitelist in
	 * the error handling path to remove the whitelisted registers.
	 */
	if (!i915_perf_stream_paranoid && sample_oa) {
		perf_group_apply_oa_whitelist(stream->engine->oa_group);
		stream->oa_whitelisted = true;
	}

	/* Take a reference on the driver that will be kept with stream_fd
	 * until its release.
	 */
	drm_dev_get(&perf->i915->drm);

	return stream_fd;

err_flags:
	if (stream->ops->destroy)
		stream->ops->destroy(stream);
err_alloc:
	kfree(stream);
err_ctx:
	if (specific_ctx)
		i915_gem_context_put(specific_ctx);
err:
	return ret;
}

static u64 oa_exponent_to_ns(struct i915_perf *perf, int exponent)
{
	u64 nom = (2ULL << exponent) * NSEC_PER_SEC;
	u32 den = i915_perf_oa_timestamp_frequency(perf->i915);

	return div_u64(nom + den - 1, den);
}

static __always_inline bool
oa_format_valid(struct i915_perf *perf, enum drm_i915_oa_format format)
{
	return test_bit(format, perf->format_mask);
}

static __always_inline void
oa_format_add(struct i915_perf *perf, int format)
{
	__set_bit(format, perf->format_mask);
}

static int
select_oa_buffer_exponent(struct drm_i915_private *i915,
			  u64 requested_size)
{
	int order;

	/*
	 * When no size is specified, use the largest size supported by all
	 * generations.
	 */
	if (!requested_size)
		return order_base_2(SZ_16M);

	order = order_base_2(clamp_t(u64, requested_size, SZ_128K,
				     max_oa_buffer_size(i915)));
	if (requested_size != (1UL << order))
		return -EINVAL;

	return order;
}

/**
 * read_properties_unlocked - validate + copy userspace stream open properties
 * @perf: i915 perf instance
 * @uprops: The array of u64 key value pairs given by userspace
 * @n_props: The number of key value pairs expected in @uprops
 * @props: The stream configuration built up while validating properties
 *
 * Note this function only validates properties in isolation it doesn't
 * validate that the combination of properties makes sense or that all
 * properties necessary for a particular kind of stream have been set.
 *
 * Note that there currently aren't any ordering requirements for properties so
 * we shouldn't validate or assume anything about ordering here. This doesn't
 * rule out defining new properties with ordering requirements in the future.
 */
static int read_properties_unlocked(struct i915_perf *perf,
				    u64 __user *uprops,
				    u32 n_props,
				    struct perf_open_properties *props)
{
	u64 __user *uprop = uprops;
	u32 i;
	int ret;
	u8 class, instance;
	bool config_sseu = false;
	struct drm_i915_gem_context_param_sseu user_sseu;
	const struct i915_oa_format *f;
	u32 notify_num_reports = 1, max_reports;

	memset(props, 0, sizeof(struct perf_open_properties));
	props->poll_oa_period = DEFAULT_POLL_PERIOD_NS;

	/* Considering that ID = 0 is reserved and assuming that we don't
	 * (currently) expect any configurations to ever specify duplicate
	 * values for a particular property ID then the last _PROP_MAX value is
	 * one greater than the maximum number of properties we expect to get
	 * from userspace.
	 */
	if (!n_props || n_props >= PRELIM_DRM_I915_PERF_PROP_MAX) {
		drm_dbg(&perf->i915->drm,
			"Invalid no. of i915 perf properties given\n");
		return -EINVAL;
	}

	/* Defaults when class:instance is not passed */
	class = perf->default_ci.engine_class;
	instance = perf->default_ci.engine_instance;

	for (i = 0; i < n_props; i++) {
		u64 oa_period, oa_freq_hz;
		u64 id, value;

		ret = get_user(id, uprop);
		if (ret)
			return ret;

		ret = get_user(value, uprop + 1);
		if (ret)
			return ret;

		switch (id) {
		case DRM_I915_PERF_PROP_CTX_HANDLE:
			props->single_context = 1;
			props->ctx_handle = value;
			break;
		case DRM_I915_PERF_PROP_SAMPLE_OA:
			if (value)
				props->sample_flags |= SAMPLE_OA_REPORT;
			break;
		case DRM_I915_PERF_PROP_OA_METRICS_SET:
			if (value == 0) {
				drm_dbg(&perf->i915->drm,
					"Unknown OA metric set ID\n");
				return -EINVAL;
			}
			props->metrics_set = value;
			break;
		case DRM_I915_PERF_PROP_OA_FORMAT:
			if (value == 0 || value >= PRELIM_I915_OA_FORMAT_MAX) {
				drm_dbg(&perf->i915->drm,
					"Out-of-range OA report format %llu\n",
					  value);
				return -EINVAL;
			}
			if (!oa_format_valid(perf, value)) {
				drm_dbg(&perf->i915->drm,
					"Unsupported OA report format %llu\n",
					  value);
				return -EINVAL;
			}
			props->oa_format = value;
			break;
		case DRM_I915_PERF_PROP_OA_EXPONENT:
			if (value > OA_EXPONENT_MAX) {
				drm_dbg(&perf->i915->drm,
					"OA timer exponent too high (> %u)\n",
					 OA_EXPONENT_MAX);
				return -EINVAL;
			}

			/* Theoretically we can program the OA unit to sample
			 * e.g. every 160ns for HSW, 167ns for BDW/SKL or 104ns
			 * for BXT. We don't allow such high sampling
			 * frequencies by default unless root.
			 */

			BUILD_BUG_ON(sizeof(oa_period) != 8);
			oa_period = oa_exponent_to_ns(perf, value);

			/* This check is primarily to ensure that oa_period <=
			 * UINT32_MAX (before passing to do_div which only
			 * accepts a u32 denominator), but we can also skip
			 * checking anything < 1Hz which implicitly can't be
			 * limited via an integer oa_max_sample_rate.
			 */
			if (oa_period <= NSEC_PER_SEC) {
				u64 tmp = NSEC_PER_SEC;
				do_div(tmp, oa_period);
				oa_freq_hz = tmp;
			} else
				oa_freq_hz = 0;

			if (oa_freq_hz > i915_oa_max_sample_rate && !perfmon_capable()) {
				drm_dbg(&perf->i915->drm,
					"OA exponent would exceed the max sampling frequency (sysctl dev.i915.oa_max_sample_rate) %uHz without CAP_PERFMON or CAP_SYS_ADMIN privileges\n",
					  i915_oa_max_sample_rate);
				return -EACCES;
			}

			props->oa_periodic = true;
			props->oa_period_exponent = value;
			break;
		case DRM_I915_PERF_PROP_HOLD_PREEMPTION:
			props->hold_preemption = !!value;
			break;
		case DRM_I915_PERF_PROP_GLOBAL_SSEU: {
			if (GRAPHICS_VER_FULL(perf->i915) >= IP_VER(12, 50)) {
				drm_dbg(&perf->i915->drm,
					"SSEU config not supported on gfx %x\n",
					GRAPHICS_VER_FULL(perf->i915));
				return -ENODEV;
			}

			if (copy_from_user(&user_sseu,
					   u64_to_user_ptr(value),
					   sizeof(user_sseu))) {
				drm_dbg(&perf->i915->drm,
					"Unable to copy global sseu parameter\n");
				return -EFAULT;
			}
			config_sseu = true;
			break;
		}
		case DRM_I915_PERF_PROP_POLL_OA_PERIOD:
			if (value < 100000 /* 100us */) {
				drm_dbg(&perf->i915->drm,
					"OA availability timer too small (%lluns < 100us)\n",
					  value);
				return -EINVAL;
			}
			props->poll_oa_period = value;
			break;
		case PRELIM_DRM_I915_PERF_PROP_OA_BUFFER_SIZE:
			ret = select_oa_buffer_exponent(perf->i915, value);
			if (ret < 0) {
				DRM_DEBUG("OA buffer size invalid %llu\n", value);
				return ret;
			}
			props->oa_buffer_size_exponent = ret;
			break;
		case PRELIM_DRM_I915_PERF_PROP_OA_ENGINE_CLASS:
			class = (u8)value;
			break;
		case PRELIM_DRM_I915_PERF_PROP_OA_ENGINE_INSTANCE:
			instance = (u8)value;
			break;
		case PRELIM_DRM_I915_PERF_PROP_OA_NOTIFY_NUM_REPORTS:
			if (!value) {
				DRM_DEBUG("OA_NOTIFY_NUM_REPORTS must be a positive value %lld\n", value);
				return -EINVAL;
			}

			notify_num_reports = (u32)value;
			break;
		default:
			MISSING_CASE(id);
			return -EINVAL;
		}

		uprop += 2;
	}

	if (!props->oa_format) {
		drm_dbg(&perf->i915->drm, "OA report format not specified\n");
		return -EINVAL;
	}

	/*
	 * Enforce SAMPLE_OA is present if user passes OA_EXPONENT. The converse
	 * case when user passes SAMPLE_OA without OA_EXPONENT is handled in
	 * -EIO return in i915_oa_wait_unlocked.
	 */
	if (props->oa_periodic && !(props->sample_flags & SAMPLE_OA_REPORT))
		return -EINVAL;

	props->engine = intel_engine_lookup_user(perf->i915, class, instance);
	if (!props->engine) {
		drm_dbg(&perf->i915->drm,
			"OA engine class and instance invalid %d:%d\n",
			class, instance);
		return -EINVAL;
	}

	if (!engine_supports_oa(perf->i915, props->engine))
		return -EINVAL;

	if (!oa_unit_functional(props->engine))
		return -ENODEV;

	i = array_index_nospec(props->oa_format, PRELIM_I915_OA_FORMAT_MAX);
	f = &perf->oa_formats[i];
	if (!engine_class_supports_oa_format(props->engine, f->type)) {
		DRM_DEBUG("Invalid OA format %d for class %d\n",
			  f->type, props->engine->class);
		return -EINVAL;
	}

	if (config_sseu) {
		ret = get_sseu_config(&props->sseu, props->engine, &user_sseu);
		if (ret) {
			DRM_DEBUG("Invalid SSEU configuration\n");
			return ret;
		}
		props->has_sseu = true;
	}

	/* If no buffer size was requested, select the default one. */
	if (!props->oa_buffer_size_exponent) {
		props->oa_buffer_size_exponent =
			select_oa_buffer_exponent(perf->i915, 0);
	}

	max_reports = (1 << props->oa_buffer_size_exponent) /
		      perf->oa_formats[props->oa_format].size;
	if (notify_num_reports > max_reports) {
		DRM_DEBUG("OA_NOTIFY_NUM_REPORTS %d exceeds %d\n", notify_num_reports, max_reports);
		return -EINVAL;
	}

	props->notify_num_reports = notify_num_reports;

	return 0;
}

/**
 * i915_perf_open_ioctl - DRM ioctl() for userspace to open a stream FD
 * @dev: drm device
 * @data: ioctl data copied from userspace (unvalidated)
 * @file: drm file
 *
 * Validates the stream open parameters given by userspace including flags
 * and an array of u64 key, value pair properties.
 *
 * Very little is assumed up front about the nature of the stream being
 * opened (for instance we don't assume it's for periodic OA unit metrics). An
 * i915-perf stream is expected to be a suitable interface for other forms of
 * buffered data written by the GPU besides periodic OA metrics.
 *
 * Note we copy the properties from userspace outside of the i915 perf
 * mutex to avoid an awkward lockdep with mmap_lock.
 *
 * Most of the implementation details are handled by
 * i915_perf_open_ioctl_locked() after taking the &gt->perf.lock
 * mutex for serializing with any non-file-operation driver hooks.
 *
 * Return: A newly opened i915 Perf stream file descriptor or negative
 * error code on failure.
 */
int i915_perf_open_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file)
{
	struct i915_perf *perf = &to_i915(dev)->perf;
	struct drm_i915_perf_open_param *param = data;
	struct intel_gt *gt;
	struct perf_open_properties props;
	u32 known_open_flags;
	int ret;

	if (!perf->i915)
		return -EOPNOTSUPP;

	known_open_flags = I915_PERF_FLAG_FD_CLOEXEC |
			   I915_PERF_FLAG_FD_NONBLOCK |
			   PRELIM_I915_PERF_FLAG_FD_EU_STALL |
			   I915_PERF_FLAG_DISABLED;
	if (param->flags & ~known_open_flags) {
		drm_dbg(&perf->i915->drm,
			"Unknown drm_i915_perf_open_param flag\n");
		return -EINVAL;
	}

	if (param->flags & PRELIM_I915_PERF_FLAG_FD_EU_STALL)
		return i915_open_eu_stall_cntr(perf->i915, param, file);

	ret = read_properties_unlocked(perf,
				       u64_to_user_ptr(param->properties_ptr),
				       param->num_properties,
				       &props);
	if (ret)
		return ret;

	gt = props.engine->gt;

	mutex_lock(&gt->perf.lock);
	ret = i915_perf_open_ioctl_locked(perf, param, &props, file);
	mutex_unlock(&gt->perf.lock);

	return ret;
}

/**
 * i915_perf_register - exposes i915-perf to userspace
 * @i915: i915 device instance
 *
 * In particular OA metric sets are advertised under a sysfs metrics/
 * directory allowing userspace to enumerate valid IDs that can be
 * used to open an i915-perf stream.
 */
void i915_perf_register(struct drm_i915_private *i915)
{
	struct i915_perf *perf = &i915->perf;
	struct intel_gt *gt = to_gt(i915);

	if (!perf->i915)
		return;

	/* To be sure we're synchronized with an attempted
	 * i915_perf_open_ioctl(); considering that we register after
	 * being exposed to userspace.
	 */
	mutex_lock(&gt->perf.lock);

	perf->metrics_kobj =
		kobject_create_and_add("metrics",
				       &i915->drm.primary->kdev->kobj);

	mutex_unlock(&gt->perf.lock);
}

/**
 * i915_perf_unregister - hide i915-perf from userspace
 * @i915: i915 device instance
 *
 * i915-perf state cleanup is split up into an 'unregister' and
 * 'deinit' phase where the interface is first hidden from
 * userspace by i915_perf_unregister() before cleaning up
 * remaining state in i915_perf_fini().
 */
void i915_perf_unregister(struct drm_i915_private *i915)
{
	struct i915_perf *perf = &i915->perf;

	if (!perf->metrics_kobj)
		return;

	kobject_put(perf->metrics_kobj);
	perf->metrics_kobj = NULL;
}

static bool gen8_is_valid_flex_addr(struct i915_perf *perf, u32 addr)
{
	static const i915_reg_t flex_eu_regs[] = {
		EU_PERF_CNTL0,
		EU_PERF_CNTL1,
		EU_PERF_CNTL2,
		EU_PERF_CNTL3,
		EU_PERF_CNTL4,
		EU_PERF_CNTL5,
		EU_PERF_CNTL6,
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(flex_eu_regs); i++) {
		if (i915_mmio_reg_offset(flex_eu_regs[i]) == addr)
			return true;
	}
	return false;
}

static bool reg_in_range_table(u32 addr, const struct i915_range *table)
{
	while (table->start || table->end) {
		if (addr >= table->start && addr <= table->end)
			return true;

		table++;
	}

	return false;
}

#define REG_EQUAL(addr, mmio) \
	((addr) == i915_mmio_reg_offset(mmio))

static const struct i915_range gen12_oa_b_counters[] = {
	{ .start = 0x2b2c, .end = 0x2b2c },	/* GEN12_OAG_OA_PESS */
	{ .start = 0xd900, .end = 0xd91c },	/* GEN12_OAG_OASTARTTRIG[1-8] */
	{ .start = 0xd920, .end = 0xd93c },	/* GEN12_OAG_OAREPORTTRIG1[1-8] */
	{ .start = 0xd940, .end = 0xd97c },	/* GEN12_OAG_CEC[0-7][0-1] */
	{ .start = 0xdc00, .end = 0xdc3c },	/* GEN12_OAG_SCEC[0-7][0-1] */
	{ .start = 0xdc40, .end = 0xdc40 },	/* GEN12_OAG_SPCTR_CNF */
	{ .start = 0xdc44, .end = 0xdc44 },	/* GEN12_OAA_DBG_REG */
	{}
};

static const struct i915_range xehp_oa_b_counters[] = {
	{ .start = 0xdc48, .end = 0xdc48 },	/* OAA_ENABLE_REG */
	{ .start = 0xdd00, .end = 0xdd48 },	/* OAG_LCE0_0 - OAA_LENABLE_REG */
	{},
};

static const struct i915_range gen12_oa_mux_regs[] = {
	{ .start = 0x0d00, .end = 0x0d04 },     /* RPM_CONFIG[0-1] */
	{ .start = 0x0d0c, .end = 0x0d2c },     /* NOA_CONFIG[0-8] */
	{ .start = 0x9840, .end = 0x9840 },	/* GDT_CHICKEN_BITS */
	{ .start = 0x9884, .end = 0x9888 },	/* NOA_WRITE */
	{ .start = 0x20cc, .end = 0x20cc },	/* WAIT_FOR_RC6_EXIT */
	{}
};

/*
 * Ref: 14010536224:
 * 0x20cc is repurposed on MTL, so use a separate array for MTL. Also add the
 * MPES/MPEC registers.
 */
static const struct i915_range mtl_oa_mux_regs[] = {
	{ .start = 0x0d00, .end = 0x0d04 },	/* RPM_CONFIG[0-1] */
	{ .start = 0x0d0c, .end = 0x0d2c },	/* NOA_CONFIG[0-8] */
	{ .start = 0x9840, .end = 0x9840 },	/* GDT_CHICKEN_BITS */
	{ .start = 0x9884, .end = 0x9888 },	/* NOA_WRITE */
	{ .start = 0x393200, .end = 0x39323C },	/* MPES[0-7] */
};

static bool gen12_is_valid_b_counter_addr(struct i915_perf *perf, u32 addr)
{
	return reg_in_range_table(addr, gen12_oa_b_counters);
}

#define REG_IN_RANGE(addr, start, end) \
	((addr) >= i915_mmio_reg_offset(start) && \
	 (addr) <= i915_mmio_reg_offset(end))

static bool __is_valid_media_b_counter_addr(u32 addr, u32 base)
{
	return REG_IN_RANGE(addr, GEN12_OAM_STARTTRIG1(base), GEN12_OAM_STARTTRIG8(base)) ||
		REG_IN_RANGE(addr, GEN12_OAM_REPORTTRIG1(base), GEN12_OAM_REPORTTRIG8(base)) ||
		REG_IN_RANGE(addr, GEN12_OAM_CEC0_0(base), GEN12_OAM_CEC7_1(base));
}

static bool is_valid_oam_b_counter_addr(struct i915_perf *perf, u32 addr)
{
	struct i915_perf_group *group = to_gt(perf->i915)->perf.group;
	int i;

	/*
	 * Check against groups in single gt since registers are
	 * the same across all gts
	 */
	for (i = 0; i < to_gt(perf->i915)->perf.num_perf_groups; i++) {
		if (group[i].type != TYPE_OAM)
			continue;

		if (__is_valid_media_b_counter_addr(addr, group[i].regs.base))
			return true;
	}

	return false;
}

static bool xehp_is_valid_b_counter_addr(struct i915_perf *perf, u32 addr)
{
	return reg_in_range_table(addr, xehp_oa_b_counters) ||
		reg_in_range_table(addr, gen12_oa_b_counters) ||
		is_valid_oam_b_counter_addr(perf, addr);
}

static bool gen12_is_valid_mux_addr(struct i915_perf *perf, u32 addr)
{
	if (IS_METEORLAKE(perf->i915))
		return reg_in_range_table(addr, mtl_oa_mux_regs);
	else
		return reg_in_range_table(addr, gen12_oa_mux_regs);
}

static u32 mask_reg_value(u32 reg, u32 val)
{
	/* HALF_SLICE_CHICKEN2 is programmed with a the
	 * WaDisableSTUnitPowerOptimization workaround. Make sure the value
	 * programmed by userspace doesn't change this.
	 */
	if (REG_EQUAL(reg, HALF_SLICE_CHICKEN2))
		val = val & ~_MASKED_BIT_ENABLE(GEN8_ST_PO_DISABLE);

	/* WAIT_FOR_RC6_EXIT has only one bit fullfilling the function
	 * indicated by its name and a bunch of selection fields used by OA
	 * configs.
	 */
	if (REG_EQUAL(reg, WAIT_FOR_RC6_EXIT))
		val = val & ~_MASKED_BIT_ENABLE(HSW_WAIT_FOR_RC6_EXIT_ENABLE);

	return val;
}

static struct i915_oa_reg *alloc_oa_regs(struct i915_perf *perf,
					 bool (*is_valid)(struct i915_perf *perf, u32 addr),
					 u32 __user *regs,
					 u32 n_regs)
{
	struct i915_oa_reg *oa_regs;
	int err;
	u32 i;

	if (!n_regs)
		return NULL;

	/* No is_valid function means we're not allowing any register to be programmed. */
	GEM_BUG_ON(!is_valid);
	if (!is_valid)
		return ERR_PTR(-EINVAL);

	oa_regs = kmalloc_array(n_regs, sizeof(*oa_regs), GFP_KERNEL);
	if (!oa_regs)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < n_regs; i++) {
		u32 addr, value;

		err = get_user(addr, regs);
		if (err)
			goto addr_err;

		if (!is_valid(perf, addr)) {
			drm_dbg(&perf->i915->drm,
				"Invalid oa_reg address: %X\n", addr);
			err = -EINVAL;
			goto addr_err;
		}

		err = get_user(value, regs + 1);
		if (err)
			goto addr_err;

		oa_regs[i].addr = _MMIO(addr);
		oa_regs[i].value = mask_reg_value(addr, value);

		regs += 2;
	}

	return oa_regs;

addr_err:
	kfree(oa_regs);
	return ERR_PTR(err);
}

static ssize_t show_dynamic_id(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       char *buf)
{
	struct i915_oa_config *oa_config =
		container_of(attr, typeof(*oa_config), sysfs_metric_id);

	return sprintf(buf, "%d\n", oa_config->id);
}

static int create_dynamic_oa_sysfs_entry(struct i915_perf *perf,
					 struct i915_oa_config *oa_config)
{
	sysfs_attr_init(&oa_config->sysfs_metric_id.attr);
	oa_config->sysfs_metric_id.attr.name = "id";
	oa_config->sysfs_metric_id.attr.mode = S_IRUGO;
	oa_config->sysfs_metric_id.show = show_dynamic_id;
	oa_config->sysfs_metric_id.store = NULL;

	oa_config->attrs[0] = &oa_config->sysfs_metric_id.attr;
	oa_config->attrs[1] = NULL;

	oa_config->sysfs_metric.name = oa_config->uuid;
	oa_config->sysfs_metric.attrs = oa_config->attrs;

	return sysfs_create_group(perf->metrics_kobj,
				  &oa_config->sysfs_metric);
}

/**
 * i915_perf_add_config_ioctl - DRM ioctl() for userspace to add a new OA config
 * @dev: drm device
 * @data: ioctl data (pointer to struct drm_i915_perf_oa_config) copied from
 *        userspace (unvalidated)
 * @file: drm file
 *
 * Validates the submitted OA register to be saved into a new OA config that
 * can then be used for programming the OA unit and its NOA network.
 *
 * Returns: A new allocated config number to be used with the perf open ioctl
 * or a negative error code on failure.
 */
int i915_perf_add_config_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file)
{
	struct i915_perf *perf = &to_i915(dev)->perf;
	struct drm_i915_perf_oa_config *args = data;
	struct i915_oa_config *oa_config, *tmp;
	struct i915_oa_reg *regs;
	int err, id;

	if (!perf->i915)
		return -EOPNOTSUPP;

	if (!perf->metrics_kobj) {
		drm_dbg(&perf->i915->drm,
			"OA metrics weren't advertised via sysfs\n");
		return -EINVAL;
	}

	if (i915_perf_stream_paranoid && !perfmon_capable()) {
		drm_dbg(&perf->i915->drm,
			"Insufficient privileges to add i915 OA config\n");
		return -EACCES;
	}

	if ((!args->mux_regs_ptr || !args->n_mux_regs) &&
	    (!args->boolean_regs_ptr || !args->n_boolean_regs) &&
	    (!args->flex_regs_ptr || !args->n_flex_regs)) {
		drm_dbg(&perf->i915->drm,
			"No OA registers given\n");
		return -EINVAL;
	}

	oa_config = kzalloc(sizeof(*oa_config), GFP_KERNEL);
	if (!oa_config) {
		drm_dbg(&perf->i915->drm,
			"Failed to allocate memory for the OA config\n");
		return -ENOMEM;
	}

	oa_config->perf = perf;
	kref_init(&oa_config->ref);

	if (!uuid_is_valid(args->uuid)) {
		drm_dbg(&perf->i915->drm,
			"Invalid uuid format for OA config\n");
		err = -EINVAL;
		goto reg_err;
	}

	/* Last character in oa_config->uuid will be 0 because oa_config is
	 * kzalloc.
	 */
	memcpy(oa_config->uuid, args->uuid, sizeof(args->uuid));

	oa_config->mux_regs_len = args->n_mux_regs;
	regs = alloc_oa_regs(perf,
			     perf->ops.is_valid_mux_reg,
			     u64_to_user_ptr(args->mux_regs_ptr),
			     args->n_mux_regs);

	if (IS_ERR(regs)) {
		drm_dbg(&perf->i915->drm,
			"Failed to create OA config for mux_regs\n");
		err = PTR_ERR(regs);
		goto reg_err;
	}
	oa_config->mux_regs = regs;

	oa_config->b_counter_regs_len = args->n_boolean_regs;
	regs = alloc_oa_regs(perf,
			     perf->ops.is_valid_b_counter_reg,
			     u64_to_user_ptr(args->boolean_regs_ptr),
			     args->n_boolean_regs);

	if (IS_ERR(regs)) {
		drm_dbg(&perf->i915->drm,
			"Failed to create OA config for b_counter_regs\n");
		err = PTR_ERR(regs);
		goto reg_err;
	}
	oa_config->b_counter_regs = regs;

	oa_config->flex_regs_len = args->n_flex_regs;
	regs = alloc_oa_regs(perf,
			     perf->ops.is_valid_flex_reg,
			     u64_to_user_ptr(args->flex_regs_ptr),
			     args->n_flex_regs);

	if (IS_ERR(regs)) {
		drm_dbg(&perf->i915->drm,
			"Failed to create OA config for flex_regs\n");
		err = PTR_ERR(regs);
		goto reg_err;
	}
	oa_config->flex_regs = regs;

	err = mutex_lock_interruptible(&perf->metrics_lock);
	if (err)
		goto reg_err;

	/* We shouldn't have too many configs, so this iteration shouldn't be
	 * too costly.
	 */
	idr_for_each_entry(&perf->metrics_idr, tmp, id) {
		if (!strcmp(tmp->uuid, oa_config->uuid)) {
			drm_dbg(&perf->i915->drm,
				"OA config already exists with this uuid\n");
			err = -EADDRINUSE;
			goto sysfs_err;
		}
	}

	err = create_dynamic_oa_sysfs_entry(perf, oa_config);
	if (err) {
		drm_dbg(&perf->i915->drm,
			"Failed to create sysfs entry for OA config\n");
		goto sysfs_err;
	}

	/* Config id 0 is invalid, id 1 for kernel stored test config. */
	oa_config->id = idr_alloc(&perf->metrics_idr,
				  oa_config, 2,
				  0, GFP_KERNEL);
	if (oa_config->id < 0) {
		drm_dbg(&perf->i915->drm,
			"Failed to create sysfs entry for OA config\n");
		err = oa_config->id;
		goto sysfs_err;
	}

	mutex_unlock(&perf->metrics_lock);

	drm_dbg(&perf->i915->drm,
		"Added config %s id=%i\n", oa_config->uuid, oa_config->id);

	return oa_config->id;

sysfs_err:
	mutex_unlock(&perf->metrics_lock);
reg_err:
	i915_oa_config_put(oa_config);
	drm_dbg(&perf->i915->drm,
		"Failed to add new OA config\n");
	return err;
}

/**
 * i915_perf_remove_config_ioctl - DRM ioctl() for userspace to remove an OA config
 * @dev: drm device
 * @data: ioctl data (pointer to u64 integer) copied from userspace
 * @file: drm file
 *
 * Configs can be removed while being used, the will stop appearing in sysfs
 * and their content will be freed when the stream using the config is closed.
 *
 * Returns: 0 on success or a negative error code on failure.
 */
int i915_perf_remove_config_ioctl(struct drm_device *dev, void *data,
				  struct drm_file *file)
{
	struct i915_perf *perf = &to_i915(dev)->perf;
	u64 *arg = data;
	struct i915_oa_config *oa_config;
	int ret;

	if (!perf->i915)
		return -EOPNOTSUPP;

	if (i915_perf_stream_paranoid && !perfmon_capable()) {
		drm_dbg(&perf->i915->drm,
			"Insufficient privileges to remove i915 OA config\n");
		return -EACCES;
	}

	ret = mutex_lock_interruptible(&perf->metrics_lock);
	if (ret)
		return ret;

	oa_config = idr_find(&perf->metrics_idr, *arg);
	if (!oa_config) {
		drm_dbg(&perf->i915->drm,
			"Failed to remove unknown OA config\n");
		ret = -ENOENT;
		goto err_unlock;
	}

	GEM_BUG_ON(*arg != oa_config->id);

	sysfs_remove_group(perf->metrics_kobj, &oa_config->sysfs_metric);

	idr_remove(&perf->metrics_idr, *arg);

	mutex_unlock(&perf->metrics_lock);

	drm_dbg(&perf->i915->drm,
		"Removed config %s id=%i\n", oa_config->uuid, oa_config->id);

	i915_oa_config_put(oa_config);

	return 0;

err_unlock:
	mutex_unlock(&perf->metrics_lock);
	return ret;
}

static struct ctl_table oa_table[] = {
	{
	 .procname = "perf_stream_paranoid",
	 .data = &i915_perf_stream_paranoid,
	 .maxlen = sizeof(i915_perf_stream_paranoid),
	 .mode = 0644,
	 .proc_handler = proc_dointvec_minmax,
	 .extra1 = SYSCTL_ZERO,
	 .extra2 = SYSCTL_ONE,
	 },
	{
	 .procname = "oa_max_sample_rate",
	 .data = &i915_oa_max_sample_rate,
	 .maxlen = sizeof(i915_oa_max_sample_rate),
	 .mode = 0644,
	 .proc_handler = proc_dointvec_minmax,
	 .extra1 = SYSCTL_ZERO,
	 .extra2 = &oa_sample_rate_hard_limit,
	 },
#ifndef BPM_EMPTY_OA_CTL_TABLE_PRESENT
	{}
#endif
};

#ifndef BPM_REGISTER_SYSCTL_TABLE_NOT_PRESENT
static struct ctl_table i915_root[] = {
	{
	 .procname = CPTCFG_MODULE_I915,
	 .maxlen = 0,
	 .mode = 0555,
	 .child = oa_table,
	 },
	{}
};
#endif

#ifndef BPM_REGISTER_SYSCTL_TABLE_NOT_PRESENT
static struct ctl_table dev_root[] = {
	{
	 .procname = "dev",
	 .maxlen = 0,
	 .mode = 0555,
	 .child = i915_root,
	 },
	{}
};
#endif

static u32 __num_perf_groups_per_gt(struct intel_gt *gt)
{
	enum intel_platform platform = INTEL_INFO(gt->i915)->platform;

	switch (platform) {
	case INTEL_PONTEVECCHIO:
		return 4;
	case INTEL_DG2:
		return 3;
	case INTEL_XEHPSDV:
		return 5;
	case INTEL_METEORLAKE:
		return 1;
	default:
		return 1;
	}
}

static u32 __oam_engine_group(struct intel_engine_cs *engine)
{
	enum intel_platform platform = INTEL_INFO(engine->i915)->platform;
	struct intel_gt *gt = engine->gt;
	u32 group = PERF_GROUP_INVALID;

	switch (platform) {
	case INTEL_METEORLAKE:
		/*
		 * There's 1 SAMEDIA gt and 1 OAM per SAMEDIA gt. All media slices
		 * within the gt use the same OAM. All MTL SKUs list 1 SA MEDIA.
		 */
		drm_WARN_ON(&engine->i915->drm,
			    engine->gt->type != GT_MEDIA);

		group = PERF_GROUP_OAM_SAMEDIA_0;
		break;
	case INTEL_PONTEVECCHIO:
		/*
		 * PVC mappings:
		 *
		 * VCS0 - PERF_GROUP_OAM_0
		 * VCS1 - PERF_GROUP_OAM_2
		 * VCS2 - PERF_GROUP_OAM_1
		 */
		drm_WARN_ON(&engine->i915->drm,
			    engine->class == VIDEO_ENHANCEMENT_CLASS);

		if (engine->id == VCS0)
			group = PERF_GROUP_OAM_0;
		else if (engine->id == VCS1)
			group = PERF_GROUP_OAM_2;
		else if (engine->id == VCS2)
			group = PERF_GROUP_OAM_1;
		else
			drm_WARN(&gt->i915->drm, 1,
				 "Unsupported vcs for OA %d\n", engine->id);
		break;
	case INTEL_DG2:
		/*
		 * DG2 mappings:
		 *
		 * VCS0, VECS0 - PERF_GROUP_OAM_0
		 * VCS2, VECS1 - PERF_GROUP_OAM_1
		 */
		fallthrough;
	case INTEL_XEHPSDV:
		/*
		 * XEHPSDV mappings:
		 *
		 * VCS0, VCS1, VECS0 - PERF_GROUP_OAM_0
		 * VCS2, VCS3, VECS1 - PERF_GROUP_OAM_1
		 * VCS4, VCS5, VECS2 - PERF_GROUP_OAM_2
		 * VCS6, VCS7, VECS3 - PERF_GROUP_OAM_3
		 */
		group = engine->class == VIDEO_ENHANCEMENT_CLASS ?
			engine->instance + 1 : (engine->instance >> 1) + 1;
		break;
	default:
		break;
	}

	drm_WARN_ON(&gt->i915->drm, group >= __num_perf_groups_per_gt(gt));

	return group;
}

static u32 __oa_engine_group(struct intel_engine_cs *engine)
{
	if (!engine_supports_oa(engine->i915, engine))
		return PERF_GROUP_INVALID;

	switch (engine->class) {
	case RENDER_CLASS:
	case COMPUTE_CLASS:
		return PERF_GROUP_OAG;

	case VIDEO_DECODE_CLASS:
	case VIDEO_ENHANCEMENT_CLASS:
		return __oam_engine_group(engine);

	default:
		return PERF_GROUP_INVALID;
	}
}

static struct i915_perf_regs __oam_regs(u32 base)
{
	return (struct i915_perf_regs) {
		base,
		GEN12_OAM_HEAD_POINTER(base),
		GEN12_OAM_TAIL_POINTER(base),
		GEN12_OAM_BUFFER(base),
		GEN12_OAM_CONTEXT_CONTROL(base),
		GEN12_OAM_CONTROL(base),
		GEN12_OAM_DEBUG(base),
		GEN12_OAM_STATUS(base),
		GEN12_OAM_CONTROL_COUNTER_FORMAT_SHIFT,
	};
}

static struct i915_perf_regs __oag_regs(void)
{
	return (struct i915_perf_regs) {
		0,
		GEN12_OAG_OAHEADPTR,
		GEN12_OAG_OATAILPTR,
		GEN12_OAG_OABUFFER,
		GEN12_OAG_OAGLBCTXCTRL,
		GEN12_OAG_OACONTROL,
		GEN12_OAG_OA_DEBUG,
		GEN12_OAG_OASTATUS,
		GEN12_OAG_OACONTROL_OA_COUNTER_FORMAT_SHIFT,
	};
}

static void oa_init_regs(struct intel_gt *gt, u32 id)
{
	struct i915_perf_group *group = &gt->perf.group[id];
	struct i915_perf_regs *regs = &group->regs;

	if (id == PERF_GROUP_OAG && gt->type != GT_MEDIA)
		*regs = __oag_regs();
	else if (IS_METEORLAKE(gt->i915))
		*regs = __oam_regs(mtl_oa_base[id]);
	else if (IS_PONTEVECCHIO(gt->i915))
		*regs = __oam_regs(pvc_oa_base[id]);
	else if (IS_DG2(gt->i915))
		*regs = __oam_regs(dg2_oa_base[id]);
	else
		drm_WARN(&gt->i915->drm, 1, "Unsupported platform for OA\n");
}

static void oa_init_groups(struct intel_gt *gt)
{
	int i, num_groups = gt->perf.num_perf_groups;
	struct i915_perf *perf = &gt->i915->perf;

	for (i = 0; i < num_groups; i++) {
		struct i915_perf_group *g = &gt->perf.group[i];

		/*
		 * HSD: 22012764120
		 * OAM traffic uses the VDBOX0 channel of the media slice that
		 * the OAM unit belongs to. In case the VDBOX0 is fused off, OAM
		 * traffic is blocked and OAM cannot be used. VDBOX0 corresponds
		 * to even numbered VDBOXes in the driver. Ensure that such OAM
		 * units are disabled from use.
		 */
		if (OAM_USES_VDBOX0_CHANNEL(gt->i915) &&
		    ((!HAS_ENGINE(gt, _VCS(0)) && i == PERF_GROUP_OAM_0) ||
		     (!HAS_ENGINE(gt, _VCS(2)) && i == PERF_GROUP_OAM_1) ||
		     (!HAS_ENGINE(gt, _VCS(4)) && i == PERF_GROUP_OAM_2) ||
		     (!HAS_ENGINE(gt, _VCS(6)) && i == PERF_GROUP_OAM_3))) {
			g->num_engines = 0;
			continue;
		}

		/* Fused off engines can result in a group with num_engines == 0 */
		if (g->num_engines == 0)
			continue;

		/* Set oa_unit_ids now to ensure ids remain contiguous. */
		g->oa_unit_id = perf->oa_unit_ids++;

		g->gt = gt;
		oa_init_regs(gt, i);
		g->fw_domains = FORCEWAKE_ALL;
		if (i == PERF_GROUP_OAG) {
			g->type = TYPE_OAG;

			/*
			 * Enabling all fw domains for OAG caps the max GT
			 * frequency to media FF max. This could be less than
			 * what the user sets through the sysfs and perf
			 * measurements could be skewed. Since some platforms
			 * have separate OAM units to measure media perf, do not
			 * enable media fw domains for OAG.
			 */
			if (HAS_OAM(gt->i915))
				g->fw_domains = FORCEWAKE_GT | FORCEWAKE_RENDER;
		} else {
			g->type = TYPE_OAM;
		}
	}
}

static int oa_init_gt(struct intel_gt *gt)
{
	u32 num_groups = __num_perf_groups_per_gt(gt);
	struct intel_engine_cs *engine;
	struct i915_perf_group *g;
	intel_engine_mask_t tmp;

	g = kzalloc(sizeof(*g) * num_groups, GFP_KERNEL);
	if (drm_WARN_ON(&gt->i915->drm, !g))
		return -ENOMEM;

	for_each_engine_masked(engine, gt, ALL_ENGINES, tmp) {
		u32 index;

		index = __oa_engine_group(engine);
		if (index < num_groups) {
			g[index].engine_mask |= BIT(engine->id);
			g[index].num_engines++;
			engine->oa_group = &g[index];
		} else {
			engine->oa_group = NULL;
		}
	}

	gt->perf.num_perf_groups = num_groups;
	gt->perf.group = g;

	oa_init_groups(gt);

	return 0;
}

static int oa_init_engine_groups(struct i915_perf *perf)
{
	struct intel_gt *gt;
	int i, ret;

	for_each_gt(gt, perf->i915, i) {
		ret = oa_init_gt(gt);
		if (ret)
			return ret;
	}

	return 0;
}

static u16 oa_init_default_class(struct i915_perf *perf)
{
	bool has_vcs = false, has_vecs = false;
	bool has_rcs = false, has_ccs = false;
	struct intel_gt *gt;
	int i, j;

	for_each_gt(gt, perf->i915, i) {
		for (j = 0; j < gt->perf.num_perf_groups; j++) {
			struct i915_perf_group *g = &gt->perf.group[j];

			if (!g->num_engines)
				continue;

			if (g->engine_mask & (RCS_MASK(gt) << RCS0))
				has_rcs = true;
			else if (g->engine_mask & (CCS_MASK(gt) << CCS0))
				has_ccs = true;
			else if (g->engine_mask & (VDBOX_MASK(gt) << VCS0))
				has_vcs = true;
			else if (g->engine_mask & (VEBOX_MASK(gt) << VECS0))
				has_vecs = true;
			else
				drm_WARN(&gt->i915->drm, 1,
					 "Invalid g->engine_mask\n");
		}
	}

	if (has_rcs)
		return I915_ENGINE_CLASS_RENDER;
	else if (has_ccs)
		return I915_ENGINE_CLASS_COMPUTE;
	else if (has_vcs)
		return I915_ENGINE_CLASS_VIDEO;
	else if (has_vecs)
		return I915_ENGINE_CLASS_VIDEO_ENHANCE;

	drm_WARN(&perf->i915->drm, 1,
		 "Failed to find default class for perf\n");
	return 0;
}

static void oa_init_supported_formats(struct i915_perf *perf)
{
	struct drm_i915_private *i915 = perf->i915;
	enum intel_platform platform = INTEL_INFO(i915)->platform;

	switch (platform) {
	case INTEL_HASWELL:
		oa_format_add(perf, I915_OA_FORMAT_A13);
		oa_format_add(perf, I915_OA_FORMAT_A13);
		oa_format_add(perf, I915_OA_FORMAT_A29);
		oa_format_add(perf, I915_OA_FORMAT_A13_B8_C8);
		oa_format_add(perf, I915_OA_FORMAT_B4_C8);
		oa_format_add(perf, I915_OA_FORMAT_A45_B8_C8);
		oa_format_add(perf, I915_OA_FORMAT_B4_C8_A16);
		oa_format_add(perf, I915_OA_FORMAT_C4_B8);
		break;

	case INTEL_BROADWELL:
	case INTEL_CHERRYVIEW:
	case INTEL_SKYLAKE:
	case INTEL_BROXTON:
	case INTEL_KABYLAKE:
	case INTEL_GEMINILAKE:
	case INTEL_COFFEELAKE:
	case INTEL_COMETLAKE:
	case INTEL_ICELAKE:
	case INTEL_ELKHARTLAKE:
	case INTEL_JASPERLAKE:
	case INTEL_TIGERLAKE:
	case INTEL_ROCKETLAKE:
	case INTEL_DG1:
	case INTEL_ALDERLAKE_S:
	case INTEL_ALDERLAKE_P:
		oa_format_add(perf, I915_OA_FORMAT_A12);
		oa_format_add(perf, I915_OA_FORMAT_A12_B8_C8);
		oa_format_add(perf, I915_OA_FORMAT_A32u40_A4u32_B8_C8);
		oa_format_add(perf, I915_OA_FORMAT_C4_B8);
		break;

	case INTEL_XEHPSDV:
		oa_format_add(perf, I915_OAR_FORMAT_A32u40_A4u32_B8_C8);
		oa_format_add(perf, I915_OA_FORMAT_A24u40_A14u32_B8_C8);
		oa_format_add(perf, PRELIM_I915_OAR_FORMAT_A32u40_A4u32_B8_C8);
		oa_format_add(perf, PRELIM_I915_OA_FORMAT_A24u40_A14u32_B8_C8);
		oa_format_add(perf, PRELIM_I915_OAM_FORMAT_A2u64_B8_C8);
		break;

	case INTEL_DG2:
	case INTEL_PONTEVECCHIO:
		oa_format_add(perf, I915_OAR_FORMAT_A32u40_A4u32_B8_C8);
		oa_format_add(perf, I915_OA_FORMAT_A24u40_A14u32_B8_C8);
		oa_format_add(perf, PRELIM_I915_OAR_FORMAT_A32u40_A4u32_B8_C8);
		oa_format_add(perf, PRELIM_I915_OA_FORMAT_A24u40_A14u32_B8_C8);
		oa_format_add(perf, PRELIM_I915_OAM_FORMAT_A2u64_B8_C8);
		oa_format_add(perf, PRELIM_I915_OAR_FORMAT_A36u64_B8_C8);
		oa_format_add(perf, PRELIM_I915_OAC_FORMAT_A24u64_B8_C8);
		oa_format_add(perf, PRELIM_I915_OA_FORMAT_A38u64_R2u64_B8_C8);
		oa_format_add(perf, PRELIM_I915_OAM_FORMAT_A2u64_R2u64_B8_C8);
		oa_format_add(perf, PRELIM_I915_OAC_FORMAT_A22u32_R2u32_B8_C8);
		break;

	case INTEL_METEORLAKE:
		oa_format_add(perf, I915_OAR_FORMAT_A32u40_A4u32_B8_C8);
		oa_format_add(perf, I915_OA_FORMAT_A24u40_A14u32_B8_C8);
		oa_format_add(perf, PRELIM_I915_OAR_FORMAT_A32u40_A4u32_B8_C8);
		oa_format_add(perf, PRELIM_I915_OA_FORMAT_A24u40_A14u32_B8_C8);
		oa_format_add(perf, PRELIM_I915_OAR_FORMAT_A36u64_B8_C8);
		oa_format_add(perf, PRELIM_I915_OAC_FORMAT_A24u64_B8_C8);
		oa_format_add(perf, PRELIM_I915_OA_FORMAT_A38u64_R2u64_B8_C8);
		oa_format_add(perf, PRELIM_I915_OAC_FORMAT_A22u32_R2u32_B8_C8);
		oa_format_add(perf, PRELIM_I915_OAM_FORMAT_MPEC8u64_B8_C8);
		oa_format_add(perf, PRELIM_I915_OAM_FORMAT_MPEC8u32_B8_C8);
		break;

	default:
		MISSING_CASE(platform);
	}

	if (IS_DG2_G11(i915)) {
		/* Wa_1608133521:dg2 */
		oa_formats[PRELIM_I915_OAR_FORMAT_A36u64_B8_C8].header = HDR_32_BIT;
		oa_formats[PRELIM_I915_OAC_FORMAT_A24u64_B8_C8].header = HDR_32_BIT;
		oa_formats[PRELIM_I915_OA_FORMAT_A38u64_R2u64_B8_C8].header = HDR_32_BIT;
		oa_formats[PRELIM_I915_OAM_FORMAT_A2u64_R2u64_B8_C8].header = HDR_32_BIT;
	}
}

static void gen12_init_info(struct drm_i915_private *i915)
{
	enum intel_platform platform = INTEL_INFO(i915)->platform;
	struct i915_perf *perf = &i915->perf;

	switch (platform) {
	case INTEL_XEHPSDV:
		perf->ctx_pwr_clk_state_offset[PRELIM_I915_ENGINE_CLASS_COMPUTE] =
			XEHPSDV_CTX_CCS_PWR_CLK_STATE;
		break;
	case INTEL_DG2:
	case INTEL_METEORLAKE:
		perf->ctx_pwr_clk_state_offset[PRELIM_I915_ENGINE_CLASS_COMPUTE] =
			CTX_R_PWR_CLK_STATE;
		break;
	case INTEL_PONTEVECCHIO:
		perf->ctx_pwr_clk_state_offset[PRELIM_I915_ENGINE_CLASS_COMPUTE] =
			PVC_CTX_CCS_PWR_CLK_STATE;
		break;
	default:
		break;
	}
}

static void i915_perf_init_info(struct drm_i915_private *i915)
{
	struct i915_perf *perf = &i915->perf;
	u16 class = I915_ENGINE_CLASS_RENDER;

	perf->ctx_pwr_clk_state_offset[class] = CTX_R_PWR_CLK_STATE;
	perf->gen8_valid_ctx_bit = BIT(16);

	/*
	 * Calculate offset at runtime in oa_pin_context for gen12 and
	 * cache the value in perf->ctx_oactxctrl_offset array that is
	 * indexed using the uabi engine class.
	 */
	gen12_init_info(i915);
}

/**
 * i915_perf_init - initialize i915-perf state on module bind
 * @i915: i915 device instance
 *
 * Initializes i915-perf state without exposing anything to userspace.
 *
 * Note: i915-perf initialization is split into an 'init' and 'register'
 * phase with the i915_perf_register() exposing state to userspace.
 */
int i915_perf_init(struct drm_i915_private *i915)
{
	struct i915_perf *perf = &i915->perf;

	/* XXX const struct i915_perf_ops! */
	if (IS_SRIOV_VF(i915))
		return 0;

	perf->oa_formats = oa_formats;
	/* Note: that although we could theoretically also support the
	 * legacy ringbuffer mode on BDW (and earlier iterations of
	 * this driver, before upstreaming did this) it didn't seem
	 * worth the complexity to maintain now that BDW+ enable
	 * execlist mode by default.
	 */
	perf->ops.read = gen8_oa_read;
	i915_perf_init_info(i915);

	perf->ops.is_valid_b_counter_reg =
		HAS_OA_SLICE_CONTRIB_LIMITS(i915) ?
		xehp_is_valid_b_counter_addr :
		gen12_is_valid_b_counter_addr;
	perf->ops.is_valid_mux_reg =
		gen12_is_valid_mux_addr;
	perf->ops.is_valid_flex_reg =
		gen8_is_valid_flex_addr;

	perf->ops.oa_enable = gen12_oa_enable;
	perf->ops.oa_disable = gen12_oa_disable;
	perf->ops.enable_metric_set = gen12_enable_metric_set;
	perf->ops.disable_metric_set = gen12_disable_metric_set;
	perf->ops.oa_hw_tail_read = gen12_oa_hw_tail_read;

	if (perf->ops.enable_metric_set) {
		struct intel_gt *gt;
		int i, ret;

		for_each_gt(gt, i915, i)
			mutex_init(&gt->perf.lock);

		/* Choose a representative limit */
		oa_sample_rate_hard_limit = to_gt(i915)->clock_frequency / 2;

		mutex_init(&perf->metrics_lock);
		idr_init_base(&perf->metrics_idr, 1);

		/* We set up some ratelimit state to potentially throttle any
		 * _NOTES about spurious, invalid OA reports which we don't
		 * forward to userspace.
		 *
		 * We print a _NOTE about any throttling when closing the
		 * stream instead of waiting until driver _fini which no one
		 * would ever see.
		 *
		 * Using the same limiting factors as printk_ratelimit()
		 */
		ratelimit_state_init(&perf->spurious_report_rs, 5 * HZ, 10);
		/* Since we use a DRM_NOTE for spurious reports it would be
		 * inconsistent to let __ratelimit() automatically print a
		 * warning for throttling.
		 */
		ratelimit_set_flags(&perf->spurious_report_rs,
				    RATELIMIT_MSG_ON_RELEASE);

		ratelimit_state_init(&perf->tail_pointer_race,
				     5 * HZ, 10);
		ratelimit_set_flags(&perf->tail_pointer_race,
				    RATELIMIT_MSG_ON_RELEASE);

		atomic64_set(&perf->noa_programming_delay,
			     500 * 1000 /* 500us */);

		perf->i915 = i915;

		ret = oa_init_engine_groups(perf);
		if (ret) {
			drm_err(&i915->drm,
				"OA initialization failed %d\n", ret);
			return ret;
		}

		oa_init_supported_formats(perf);
		perf->default_ci.engine_class = oa_init_default_class(perf);
	}

	return 0;
}

static int destroy_config(int id, void *p, void *data)
{
	i915_oa_config_put(p);
	return 0;
}

int i915_perf_sysctl_register(void)
{
#ifdef BPM_REGISTER_SYSCTL_TABLE_NOT_PRESENT
	sysctl_header = register_sysctl("dev/i915", oa_table);
#else
	sysctl_header = register_sysctl_table(dev_root);
#endif
	return 0;
}

void i915_perf_sysctl_unregister(void)
{
	unregister_sysctl_table(sysctl_header);
}

/**
 * i915_perf_fini - Counter part to i915_perf_init()
 * @i915: i915 device instance
 */
void i915_perf_fini(struct drm_i915_private *i915)
{
	struct i915_perf *perf = &i915->perf;
	struct intel_gt *gt;
	int i;

	if (!perf->i915)
		return;

	for_each_gt(gt, perf->i915, i)
		kfree(gt->perf.group);

	idr_for_each(&perf->metrics_idr, destroy_config, perf);
	idr_destroy(&perf->metrics_idr);

	memset(&perf->ops, 0, sizeof(perf->ops));
	perf->i915 = NULL;
}

/**
 * i915_perf_ioctl_version - Version of the i915-perf subsystem
 *
 * This version number is used by userspace to detect available features.
 */
int i915_perf_ioctl_version(void)
{
	/*
	 * 1: Initial version
	 *   I915_PERF_IOCTL_ENABLE
	 *   I915_PERF_IOCTL_DISABLE
	 *
	 * 2: Added runtime modification of OA config.
	 *   I915_PERF_IOCTL_CONFIG
	 *
	 * 3: Add DRM_I915_PERF_PROP_HOLD_PREEMPTION parameter to hold
	 *    preemption on a particular context so that performance data is
	 *    accessible from a delta of MI_RPC reports without looking at the
	 *    OA buffer.
	 *
	 * 4: Add DRM_I915_PERF_PROP_ALLOWED_SSEU to limit what contexts can
	 *    be run for the duration of the performance recording based on
	 *    their SSEU configuration.
	 *
	 * 5: Add DRM_I915_PERF_PROP_POLL_OA_PERIOD parameter that controls the
	 *    interval for the hrtimer used to check for OA data.
	 *
	 * 6: Whitelist OATRIGGER registers to allow user to trigger reports
	 *    into the OA buffer. This applies only to gen8+. The feature can
	 *    only be accessed if perf_stream_paranoid is set to 0 by privileged
	 *    user.
	 *
	 * 7: Whitelist below OA registers for user to identify the location of
	 *    triggered reports in the OA buffer. This applies only to gen8+.
	 *    The feature can only be accessed if perf_stream_paranoid is set to
	 *    0 by privileged user.
	 *
	 *    - OA buffer head/tail/status/buffer registers for read only
	 *    - OA counters A18, A19, A20 for read/write
	 *
	 * 1000: Added an option to map oa buffer at umd driver level and trigger
	 *       oa reports within oa buffer from command buffer. See
	 *       PRELIM_I915_PERF_IOCTL_GET_OA_BUFFER_INFO.
	 *
	 * 1001: PRELIM_DRM_I915_PERF_PROP_OA_BUFFER_SIZE so user can
	 *	 configure the OA buffer size. Sizes are configured as
	 *	 powers of 2 ranging from 128kb to maximum size supported
	 *	 by the platforms. Max size supported is 16Mb before
	 *	 XEHPSDV. From XEHPSDV onwards, it is 128Mb.
	 *
	 * 1002: Add PRELIM_DRM_I915_PERF_PROP_OA_ENGINE_CLASS and
	 *	 PRELIM_DRM_I915_PERF_PROP_OA_ENGINE_INSTANCE
	 *
	 * 1003: Add perf record type - PRELIM_DRM_I915_PERF_RECORD_OA_MMIO_TRG_Q_FULL
	 *
	 * 1004: Add support for video decode and enhancement classes.
	 *
	 * 1005: Supports OAC and hence MI_REPORT_PERF_COUNTER for compute class.
	 *
	 * 1006: Added support for EU stall monitoring.
	 *
	 * 1007: Added support for MPES configuration.
	 *
	 * 1008: Added support for throttling poll.
	 */
	return 1008;
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftests/i915_perf.c"
#endif
