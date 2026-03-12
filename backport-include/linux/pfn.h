#ifndef __BACKPORT_LINUX_PFN_H
#define __BACKPORT_LINUX_PFN_H
#include_next <linux/pfn.h>

#ifdef BPM_PFN_T_NOT_PRESENT
#ifndef __ASSEMBLY__
/*
 * pfn_t: encapsulates a page-frame number that is optionally backed
 * by memmap (struct page).  Whether a pfn_t has a 'struct page'
 * backing is indicated by flags in the high bits of the value.
 */
typedef struct {
	u64 val;
} pfn_t;
#endif
#endif

#endif /* __BACKPORT_LINUX_PFN_H */
