#ifndef _BACKPORT_LINUX_HDMI_H_
#define _BACKPORT_LINUX_HDMI_H_
#include <linux/version.h>
#include_next <linux/hdmi.h>

#if LINUX_VERSION_IS_LESS(5,8,0)
int hdmi_drm_infoframe_unpack_only(struct hdmi_drm_infoframe *frame,
                                   const void *buffer, size_t size);
#endif /*LINUX_VERSION_IS_LESS(5,8,0)*/
#endif /*_BACKPORT_LINUX_HDMI_H_*/

