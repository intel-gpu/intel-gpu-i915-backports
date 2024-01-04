#ifndef __BACKPORT_GENERATED_UTS_RELEASE_H
#define __BACKPORT_GENERATED_UTS_RELEASE_H

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
#define UTS_RELEASE "2.6.32"
#else
#include_next <generated/utsrelease.h>
#endif

/*
 * We only want the UTS_UBUNTU_RELEASE_ABI var when we are on a normal
 * Ubuntu distribution kernel and not when we are on a Ubuntu mainline
 * kernel. Some of the Ubuntu mainline kernel do have an invalid octal
 * number in this field like 031418 and we do not want to evaluate this
 * at all on the Ubuntu mainline kernels.  All Ubuntu distribution
 * kernel have CONFIG_VERSION_SIGNATURE set so this way we can detect
 * the which type of kernel we are on.
 */
#ifndef UTS_UBUNTU_RELEASE_ABI
#define UTS_UBUNTU_RELEASE_ABI 0
#elif !defined(CONFIG_VERSION_SIGNATURE)
#undef UTS_UBUNTU_RELEASE_ABI
#define UTS_UBUNTU_RELEASE_ABI 0
#endif

#endif /* __BACKPORT_GENERATED_UTS_RELEASE_H */
