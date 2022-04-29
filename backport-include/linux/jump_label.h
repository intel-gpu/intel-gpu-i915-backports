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
 *
 */

#ifndef _BACKPORT_JUMP_LABEL_H
#define _BACKPORT_JUMP_LABEL_H
#include <linux/version.h>
#if RHEL_RELEASE_CODE == RHEL_RELEASE_VERSION(7,5)
#include <asm/atomic.h>
#include <asm/bug.h>
#endif
#include_next <linux/jump_label.h>


extern bool ____wrong_branch_error(void);
//extern int static_key_count(struct static_key *key);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,18,0)
static inline int static_key_count(struct static_key *key)
{
        return atomic_read(&key->enabled);
}

static inline void static_key_enable(struct static_key *key)
{
        int count = static_key_count(key);

        WARN_ON_ONCE(count < 0 || count > 1); 

        if (!count)
                static_key_slow_inc(key);
}

static inline void static_key_disable(struct static_key *key)
{
        int count = static_key_count(key);

        WARN_ON_ONCE(count < 0 || count > 1);

        if (count)
                static_key_slow_dec(key);
}

struct static_key_true {
        struct static_key key;
};

struct static_key_false {
        struct static_key key;
};

#define static_branch_likely(x)         likely(static_key_enabled(&(x)->key))
#define static_branch_unlikely(x)       unlikely(static_key_enabled(&(x)->key))
#endif

#define static_branch_enable(x)         static_key_enable(&(x)->key)
#define static_branch_disable(x)        static_key_disable(&(x)->key)

#define static_key_enabled(x)                                                   \
({                                                                              \
        if (!__builtin_types_compatible_p(typeof(*x), struct static_key) &&     \
            !__builtin_types_compatible_p(typeof(*x), struct static_key_true) &&\
            !__builtin_types_compatible_p(typeof(*x), struct static_key_false)) \
                ____wrong_branch_error();                                       \
        static_key_count((struct static_key *)x) > 0;                           \
})

#define JUMP_TYPE_FALSE         0UL
#define JUMP_TYPE_TRUE          1UL


#ifdef STATIC_KEY_INIT_TRUE
#undef STATIC_KEY_INIT_TRUE
#define STATIC_KEY_INIT_TRUE                                    \
	{ .enabled = { 1 },                                     \
	  .entries = (void *)JUMP_TYPE_TRUE }
#endif

#ifdef STATIC_KEY_INIT_FALSE
#undef STATIC_KEY_INIT_FALSE
#define STATIC_KEY_INIT_FALSE                                   \
	{ .enabled = { 0 },                                     \
	  .entries = (void *)JUMP_TYPE_FALSE }
#endif

#define STATIC_KEY_TRUE_INIT  (struct static_key_true) { .key = STATIC_KEY_INIT_TRUE,  }
#define STATIC_KEY_FALSE_INIT (struct static_key_false){ .key = STATIC_KEY_INIT_FALSE, }

#define DEFINE_STATIC_KEY_TRUE(name)    \
        struct static_key_true name = STATIC_KEY_TRUE_INIT

#define DECLARE_STATIC_KEY_TRUE(name)   \
        extern struct static_key_true name

#define DEFINE_STATIC_KEY_FALSE(name)   \
        struct static_key_false name = STATIC_KEY_FALSE_INIT

#define DECLARE_STATIC_KEY_FALSE(name)  \
        extern struct static_key_false name

#endif
