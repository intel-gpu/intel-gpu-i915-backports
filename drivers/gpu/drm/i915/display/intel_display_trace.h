/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright © 2021 Intel Corporation
 */

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
#undef TRACE_SYSTEM
#define TRACE_SYSTEM i915

#if !defined(__INTEL_DISPLAY_TRACE_H__) || defined(TRACE_HEADER_MULTI_READ)
#define __INTEL_DISPLAY_TRACE_H__

#include <linux/string_helpers.h>
#include <linux/types.h>
#include <linux/string_helpers.h>
#include <linux/tracepoint.h>
#include <backport/backport_path.h>

#include "i915_drv.h"
#include "i915_irq.h"
#include "intel_crtc.h"
#include "intel_display_types.h"

TRACE_EVENT(intel_pipe_enable,
	    TP_PROTO(struct intel_crtc *crtc),
	    TP_ARGS(crtc),

	    TP_STRUCT__entry(
			     __array(u32, frame, 3)
			     __array(u32, scanline, 3)
			     __field(enum pipe, pipe)
			     ),
	    TP_fast_assign(
			   struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
			   struct intel_crtc *it__;
			   for_each_intel_crtc(&dev_priv->drm, it__) {
				   __entry->frame[it__->pipe] = intel_crtc_get_vblank_counter(it__);
				   __entry->scanline[it__->pipe] = intel_get_crtc_scanline(it__);
			   }
			   __entry->pipe = crtc->pipe;
			   ),

	    TP_printk("pipe %c enable, pipe A: frame=%u, scanline=%u, pipe B: frame=%u, scanline=%u, pipe C: frame=%u, scanline=%u",
		      pipe_name(__entry->pipe),
		      __entry->frame[PIPE_A], __entry->scanline[PIPE_A],
		      __entry->frame[PIPE_B], __entry->scanline[PIPE_B],
		      __entry->frame[PIPE_C], __entry->scanline[PIPE_C])
);

TRACE_EVENT(intel_pipe_disable,
	    TP_PROTO(struct intel_crtc *crtc),
	    TP_ARGS(crtc),

	    TP_STRUCT__entry(
			     __array(u32, frame, 3)
			     __array(u32, scanline, 3)
			     __field(enum pipe, pipe)
			     ),

	    TP_fast_assign(
			   struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
			   struct intel_crtc *it__;
			   for_each_intel_crtc(&dev_priv->drm, it__) {
				   __entry->frame[it__->pipe] = intel_crtc_get_vblank_counter(it__);
				   __entry->scanline[it__->pipe] = intel_get_crtc_scanline(it__);
			   }
			   __entry->pipe = crtc->pipe;
			   ),

	    TP_printk("pipe %c disable, pipe A: frame=%u, scanline=%u, pipe B: frame=%u, scanline=%u, pipe C: frame=%u, scanline=%u",
		      pipe_name(__entry->pipe),
		      __entry->frame[PIPE_A], __entry->scanline[PIPE_A],
		      __entry->frame[PIPE_B], __entry->scanline[PIPE_B],
		      __entry->frame[PIPE_C], __entry->scanline[PIPE_C])
);

TRACE_EVENT(intel_pipe_crc,
	    TP_PROTO(struct intel_crtc *crtc, const u32 *crcs),
	    TP_ARGS(crtc, crcs),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(u32, frame)
			     __field(u32, scanline)
			     __array(u32, crcs, 5)
			     ),

	    TP_fast_assign(
			   __entry->pipe = crtc->pipe;
			   __entry->frame = intel_crtc_get_vblank_counter(crtc);
			   __entry->scanline = intel_get_crtc_scanline(crtc);
			   memcpy(__entry->crcs, crcs, sizeof(__entry->crcs));
			   ),

	    TP_printk("pipe %c, frame=%u, scanline=%u crc=%08x %08x %08x %08x %08x",
		      pipe_name(__entry->pipe), __entry->frame, __entry->scanline,
		      __entry->crcs[0], __entry->crcs[1], __entry->crcs[2],
		      __entry->crcs[3], __entry->crcs[4])
);

TRACE_EVENT(intel_plane_update_noarm,
	    TP_PROTO(struct drm_plane *plane, struct intel_crtc *crtc),
	    TP_ARGS(plane, crtc),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(u32, frame)
			     __field(u32, scanline)
			     __array(int, src, 4)
			     __array(int, dst, 4)
			     __string(name, plane->name)
			     ),

	    TP_fast_assign(
			   __assign_str(name, plane->name);
			   __entry->pipe = crtc->pipe;
			   __entry->frame = intel_crtc_get_vblank_counter(crtc);
			   __entry->scanline = intel_get_crtc_scanline(crtc);
			   memcpy(__entry->src, &plane->state->src, sizeof(__entry->src));
			   memcpy(__entry->dst, &plane->state->dst, sizeof(__entry->dst));
			   ),

	    TP_printk("pipe %c, plane %s, frame=%u, scanline=%u, " DRM_RECT_FP_FMT " -> " DRM_RECT_FMT,
		      pipe_name(__entry->pipe), __get_str(name),
		      __entry->frame, __entry->scanline,
		      DRM_RECT_FP_ARG((const struct drm_rect *)__entry->src),
		      DRM_RECT_ARG((const struct drm_rect *)__entry->dst))
);

