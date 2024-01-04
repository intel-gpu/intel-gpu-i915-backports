#ifndef _BACKPORT_LINUX_HDMI_H_
#define _BACKPORT_LINUX_HDMI_H_
#include <linux/version.h>
#include_next <linux/hdmi.h>

#ifdef BPM_HDMI_DRM_INFOFRAME_UNPACK_NOT_PRESENT
#define hdmi_drm_infoframe_unpack_only LINUX_I915_BACKPORT(hdmi_drm_infoframe_unpack_only)
int hdmi_drm_infoframe_unpack_only(struct hdmi_drm_infoframe *frame,
                                   const void *buffer, size_t size);
#endif
#endif /*_BACKPORT_LINUX_HDMI_H_*/
