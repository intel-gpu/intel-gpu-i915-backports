/*
 * Copyright (C) 2021 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/file.h>

#ifdef BPM_PCIE_AER_IS_NATIVE_API_NOT_PRESENT
#ifdef CONFIG_PCIEPORTBUS

bool pcie_ports_native;

int check_pcie_port_param(void)
{
	int err = 0, len = 0;
	struct path root;
	struct file *file;
	char *file_path = "/proc/cmdline";
	char *result = NULL;
	char buf[10] = {0};
	void *file_buf = NULL;
	loff_t pos;

	task_lock(&init_task);
	get_fs_root(init_task.fs, &root);
	task_unlock(&init_task);

	file = file_open_root(&root, file_path, O_RDONLY, 0);
        if (IS_ERR(file) != 0)
	{
                printk(KERN_ERR "Failed to open %s\n", file_path);
		err = IS_ERR(file);
		return err;
        }

	file_buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
        if (file_buf == NULL)
	{
		printk(KERN_ERR "Failed to allocate buffer memory\n");
		err = -ENOMEM;
                goto out_fput;
        }

	len = kernel_read(file, file_buf, PAGE_SIZE, &pos);
	if (len < 0)
	{
		printk(KERN_ERR "Failed to read from %s\n", file_path);
		err = -EINVAL;
                goto out_kfree;
        }

	result = strstr((char *)file_buf, "pcie_ports=");
	if (result != NULL)
	{
		len = strlen("native");
		strncpy(buf, result + strlen("pcie_ports="), len);
		buf[len] = '\0';

		if (!strncmp(buf, "native", 6))
		{
			pcie_ports_native = true;
			printk(KERN_INFO"pcie_ports_native is set\n");
		}
	}

out_kfree:
	kfree(file_buf);
out_fput:
	fput(file);

	return err;
}

#endif
#endif
