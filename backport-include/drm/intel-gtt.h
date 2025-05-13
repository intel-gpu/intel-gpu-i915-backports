#ifndef __BACKPORT_INTEL_GTT_H
#define __BACKPORT_INTEL_GTT_H

#ifdef BPM_DRM_INTEL_HEADERS_NOT_PRESENT
#include_next <drm/intel/intel-gtt.h>
#else
#include_next <drm/intel-gtt.h>
#endif

#endif