TRACE_EVENT(intel_plane_update_arm,
	    TP_PROTO(struct drm_plane *plane, struct intel_crtc *crtc),
	    TP_ARGS(plane, crtc),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(u32, frame)
			     __field(u32, scanline)
			     __array(int, src, 4)
			     __array(int, dst, 4)
			     __string(name, plane->name)
			     ),

	    TP_fast_assign(
			   __assign_str(name, plane->name);
			   __entry->pipe = crtc->pipe;
			   __entry->frame = intel_crtc_get_vblank_counter(crtc);
			   __entry->scanline = intel_get_crtc_scanline(crtc);
			   memcpy(__entry->src, &plane->state->src, sizeof(__entry->src));
			   memcpy(__entry->dst, &plane->state->dst, sizeof(__entry->dst));
			   ),

	    TP_printk("pipe %c, plane %s, frame=%u, scanline=%u, " DRM_RECT_FP_FMT " -> " DRM_RECT_FMT,
		      pipe_name(__entry->pipe), __get_str(name),
		      __entry->frame, __entry->scanline,
		      DRM_RECT_FP_ARG((const struct drm_rect *)__entry->src),
		      DRM_RECT_ARG((const struct drm_rect *)__entry->dst))
);

TRACE_EVENT(intel_plane_disable_arm,
	    TP_PROTO(struct drm_plane *plane, struct intel_crtc *crtc),
	    TP_ARGS(plane, crtc),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(u32, frame)
			     __field(u32, scanline)
			     __string(name, plane->name)
			     ),

	    TP_fast_assign(
			   __assign_str(name, plane->name);
			   __entry->pipe = crtc->pipe;
			   __entry->frame = intel_crtc_get_vblank_counter(crtc);
			   __entry->scanline = intel_get_crtc_scanline(crtc);
			   ),

	    TP_printk("pipe %c, plane %s, frame=%u, scanline=%u",
		      pipe_name(__entry->pipe), __get_str(name),
		      __entry->frame, __entry->scanline)
);

TRACE_EVENT(intel_fbc_activate,
	    TP_PROTO(struct intel_plane *plane),
	    TP_ARGS(plane),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(u32, frame)
			     __field(u32, scanline)
			     ),

	    TP_fast_assign(
			   struct intel_crtc *crtc = intel_crtc_for_pipe(to_i915(plane->base.dev),
									 plane->pipe);
			   __entry->pipe = crtc->pipe;
			   __entry->frame = intel_crtc_get_vblank_counter(crtc);
			   __entry->scanline = intel_get_crtc_scanline(crtc);
			   ),

	    TP_printk("pipe %c, frame=%u, scanline=%u",
		      pipe_name(__entry->pipe), __entry->frame, __entry->scanline)
);

TRACE_EVENT(intel_fbc_deactivate,
	    TP_PROTO(struct intel_plane *plane),
	    TP_ARGS(plane),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(u32, frame)
			     __field(u32, scanline)
			     ),

	    TP_fast_assign(
			   struct intel_crtc *crtc = intel_crtc_for_pipe(to_i915(plane->base.dev),
									 plane->pipe);
			   __entry->pipe = crtc->pipe;
			   __entry->frame = intel_crtc_get_vblank_counter(crtc);
			   __entry->scanline = intel_get_crtc_scanline(crtc);
			   ),

	    TP_printk("pipe %c, frame=%u, scanline=%u",
		      pipe_name(__entry->pipe), __entry->frame, __entry->scanline)
);

TRACE_EVENT(intel_fbc_nuke,
	    TP_PROTO(struct intel_plane *plane),
	    TP_ARGS(plane),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(u32, frame)
			     __field(u32, scanline)
			     ),

	    TP_fast_assign(
			   struct intel_crtc *crtc = intel_crtc_for_pipe(to_i915(plane->base.dev),
									 plane->pipe);
			   __entry->pipe = crtc->pipe;
			   __entry->frame = intel_crtc_get_vblank_counter(crtc);
			   __entry->scanline = intel_get_crtc_scanline(crtc);
			   ),

	    TP_printk("pipe %c, frame=%u, scanline=%u",
		      pipe_name(__entry->pipe), __entry->frame, __entry->scanline)
);

