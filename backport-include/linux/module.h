#ifndef __BACKPORT_LINUX_MODULE_H
#define __BACKPORT_LINUX_MODULE_H
#include_next <linux/module.h>
#include <linux/rcupdate.h>

#ifdef BPM_OBJTOOL_COPY_ATTRIBUTE_NEEDED
/*
 * Objtool compatibility: __copy and ___ADDRESSABLE attributes.
 */
#ifndef __copy
#define __copy(symbol)                                  \
        __attribute__((__copy__(symbol)))
#endif

#ifndef ___ADDRESSABLE
#define ___ADDRESSABLE(sym, attrs) \
        static void * attrs __used __UNIQUE_ID(__PASTE(__addressable_,sym)) = (void *)&sym;
#define __CFI_ADDRESSABLE(sym, attrs) ___ADDRESSABLE(sym, attrs)
#endif
#endif /* BPM_OBJTOOL_COPY_ATTRIBUTE_NEEDED */

/*
 * The define overwriting module_init is based on the original module_init
 * which looks like this:
 * #define module_init(initfn)					\
 *	static inline initcall_t __inittest(void)		\
 *	{ return initfn; }					\
 *	int init_module(void) __attribute__((alias(#initfn)));
 *
 * To the call to the initfn we added the symbol dependency on compat
 * to make sure that compat.ko gets loaded for any compat modules.
 */
#ifndef BPM_DISABLE_DRM_DMABUF
#define dependency_symbol LINUX_DMABUF_BACKPORT(dependency_symbol)
extern void dependency_symbol(void);
#endif

#ifdef MODULE
#undef module_init
#ifndef BPM_DISABLE_DRM_DMABUF
#ifdef BPM_OBJTOOL_COPY_ATTRIBUTE_NEEDED
#define module_init(initfn)						\
	static inline initcall_t __maybe_unused __inittest(void)	\
	{ return initfn; }						\
	static int __init __init_backport(void)				\
	{								\
		dependency_symbol();					\
		return initfn();					\
	}								\
	int init_module(void) __copy(__init_backport)			\
		__attribute__((__alias__("__init_backport")));		\
	___ADDRESSABLE(init_module, __initdata);
#else
#define module_init(initfn)						\
	static int __init __init_backport(void)				\
	{								\
		dependency_symbol();					\
		return initfn();					\
	}								\
	int init_module(void) __attribute__((cold,alias("__init_backport")));
#endif

#else
#ifdef BPM_OBJTOOL_COPY_ATTRIBUTE_NEEDED
#define module_init(initfn)						\
	static inline initcall_t __maybe_unused __inittest(void)	\
	{ return initfn; }						\
	static int __init __init_backport(void)				\
	{								\
		return initfn();					\
	}								\
	int init_module(void) __copy(__init_backport)			\
		__attribute__((__alias__("__init_backport")));		\
	___ADDRESSABLE(init_module, __initdata);
#else
#define module_init(initfn)						\
	static int __init __init_backport(void)				\
	{								\
		return initfn();					\
	}								\
	int init_module(void) __attribute__((cold,alias("__init_backport")));
#endif
#endif
/*
 * The define overwriting module_exit is based on the original module_exit
 * which looks like this:
 * #define module_exit(exitfn)                                    \
 *         static inline exitcall_t __exittest(void)               \
 *         { return exitfn; }                                      \
 *         void cleanup_module(void) __attribute__((alias(#exitfn)));
 *
 * We replaced the call to the actual function exitfn() with a call to our
 * function which calls the original exitfn() and then rcu_barrier()
 *
 * As a module will not be unloaded that ofter it should not have a big
 * performance impact when rcu_barrier() is called on every module exit,
 * also when no kfree_rcu() backport is used in that module.
 */
#undef module_exit
#ifdef BPM_OBJTOOL_COPY_ATTRIBUTE_NEEDED
#define module_exit(exitfn)						\
	static inline exitcall_t __maybe_unused __exittest(void)	\
	{ return exitfn; }						\
	static void __exit __exit_compat(void)				\
	{								\
		exitfn();						\
		rcu_barrier();						\
	}								\
	void cleanup_module(void) __copy(__exit_compat)			\
		__attribute__((__alias__("__exit_compat")));		\
	___ADDRESSABLE(cleanup_module, __exitdata);
#else
#define module_exit(exitfn)						\
	static void __exit __exit_compat(void)				\
	{								\
		exitfn();						\
		rcu_barrier();						\
	}								\
	void cleanup_module(void) __attribute__((cold,alias("__exit_compat")));
#endif
#endif

#if LINUX_VERSION_IS_LESS(3,3,0)
#undef param_check_bool
#define param_check_bool(name, p) __param_check(name, p, bool)
#endif

#ifdef BPM_MODULE_IMPORT_TO_STRING_LITERAL_PRESENT
#undef MODULE_IMPORT_NS
#define MODULE_IMPORT_NS(ns) MODULE_INFO(import_ns, __stringify(ns))
#endif

#endif /* __BACKPORT_LINUX_MODULE_H */
