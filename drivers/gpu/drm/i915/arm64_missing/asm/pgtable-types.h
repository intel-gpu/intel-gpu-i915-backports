#ifndef STUB_X86_ASM_PGTABLE_H
#define STUB_X86_ASM_PGTABLE_H 1

/* ALL THIS IS **PLAIN WRONG** - just allowing to compile for now */

#include_next <asm/pgtable-types.h>

#define PAGE_KERNEL_IO		__pgprot_mask(__PAGE_KERNEL_IO)
#define __PAGE_KERNEL_IO		__PAGE_KERNEL
#define __PAGE_KERNEL	1
#define __pgprot_mask(x) 2

#define _PAGE_BIT_PWT           3       /* page write through */
#define _PAGE_BIT_PCD           4       /* page cache disabled */
#define _PAGE_BIT_PAT           7       /* on 4KB pages */

#define _PAGE_PWT       (_AT(pteval_t, 1) << _PAGE_BIT_PWT)
#define _PAGE_PCD       (_AT(pteval_t, 1) << _PAGE_BIT_PCD)
#define _PAGE_PAT       (_AT(pteval_t, 1) << _PAGE_BIT_PAT)

#undef pgprot_val
#define pgprot_val(x) 1

#endif
