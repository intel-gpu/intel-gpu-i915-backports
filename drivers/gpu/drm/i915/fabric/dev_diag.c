// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2022 Intel Corporation.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/stringify.h>

#include "dev_diag.h"
#include "fw.h"
#include "ops.h"
#include "port.h"

/*
 * LinkMgr Trace data
 *
 */
#define LM_MAGIC 0x4d4c4453
#define LM_VERSION 1
#define DUMP_VERSION GENMASK(31, 0)
#define DUMP_MAGIC GENMASK(63, 32)

struct linkmgr_trace_hdr {
	__be64 magic;
	ktime_t timestamp;
	u8 fw_version_string[24];
};

#define LINKMGR_TRACE_HDR_SIZE sizeof(struct linkmgr_trace_hdr)
#define LINKMGR_TRACE_MAX_BUF_SIZE (40 * 1024)
#define LINKMGR_TRACE_FILE_NAME "linkmgr_trace"

static int linkmgr_trace_open(struct inode *inode, struct file *file)
{
	struct linkmgr_trace_info {
		struct debugfs_blob_wrapper blob;
		struct mbdb_op_linkmgr_trace_dump_rsp rsp;
		char buf[LINKMGR_TRACE_MAX_BUF_SIZE + LINKMGR_TRACE_HDR_SIZE];
	} *info;
	struct fsubdev *sd = inode->i_private;

	struct linkmgr_trace_hdr *hdr;
	size_t buf_offset = LINKMGR_TRACE_HDR_SIZE;
	bool first = true;
	int err;

	if (unlikely(READ_ONCE(sd->fdev->dev_disabled)))
		return -EIO;

	if (!(sd->fw_version.environment & FW_VERSION_ENV_BIT))
		return -EIO;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	hdr = (struct linkmgr_trace_hdr *)info->buf;
	hdr->magic = cpu_to_be64(FIELD_PREP(DUMP_VERSION, LM_VERSION) |
				 FIELD_PREP(DUMP_MAGIC, LM_MAGIC));
	strncpy(hdr->fw_version_string, sd->fw_version.fw_version_string,
		sizeof(hdr->fw_version_string));

	hdr->timestamp = ktime_get_real();

	do {
		size_t len;

		err = ops_linkmgr_trace_dump(sd, MAX_TRACE_ENTRIES, first, &info->rsp);
		if (err) {
			kfree(info);
			return err;
		}

		/*
		 * minimum of max size of entries, the count or whatever is
		 * left of the buffer.
		 */
		len = min3(sizeof(info->rsp.entries),
			   info->rsp.cnt * sizeof(u64),
			   sizeof(info->buf) - buf_offset);
		if (len) {
			memcpy(&info->buf[buf_offset], &info->rsp.entries, len);
			buf_offset += len;
		}
		first = false;
	} while (info->rsp.more);

	info->blob.data = info->buf;
	info->blob.size = buf_offset;
	file->private_data = info;

	return 0;
}

static const struct file_operations linkmgr_trace_fops = {
	.owner = THIS_MODULE,
	.open = linkmgr_trace_open,
	.read = blob_read,
	.release = blob_release,
	.llseek = default_llseek,
};

#define LINKMGR_TRACE_MASK_FILE_NAME "linkmgr_trace_mask"

static ssize_t linkmgr_trace_mask_read(struct file *fp, char __user *buf, size_t count,
				       loff_t *fpos)
{
	struct fsubdev *sd;
	char read_buf[20];
	u64 mask = 0;
	size_t siz;
	int err;

	sd = fp->private_data;
	if (!sd)
		return -EBADF;

	if (unlikely(READ_ONCE(sd->fdev->dev_disabled)))
		return -EIO;

	err = ops_linkmgr_trace_mask_get(sd, &mask);

	if (err)
		return err;

	siz = scnprintf(read_buf, sizeof(read_buf), "%-18llx\n", mask);

	return simple_read_from_buffer(buf, count, fpos, read_buf, siz);
}

static ssize_t linkmgr_trace_mask_write(struct file *fp, const char __user *buf,
					size_t count, loff_t *fpos)
{
	struct fsubdev *sd;
	u64 mask = 0;
	int err;

	sd = fp->private_data;
	if (!sd)
		return -EBADF;

	if (unlikely(READ_ONCE(sd->fdev->dev_disabled)))
		return -EIO;

	err = kstrtoull_from_user(buf, count, 16, &mask);
	if (err)
		return err;

	err = ops_linkmgr_trace_mask_set(sd, mask);

	if (err)
		return err;

	*fpos += count;
	return count;
}

static const struct file_operations linkmgr_trace_mask_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.llseek = no_llseek,
	.read = linkmgr_trace_mask_read,
	.write = linkmgr_trace_mask_write
};

void create_dev_debugfs_files(struct fsubdev *sd)
{
	if (test_bit(MBOX_OP_CODE_LINK_MGR_TRACE_DUMP, sd->fw_version.supported_opcodes))
		debugfs_create_file(LINKMGR_TRACE_FILE_NAME, 0400, sd->debugfs_dir, sd,
				    &linkmgr_trace_fops);
	if (test_bit(MBOX_OP_CODE_LINK_MGR_TRACE_MASK_GET, sd->fw_version.supported_opcodes) &&
	    test_bit(MBOX_OP_CODE_LINK_MGR_TRACE_MASK_SET, sd->fw_version.supported_opcodes))
		debugfs_create_file(LINKMGR_TRACE_MASK_FILE_NAME, 0600, sd->debugfs_dir, sd,
				    &linkmgr_trace_mask_fops);
}
