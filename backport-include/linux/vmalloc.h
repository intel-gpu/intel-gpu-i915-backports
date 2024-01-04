/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BACKPORT_LINUX_VMALLOC_H
#define _BACKPORT_LINUX_VMALLOC_H

#include <linux/version.h>
#include_next <linux/vmalloc.h>
#ifdef BPM_VM_MAP_PUT_PAGES_NOT_PRESENT
/* bits in flags of vmalloc's vm_struct below */
#define VM_MAP_PUT_PAGES        0x00000100      /* put pages and free array in vfree */
#endif
#endif /* _LINUX_VMALLOC_H */

