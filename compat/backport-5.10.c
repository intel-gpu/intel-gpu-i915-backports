/*
 * Copyright (c) 2021
 *
 * Backport functionality introduced in Linux 5.10.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/sysfs.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>

#ifdef BPM_VMA_SET_FILE_NOT_PRESENT
/*
 * Change backing file, only valid to use during initial VMA setup.
 */
void vma_set_file(struct vm_area_struct *vma, struct file *file)
{
	/* Changing an anonymous vma with this is illegal */
	get_file(file);
	swap(vma->vm_file, file);
	fput(file);
}
EXPORT_SYMBOL(vma_set_file);
#endif

#ifdef BPM_SYSFS_EMIT_NOT_PRESENT
/**
 *      sysfs_emit - scnprintf equivalent, aware of PAGE_SIZE buffer.
 *      @buf:   start of PAGE_SIZE buffer.
 *      @fmt:   format
 *      @...:   optional arguments to @format
 *
 *
 * Returns number of characters written to @buf.
 */

int sysfs_emit(char *buf, const char *fmt, ...)
{
        va_list args;
        int len;

        if (WARN(!buf || offset_in_page(buf),
                 "invalid sysfs_emit: buf:%p\n", buf))
                return 0;

        va_start(args, fmt);
        len = vscnprintf(buf, PAGE_SIZE, fmt, args);
        va_end(args);

        return len;
}
EXPORT_SYMBOL_GPL(sysfs_emit);

/**
 *      sysfs_emit_at - scnprintf equivalent, aware of PAGE_SIZE buffer.
 *      @buf:   start of PAGE_SIZE buffer.
 *      @at:    offset in @buf to start write in bytes
 *              @at must be >= 0 && < PAGE_SIZE
 *      @fmt:   format
 *      @...:   optional arguments to @fmt
 *
 *
 * Returns number of characters written starting at &@buf[@at].
 */
int sysfs_emit_at(char *buf, int at, const char *fmt, ...)
{
        va_list args;
        int len;

        if (WARN(!buf || offset_in_page(buf) || at < 0 || at >= PAGE_SIZE,
                 "invalid sysfs_emit_at: buf:%p at:%d\n", buf, at))
                return 0;

        va_start(args, fmt);
        len = vscnprintf(buf + at, PAGE_SIZE - at, fmt, args);
        va_end(args);

        return len;
}
EXPORT_SYMBOL_GPL(sysfs_emit_at);
#endif

#ifdef BPM_PCI_REBAR_SIZE_NOT_PRESENT
/**
 * pci_rebar_find_pos - find position of resize ctrl reg for BAR
 * @pdev: PCI device
 * @bar: BAR to find
 *
 * Helper to find the position of the ctrl register for a BAR.
 * Returns -ENOTSUPP if resizable BARs are not supported at all.
 * Returns -ENOENT if no ctrl register for the BAR could be found.
 */
static int pci_rebar_find_pos(struct pci_dev *pdev, int bar)
{
        unsigned int pos, nbars, i;
        u32 ctrl;

        pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_REBAR);
        if (!pos)
                return -ENOTSUPP;

        pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl);
        nbars = (ctrl & PCI_REBAR_CTRL_NBAR_MASK) >>
                    PCI_REBAR_CTRL_NBAR_SHIFT;

        for (i = 0; i < nbars; i++, pos += 8) {
                int bar_idx;

                pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl);
                bar_idx = ctrl & PCI_REBAR_CTRL_BAR_IDX;
                if (bar_idx == bar)
                        return pos;
        }

        return -ENOENT;
}
/**
 * pci_rebar_get_possible_sizes - get possible sizes for BAR
 * @pdev: PCI device
 * @bar: BAR to query
 *
 * Get the possible sizes of a resizable BAR as bitmask defined in the spec
 * (bit 0=1MB, bit 19=512GB). Returns 0 if BAR isn't resizable.
 */
u32 pci_rebar_get_possible_sizes(struct pci_dev *pdev, int bar)
{
        int pos;
        u32 cap;

        pos = pci_rebar_find_pos(pdev, bar);
        if (pos < 0)
                return 0;

        pci_read_config_dword(pdev, pos + PCI_REBAR_CAP, &cap);
        cap &= PCI_REBAR_CAP_SIZES;

        /* Sapphire RX 5600 XT Pulse has an invalid cap dword for BAR 0 */
        if (pdev->vendor == PCI_VENDOR_ID_ATI && pdev->device == 0x731f &&
            bar == 0 && cap == 0x7000)
                cap = 0x3f000;

        return cap >> 4;
}
EXPORT_SYMBOL(pci_rebar_get_possible_sizes);
#endif

