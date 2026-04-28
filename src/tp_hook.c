/*
 * NoMountFS - Tracepoint hook for setresuid syscall interception.
 * This allows NoMountFS to automatically unmount paths for app processes
 * without depending on KernelSU, by hooking into the setresuid syscall
 * and triggering our umount logic when an app UID is set.
 */

#include <linux/module.h>
#include <trace/events/syscalls.h>
#include <linux/tracepoint.h>
#include <asm/unistd.h>
#include "nomount.h"
#include "nomount_umount.h"

/* Architecture-independent syscall argument extraction macros. */
#if defined(CONFIG_ARM64)
    #define SYSCALL_ARG1(regs) ((regs)->regs[0])
    #define SYSCALL_ARG2(regs) ((regs)->regs[1])
    #define SYSCALL_ARG3(regs) ((regs)->regs[2])
#elif defined(CONFIG_ARM)
    #define SYSCALL_ARG1(regs) ((regs)->uregs[0])
    #define SYSCALL_ARG2(regs) ((regs)->uregs[1])
    #define SYSCALL_ARG3(regs) ((regs)->uregs[2])
#elif defined(CONFIG_X86_64)
    #define SYSCALL_ARG1(regs) ((regs)->di)
    #define SYSCALL_ARG2(regs) ((regs)->si)
    #define SYSCALL_ARG3(regs) ((regs)->dx)
#elif defined(CONFIG_X86_32)
    #define SYSCALL_ARG1(regs) ((regs)->bx)
    #define SYSCALL_ARG2(regs) ((regs)->cx)
    #define SYSCALL_ARG3(regs) ((regs)->dx)
#else
    #error "NoMountFS: Unsupported architecture for syscall interception."
#endif

/* 1. Define the function that will be collected from the tracepoint */
static void nomount_sys_enter_probe(void *ignore, struct pt_regs *regs, long id)
{
    /* __NR_setresuid is dynamically resolved based on architecture by unistd.h */
    if (id == __NR_setresuid) {
        uid_t ruid = (uid_t)SYSCALL_ARG1(regs);
        uid_t euid = (uid_t)SYSCALL_ARG2(regs);
        uid_t suid = (uid_t)SYSCALL_ARG3(regs);

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
    
    /*
     * unregister_trace_sys_enter() only unlinks the probe. We MUST wait 
     * for any CPU currently executing the probe to finish and exit.
     * Without this, unloading the module unmaps the memory while a CPU
     * might still be executing it, causing an immediate Kernel Panic.
     */
    tracepoint_synchronize_unregister();
}
