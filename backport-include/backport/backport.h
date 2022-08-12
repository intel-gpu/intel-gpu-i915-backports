#ifndef __BACKPORT_H
#define __BACKPORT_H
#include <generated/autoconf.h>
#ifndef CONFIG_BACKPORT_INTEGRATE
#include <backport/autoconf.h>
#include <linux/backport_macro.h>
#endif
#include <linux/kconfig.h>
#ifndef __ASSEMBLY__
#define LINUX_I915_BACKPORT(__sym) i915bkpt_ ##__sym
#define LINUX_DMABUF_BACKPORT(__sym) dmabufbkpt_ ##__sym
#endif

#endif /* __BACKPORT_H */
