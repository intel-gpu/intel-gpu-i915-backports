#ifndef _BP_LINUX_BACKPORT_MACRO_H
#define _BP_LINUX_BACKPORT_MACRO_H

/*
 * 64fa30f9ffc0ed Backport and fix intel-gtt split
 * intl_gtt Api name has been changed to intel_gmch_gtt
 * so remapping names to older API via backported header.
 */

#define INTEL_GMCH_GTT_RENAMED

#endif /* _BP_LINUX_BACKPORT_MACRO_H */

