/*
 * Copyright _ 2006-2009, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 *
 * Author: Anil S Keshavamurthy <anil.s.keshavamurthy@intel.com>
 */

#include <linux/iova.h>

#ifdef BPM_ALLOC_IOVA_FAST_EXPORT_NOT_PRESENT
static inline void free_cpu_cached_iovas(unsigned int cpu,
                                         struct iova_domain *iovad)
{
}

/**
 * alloc_iova_fast - allocates an iova from rcache
 * @iovad: - iova domain in question
 * @size: - size of page frames to allocate
 * @limit_pfn: - max limit address
 * @flush_rcache: - set to flush rcache on regular allocation failure
 * This function tries to satisfy an iova allocation from the rcache,
 * and falls back to regular allocation on failure. If regular allocation
 * fails too and the flush_rcache flag is set then the rcache will be flushed.
*/
unsigned long
alloc_iova_fast(struct iova_domain *iovad, unsigned long size,
                unsigned long limit_pfn, bool flush_rcache)
{
        struct iova *new_iova;

retry:
        new_iova = alloc_iova(iovad, size, limit_pfn, true);
        if (!new_iova) {
                unsigned int cpu;

                if (!flush_rcache)
                        return 0;

                /* Try replenishing IOVAs by flushing rcache. */
                flush_rcache = false;
                for_each_online_cpu(cpu)
                        free_cpu_cached_iovas(cpu, iovad);
                goto retry;
        }

        return new_iova->pfn_lo;
}
EXPORT_SYMBOL_GPL(alloc_iova_fast);

/**
 * free_iova_fast - free iova pfn range into rcache
 * @iovad: - iova domain in question.
 * @pfn: - pfn that is allocated previously
 * @size: - # of pages in range
 * This functions frees an iova range by trying to put it into the rcache,
 * falling back to regular iova deallocation via free_iova() if this fails.
 */
void
free_iova_fast(struct iova_domain *iovad, unsigned long pfn, unsigned long size)
{
        free_iova(iovad, pfn);
}
EXPORT_SYMBOL_GPL(free_iova_fast);
#endif
