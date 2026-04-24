/*
 * NoMountFS - Tracepoint hook for setresuid syscall interception.
 * This allows NoMountFS to automatically unmount paths for app processes
 * without depending on KernelSU, by hooking into the setresuid syscall
 * and triggering our umount logic when an app UID is set.
 */

#include <linux/module.h>
#include <trace/events/syscalls.h>
#include <asm/unistd.h>
#include "nomount.h"
#include "nomount_umount.h"

/* 1. Define the function that will be collected from the tracepoint */
static void nomount_sys_enter_probe(void *ignore, struct pt_regs *regs, long id)
{
    /* __NR_setresuid is usually 117 on ARM64 */
    if (id == __NR_setresuid) {
        uid_t ruid = (uid_t)regs->regs[0];
        uid_t euid = (uid_t)regs->regs[1];
        uid_t suid = (uid_t)regs->regs[2];

        /* Call our secret magic */
        nmfs_handle_setresuid(ruid, euid, suid);
    }
}

/* 2. Register the hook at initialization (main.c) */
int nomount_init_hooks(void)
{
    int ret;
    ret = register_trace_sys_enter(nomount_sys_enter_probe, NULL);
    if (ret) {
        pr_err("nomount: Failed to register sys_enter tracepoint\n");
        return ret;
    }
    pr_info("nomount: Hooked via Tracepoints!\n");
    return 0;
}

/* 3. Unregister upon exit */
void nomount_exit_hooks(void)
{
    unregister_trace_sys_enter(nomount_sys_enter_probe, NULL);
}
