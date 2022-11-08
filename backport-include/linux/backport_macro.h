#ifndef _BP_LINUX_BACKPORT_MACRO_H
#define _BP_LINUX_BACKPORT_MACRO_H
#include <linux/version.h>

#if LINUX_VERSION_IS_LESS(5,8,0)

/*
 * 479da1f538a2 backlight: Add backlight_device_get_by_name()
 */
#define BACKLIGHT_DEV_GET_BY_NAME_NOT_PRESENT

#endif

/*
 * 64fa30f9ffc0ed Backport and fix intel-gtt split
 * intl_gtt Api name has been changed to intel_gmch_gtt
 * so remapping names to older API via backported header.
 */

#define INTEL_GMCH_GTT_RENAMED

/*
 * Disable Lowmem reservation for dg1
 */
#define BPC_LOWMEM_FOR_DG1_NOT_SUPPORTED

#endif /* _BP_LINUX_BACKPORT_MACRO_H */

