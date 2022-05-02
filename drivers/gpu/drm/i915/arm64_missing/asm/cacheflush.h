#ifndef STUB_X86_ASM_CACHEFLUSH_H
#define STUB_X86_ASM_CACHEFLUSH_H 1

#include_next <asm/cacheflush.h>

#define clflush(x) do { (void)x; } while (0)
#define clflushopt(x) do { (void)x; } while (0)

/*
 * stub already added by arch:
 *
 * 	define clflush_cache_range(x, y) do {  } while (0)
 */

#endif
