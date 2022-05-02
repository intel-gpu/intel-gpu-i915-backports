/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BACKPORT_LINUX_VMALLOC_H
#define _BACKPORT_LINUX_VMALLOC_H

#include <linux/version.h>
#include_next <linux/vmalloc.h>
#if (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(8,5) && LINUX_VERSION_CODE < KERNEL_VERSION(5,10,0))
/* bits in flags of vmalloc's vm_struct below */
#define VM_MAP_PUT_PAGES        0x00000100      /* put pages and free array in vfree */
#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(5,10,0) */
#endif /* _LINUX_VMALLOC_H */

