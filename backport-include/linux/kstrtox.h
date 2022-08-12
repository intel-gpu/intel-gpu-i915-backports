/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KSTRTOX_H
#define _LINUX_KSTRTOX_H

#include <linux/compiler.h>
#include <linux/types.h>

extern int kstrtobool(const char *s, bool *res);

static inline int strtobool(const char *s, bool *res)
{
        return kstrtobool(s, res);
}

#endif /* _LINUX_KSTRTOX_H */
