/*
 * Copyright © 2015 Intel Corporation
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
 * Authors:
 *    Owen Zhang <owen.zhang@intel.com>
 *
 */

#ifndef _BACKPORT_LINUX_PAGEVEC_H
#define _BACKPORT_LINUX_PAGEVEC_H
#include <linux/types.h>
#include_next <linux/pagevec.h>

#ifdef BPM_PAGEVEC_NOT_PRESENT

struct pagevec {
        unsigned char nr;
        bool percpu_pvec_drained;
        struct page *pages[PAGEVEC_SIZE];
};

void __pagevec_release(struct pagevec *pvec);

static inline void pagevec_init(struct pagevec *pvec)
{
        pvec->nr = 0;
        pvec->percpu_pvec_drained = false;
}

static inline void pagevec_reinit(struct pagevec *pvec)
{
        pvec->nr = 0;
}

static inline unsigned pagevec_count(struct pagevec *pvec)
{
        return pvec->nr;
}

static inline unsigned pagevec_space(struct pagevec *pvec)
{
        return PAGEVEC_SIZE - pvec->nr;
}

/*
 * Add a page to a pagevec.  Returns the number of slots still available.
 */
static inline unsigned pagevec_add(struct pagevec *pvec, struct page *page)
{
        pvec->pages[pvec->nr++] = page;
        return pagevec_space(pvec);
}

static inline void pagevec_release(struct pagevec *pvec)
{
        if (pagevec_count(pvec))
                __pagevec_release(pvec);
}
#endif
#endif
