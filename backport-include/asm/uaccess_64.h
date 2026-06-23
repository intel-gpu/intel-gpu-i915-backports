/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BACKPORT_X86_UACCESS_64_H
#define _BACKPORT_X86_UACCESS_64_H

#include_next <asm/uaccess_64.h>

#ifdef BPM_COPY_FROM_USER_INATOMIC_NOCACHE_NOT_PRESENT
#define __copy_from_user_inatomic_nocache copy_from_user_inatomic_nontemporal
#endif
#endif
