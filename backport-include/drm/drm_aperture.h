#ifndef __BACKPORT_DRM_APERTURE_H
#define __BACKPORT_DRM_APERTURE_H

#ifdef BPM_DRM_APERTURE_IS_NOT_PRESENT
#include_next <linux/aperture.h>
#else
#include_next <drm/drm_aperture.h>
#endif

#endif /* __BACKPORT_DRM_APERTURE_H */
