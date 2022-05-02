/*
 * Copyright (c) 2020
 *
 * Backport functionality introduced in Linux 5.3.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/export.h>
#include <linux/ktime.h>
#include <linux/jiffies.h>
#include <linux/moduleparam.h>
#include <linux/module.h>

int wake_up_state(struct task_struct *p, unsigned int state)
{
	        return wake_up_process(p);
}

char *dynamic_dname(struct dentry *dentry, char *buffer, int buflen,
		const char *fmt, ...)
{
	va_list args;
	char temp[64];
	int sz;

	va_start(args, fmt);
	sz = vsnprintf(temp, sizeof(temp), fmt, args) + 1;
	va_end(args);

	if (sz > sizeof(temp) || sz > buflen)
		return ERR_PTR(-ENAMETOOLONG);

	buffer += buflen - sz;
	return memcpy(buffer, temp, sz);
}
