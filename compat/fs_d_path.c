#include <linux/module.h>
#include <linux/slab.h>

/*
 * Helper function for dentry_operations.d_dname() members
 */
char *dynamic_dname(struct dentry *dentry, char *buffer, int buflen,
                const char *fmt, ...)
{
        va_list args;
        char temp[64];
        int sz;

        va_start(args, fmt);
        sz = vsnprintf(temp, sizeof(temp), fmt, args) + 1;
        va_end(args);

        if (sz > sizeof(temp) || sz > buflen)
                return ERR_PTR(-ENAMETOOLONG);

        buffer += buflen - sz;
        return memcpy(buffer, temp, sz);
}

