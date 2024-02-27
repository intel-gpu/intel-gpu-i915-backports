#ifndef __BACKPORT_H
#define __BACKPORT_H
#include <generated/autoconf.h>
#ifndef CONFIG_BACKPORT_INTEGRATE
#include <backport/autoconf.h>
#include <backport/backport_macro.h>
#endif
#include <linux/kconfig.h>

#ifndef __ASSEMBLY_
#define LINUX_DMABUF_BACKPORT(__sym) dmabufbkpt_ ##__sym
#define LINUX_I915_BACKPORT(__sym) i915bkpt_ ##__sym
#ifndef CONFIG_BACKPORT_INTEGRATE
#include <backport/checks.h>
#endif
#endif

#endif /* __BACKPORT_H */