TRACE_EVENT(intel_crtc_vblank_work_start,
	    TP_PROTO(struct intel_crtc *crtc),
	    TP_ARGS(crtc),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(u32, frame)
			     __field(u32, scanline)
			     ),

	    TP_fast_assign(
			   __entry->pipe = crtc->pipe;
			   __entry->frame = intel_crtc_get_vblank_counter(crtc);
			   __entry->scanline = intel_get_crtc_scanline(crtc);
			   ),

	    TP_printk("pipe %c, frame=%u, scanline=%u",
		      pipe_name(__entry->pipe), __entry->frame,
		       __entry->scanline)
);

TRACE_EVENT(intel_crtc_vblank_work_end,
	    TP_PROTO(struct intel_crtc *crtc),
	    TP_ARGS(crtc),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(u32, frame)
			     __field(u32, scanline)
			     ),

	    TP_fast_assign(
			   __entry->pipe = crtc->pipe;
			   __entry->frame = intel_crtc_get_vblank_counter(crtc);
			   __entry->scanline = intel_get_crtc_scanline(crtc);
			   ),

	    TP_printk("pipe %c, frame=%u, scanline=%u",
		      pipe_name(__entry->pipe), __entry->frame,
		       __entry->scanline)
);

TRACE_EVENT(intel_pipe_update_start,
	    TP_PROTO(struct intel_crtc *crtc),
	    TP_ARGS(crtc),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(u32, frame)
			     __field(u32, scanline)
			     __field(u32, min)
			     __field(u32, max)
			     ),

	    TP_fast_assign(
			   __entry->pipe = crtc->pipe;
			   __entry->frame = intel_crtc_get_vblank_counter(crtc);
			   __entry->scanline = intel_get_crtc_scanline(crtc);
			   __entry->min = crtc->debug.min_vbl;
			   __entry->max = crtc->debug.max_vbl;
			   ),

	    TP_printk("pipe %c, frame=%u, scanline=%u, min=%u, max=%u",
		      pipe_name(__entry->pipe), __entry->frame,
		       __entry->scanline, __entry->min, __entry->max)
);

TRACE_EVENT(intel_pipe_update_vblank_evaded,
	    TP_PROTO(struct intel_crtc *crtc),
	    TP_ARGS(crtc),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(u32, frame)
			     __field(u32, scanline)
			     __field(u32, min)
			     __field(u32, max)
			     ),

	    TP_fast_assign(
			   __entry->pipe = crtc->pipe;
			   __entry->frame = crtc->debug.start_vbl_count;
			   __entry->scanline = crtc->debug.scanline_start;
			   __entry->min = crtc->debug.min_vbl;
			   __entry->max = crtc->debug.max_vbl;
			   ),

	    TP_printk("pipe %c, frame=%u, scanline=%u, min=%u, max=%u",
		      pipe_name(__entry->pipe), __entry->frame,
		       __entry->scanline, __entry->min, __entry->max)
);

TRACE_EVENT(intel_pipe_update_end,
	    TP_PROTO(struct intel_crtc *crtc, u32 frame, int scanline_end),
	    TP_ARGS(crtc, frame, scanline_end),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(u32, frame)
			     __field(u32, scanline)
			     ),

	    TP_fast_assign(
			   __entry->pipe = crtc->pipe;
			   __entry->frame = frame;
			   __entry->scanline = scanline_end;
			   ),

	    TP_printk("pipe %c, frame=%u, scanline=%u",
		      pipe_name(__entry->pipe), __entry->frame,
		      __entry->scanline)
);

TRACE_EVENT(intel_frontbuffer_invalidate,
	    TP_PROTO(unsigned int frontbuffer_bits, unsigned int origin),
	    TP_ARGS(frontbuffer_bits, origin),

	    TP_STRUCT__entry(
			     __field(unsigned int, frontbuffer_bits)
			     __field(unsigned int, origin)
			     ),

	    TP_fast_assign(
			   __entry->frontbuffer_bits = frontbuffer_bits;
			   __entry->origin = origin;
			   ),

	    TP_printk("frontbuffer_bits=0x%08x, origin=%u",
		      __entry->frontbuffer_bits, __entry->origin)
);

TRACE_EVENT(intel_frontbuffer_flush,
	    TP_PROTO(unsigned int frontbuffer_bits, unsigned int origin),
	    TP_ARGS(frontbuffer_bits, origin),

	    TP_STRUCT__entry(
			     __field(unsigned int, frontbuffer_bits)
			     __field(unsigned int, origin)
			     ),

	    TP_fast_assign(
			   __entry->frontbuffer_bits = frontbuffer_bits;
			   __entry->origin = origin;
			   ),

	    TP_printk("frontbuffer_bits=0x%08x, origin=%u",
		      __entry->frontbuffer_bits, __entry->origin)
);

#endif /* __INTEL_DISPLAY_TRACE_H__ */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_PATH BACKPORT_PATH/drivers/gpu/drm/i915/display
#define TRACE_INCLUDE_FILE intel_display_trace
#include <trace/define_trace.h>
#endif /* CPTCFG_DRM_I915_DISPLAY */
