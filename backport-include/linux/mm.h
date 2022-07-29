/*
 * Copyright © 2022 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 *
 */

#ifndef _BACKPORT_LINUX_MM_H
#define _BACKPORT_LINUX_MM_H
#include <linux/version.h>
#include <linux/pagevec.h>
#include <linux/kref.h>

#include_next <linux/mm.h>

#define FOLL_REMOTE     0x2000  /* we are working on non-current tsk/mm */
extern void *kvmalloc_node(size_t size, gfp_t flags, int node);
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4,18,0))
#if RHEL_RELEASE_CODE != RHEL_RELEASE_VERSION(7,5)
static inline void *kvmalloc(size_t size, gfp_t flags)
{
        return kvmalloc_node(size, flags, NUMA_NO_NODE);
}
static inline void *kvzalloc_node(size_t size, gfp_t flags, int node)
{
        return kvmalloc_node(size, flags | __GFP_ZERO, node);
}
static inline void *kvzalloc(size_t size, gfp_t flags)
{
        return kvmalloc(size, flags | __GFP_ZERO);
}

static inline void *kvmalloc_array(size_t n, size_t size, gfp_t flags)
{
        if (size != 0 && n > SIZE_MAX / size)
                return NULL;

        return kvmalloc(n * size, flags);
}
#endif

static inline void mmgrab(struct mm_struct *mm)
{
        atomic_inc(&mm->mm_count);
}

#if RHEL_RELEASE_CODE != RHEL_RELEASE_VERSION(7,5)
long get_user_pages_remote(struct task_struct *tsk, struct mm_struct *mm,
                            unsigned long start, unsigned long nr_pages,
                            unsigned int gup_flags, struct page **pages,
                            struct vm_area_struct **vmas, int *locked);
#endif
#endif /* (LINUX_VERSION_CODE < KERNEL_VERSION(4,18,0))*/

#if LINUX_VERSION_IS_LESS(5,10,0)
#define vma_set_file LINUX_DMABUF_BACKPORT(vma_set_file)
void vma_set_file(struct vm_area_struct *vma, struct file *file);
#endif

#endif /* _BACKPORT_LINUX_MM_H */
