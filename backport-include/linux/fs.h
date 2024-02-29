#ifndef _COMPAT_LINUX_FS_H
#define _COMPAT_LINUX_FS_H
#include_next <linux/fs.h>
#include <linux/version.h>
/*
 * some versions don't have this and thus don't
 * include it from the original fs.h
 */
#include <linux/uidgid.h>

#if LINUX_VERSION_IS_LESS(3,4,0)
#define simple_open LINUX_I915_BACKPORT(simple_open)
extern int simple_open(struct inode *inode, struct file *file);
#endif

#if LINUX_VERSION_IS_LESS(3,9,0)
/**
 * backport of:
 *
 * commit 496ad9aa8ef448058e36ca7a787c61f2e63f0f54
 * Author: Al Viro <viro@zeniv.linux.org.uk>
 * Date:   Wed Jan 23 17:07:38 2013 -0500
 *
 *     new helper: file_inode(file)
 */
static inline struct inode *file_inode(struct file *f)
{
	return f->f_path.dentry->d_inode;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,18,0)
static inline int call_mmap(struct file *file, struct vm_area_struct *vma)
{
	return file->f_op->mmap(file, vma);
}

#ifndef replace_fops
/*
 * This one is to be used *ONLY* from ->open() instances.
 * fops must be non-NULL, pinned down *and* module dependencies
 * should be sufficient to pin the caller down as well.
 */
#define replace_fops(f, fops) \
	do {	\
		struct file *__file = (f); \
		fops_put(__file->f_op); \
		BUG_ON(!(__file->f_op = (fops))); \
	} while(0)
#endif /* replace_fops */
static inline struct file *file_clone_open(struct file *file)
{
	return dentry_open(&file->f_path, file->f_flags, file->f_cred);
}
#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(4,18,0) */

#if (LINUX_VERSION_IS_LESS(4,5,0) && \
     LINUX_VERSION_IS_GEQ(3,2,0))
#define no_seek_end_llseek LINUX_I915_BACKPORT(no_seek_end_llseek)
extern loff_t no_seek_end_llseek(struct file *, loff_t, int);
#endif /* < 4.5 && >= 3.2 */

#ifdef BPM_COMPAT_PTR_IOCTL_NOT_PRESENT
#ifdef CONFIG_COMPAT
#define compat_ptr_ioctl LINUX_I915_BACKPORT(compat_ptr_ioctl)
extern long compat_ptr_ioctl(struct file *file, unsigned int cmd,
					unsigned long arg);
#else
#define compat_ptr_ioctl NULL
#endif
#endif /* < 5.5 */

#ifdef BPM_STRUCT_PROC_OPS_NOT_PRESENT
#define proc_ops file_operations
#define proc_open open
#define proc_read read
#define proc_lseek llseek
#define proc_release release
#define proc_write write
#endif /* BPM_STRUCT_PROC_OPS_NOT_PRESENT */

#ifdef BPM_PAGECACHE_WRITE_BEGIN_AND_END_NOT_PRESENT
int pagecache_write_begin(struct file *, struct address_space *mapping,
                                loff_t pos, unsigned len, unsigned flags,
                                struct page **pagep, void **fsdata);

int pagecache_write_end(struct file *, struct address_space *mapping,
                                loff_t pos, unsigned len, unsigned copied,
                                struct page *page, void *fsdata);
#endif

#endif	/* _COMPAT_LINUX_FS_H */
