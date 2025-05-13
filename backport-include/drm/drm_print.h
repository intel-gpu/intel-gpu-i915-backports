#ifndef _BACKPORT_DRM_PRINT_H_
#define _BACKPORT_DRM_PRINT_H_

#include_next <drm/drm_print.h>

#ifdef BPM_DEBUGFS_CREATE_APIS_NOT_PRESENT
#include <linux/debugfs.h>
#endif

#ifdef BPM_DRM_DEBUG_PRINTER_NOT_PRESENT
static inline struct drm_printer drm_debug_printer(const char *prefix)
{
	return drm_dbg_printer (NULL, DRM_UT_DRIVER, prefix);
}
#endif


#ifdef BPM_DRM_ERR_PRINTER_SECOND_ARG_PRESENT
#define drm_err_printer(a) drm_err_printer(NULL,a)
#endif 


#endif /*_BACKPORT_DRM_PRINT_H_ */
