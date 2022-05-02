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
