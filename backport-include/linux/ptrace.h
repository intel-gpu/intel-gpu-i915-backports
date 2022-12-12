/*
 *
 * Copyright © 2021 Intel Corporation
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef __BACKPORT_PTRACE_H
#define __BACKPORT_PTRACE_H

#include <linux/version.h>
#ifdef BPM_PTRACE_MAY_ACCESS_NOT_PRESENT
#include_next <linux/ptrace.h>

#define ptrace_may_access LINUX_I915_BACKPORT(ptrace_may_access)

/**
 * ptrace_may_access - check whether the caller is permitted to access
 * a target task.
 * @task: target task
 * @mode: selects type of access and caller credentials
 *
 * Returns true on success, false on denial.
 *
 * One of the flags PTRACE_MODE_FSCREDS and PTRACE_MODE_REALCREDS must
 * be set in @mode to specify whether the access was requested through
 * a filesystem syscall (should use effective capabilities and fsuid
 * of the caller) or through an explicit syscall such as
 * process_vm_writev or ptrace (and should use the real credentials).
 */
extern bool ptrace_may_access(struct task_struct *task, unsigned int mode);

#endif
#endif /* __BACKPORT_PTRACE_H */
