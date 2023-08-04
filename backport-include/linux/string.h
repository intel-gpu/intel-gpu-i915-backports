#ifndef _BACKPORT_LINUX_STRING_H_
#define _BACKPORT_LINUX_STRING_H_
#include_next <linux/string.h>

#ifdef BPM_STR_HAS_PREFIX_NOT_PRESENT
static __always_inline size_t str_has_prefix(const char *str, const char *prefix)
{
        size_t len = strlen(prefix);
        return strncmp(str, prefix, len) == 0 ? len : 0;
}
#endif

#endif
