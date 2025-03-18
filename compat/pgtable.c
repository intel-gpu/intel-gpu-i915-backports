// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/hugetlb.h>
#include <asm/pgalloc.h>
#include <asm/fixmap.h>
#include <asm/mtrr.h>

#ifdef BPM_PMD_PTE_MKWRITE_VMA_ARG_NOT_PRESENT

pte_t pte_mkwrite(pte_t pte, struct vm_area_struct *vma)
{
	if (vma->vm_flags & VM_SHADOW_STACK)
		return pte_mkwrite_shstk(pte);

	pte = pte_mkwrite_novma(pte);

	return pte_clear_saveddirty(pte);
}
EXPORT_SYMBOL(pte_mkwrite);

pmd_t pmd_mkwrite(pmd_t pmd, struct vm_area_struct *vma)
{
	if (vma->vm_flags & VM_SHADOW_STACK)
		return pmd_mkwrite_shstk(pmd);

	pmd = pmd_mkwrite_novma(pmd);

	return pmd_clear_saveddirty(pmd);
}
EXPORT_SYMBOL(pmd_mkwrite);
#endif
