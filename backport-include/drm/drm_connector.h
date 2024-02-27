/*
 * Copyright (c) 2016 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#ifndef __BACKPORT_DRM_CONNECTOR_H__
#define __BACKPORT_DRM_CONNECTOR_H__
#include_next <drm/drm_connector.h>

#ifdef BPM_DRM_MODE_CREATE_TV_PROP_NOT_PRESENT
#define drm_mode_create_tv_properties drm_mode_create_tv_properties_legacy
#endif

#ifdef BPM_SUPPORTED_COLORSPACES_ARG_NOT_PRESENT
#define drm_mode_create_hdmi_colorspace_property(connector) \
        drm_mode_create_hdmi_colorspace_property(connector, 0)

#define drm_mode_create_dp_colorspace_property(connector) \
        drm_mode_create_dp_colorspace_property(connector, 0)
#endif

#endif /* __BACKPORT_DRM_CONNECTOR_H__ */
