#ifndef STUB_X86_ASM_CPUFEATURE_H
#define STUB_X86_ASM_CPUFEATURE_H

#include_next <asm/cpufeature.h>

/*
 * This is a lie - there is no such thing on ARM64, but this makes it take the
 * right branches
 */
#define pat_enabled()		1

#define static_cpu_has(x)	0

#endif
