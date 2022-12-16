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

#ifdef BPM_PTRACE_MAY_ACCESS_NOT_PRESENT
#include <linux/ptrace.h>


static bool ptrace_has_cap(struct user_namespace *ns, unsigned int mode)
{
        if (mode & PTRACE_MODE_NOAUDIT)
                return ns_capable_noaudit(ns, CAP_SYS_PTRACE);
        return ns_capable(ns, CAP_SYS_PTRACE);
}

/* Returns 0 on success, -errno on denial. */
static int __ptrace_may_access(struct task_struct *task, unsigned int mode)
{
        const struct cred *cred = current_cred(), *tcred;
        struct mm_struct *mm;
        kuid_t caller_uid;
        kgid_t caller_gid;

        if (!(mode & PTRACE_MODE_FSCREDS) == !(mode & PTRACE_MODE_REALCREDS)) {
                WARN(1, "denying ptrace access check without PTRACE_MODE_*CREDS\n");
                return -EPERM;
        }

        /* May we inspect the given task?
         * This check is used both for attaching with ptrace
         * and for allowing access to sensitive information in /proc.
         *
         * ptrace_attach denies several cases that /proc allows
         * because setting up the necessary parent/child relationship
         * or halting the specified task is impossible.
         */

        /* Don't let security modules deny introspection */
        if (same_thread_group(task, current))
                return 0;
        rcu_read_lock();
        if (mode & PTRACE_MODE_FSCREDS) {
                caller_uid = cred->fsuid;
                caller_gid = cred->fsgid;
        } else {
                /*
                 * Using the euid would make more sense here, but something
                 * in userland might rely on the old behavior, and this
                 * shouldn't be a security problem since
                 * PTRACE_MODE_REALCREDS implies that the caller explicitly
                 * used a syscall that requests access to another process
                 * (and not a filesystem syscall to procfs).
                 */
                caller_uid = cred->uid;
                caller_gid = cred->gid;
        }
        tcred = __task_cred(task);
        if (uid_eq(caller_uid, tcred->euid) &&
            uid_eq(caller_uid, tcred->suid) &&
            uid_eq(caller_uid, tcred->uid)  &&
            gid_eq(caller_gid, tcred->egid) &&
            gid_eq(caller_gid, tcred->sgid) &&
            gid_eq(caller_gid, tcred->gid))
                goto ok;
        if (ptrace_has_cap(tcred->user_ns, mode))
                goto ok;
        rcu_read_unlock();
        return -EPERM;
ok:
        rcu_read_unlock();
        /*
         * If a task drops privileges and becomes nondumpable (through a syscall
         * like setresuid()) while we are trying to access it, we must ensure
         * that the dumpability is read after the credentials; otherwise,
         * we may be able to attach to a task that we shouldn't be able to
         * attach to (as if the task had dropped privileges without becoming
         * nondumpable).
         * Pairs with a write barrier in commit_creds().
         */
        smp_rmb();
        mm = task->mm;
        if (mm &&
            ((get_dumpable(mm) != SUID_DUMP_USER) &&
             !ptrace_has_cap(mm->user_ns, mode)))
            return -EPERM;
	/* 
	 * Backported ptrace_may_access without calling security_ptrace_access_check().
	 * Replaced security_ptrace_access_check fuction call with return 0 value. 
	 */
	return 0;
}

bool ptrace_may_access(struct task_struct *task, unsigned int mode)
{
        int err;
        task_lock(task);
        err = __ptrace_may_access(task, mode);
        task_unlock(task);
        return !err;
}
EXPORT_SYMBOL_GPL(ptrace_may_access);
#endif /* BPM_PTRACE_MAY_ACCESS_NOT_PRESENT */
