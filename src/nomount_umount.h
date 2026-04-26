/*
 * NoMountFS: Kernel-level umount support.
 *
 * This file provides the kernel umount functionality for NoMountFS,
 * allowing automatic unmounting of paths for app processes without
 * depending on KernelSU.
 */

#ifndef __NOMOUNT_UMOUNT_H__
#define __NOMOUNT_UMOUNT_H__

#include <linux/types.h>
#include <linux/list.h>
#include <linux/rwsem.h>

/* Umount mode constants */
#define NOMOUNT_UMOUNT_WIPE     0
#define NOMOUNT_UMOUNT_ADD      1
#define NOMOUNT_UMOUNT_DEL      2

/* Umount flags */
#define NOMOUNT_UMOUNT_FLAG_NONE        0x0
#define NOMOUNT_UMOUNT_FLAG_MNT_ONLY    0x1

/* Mount entry structure for umount list */
struct mount_entry {
	char *umountable;
	unsigned int flags;
	struct list_head list;
};

/* Global umount list and lock */
extern struct list_head nomount_mount_list;
extern struct rw_semaphore nomount_mount_list_lock;

/* Feature control */
extern bool nomount_kernel_umount_enabled;

/* Initialization and cleanup */
void nomount_kernel_umount_init(void);
void nomount_kernel_umount_exit(void);

/* Main umount handler - called from setuid/setresuid hooks */
int nmfs_handle_setresuid(uid_t ruid, uid_t euid, uid_t suid);

/* Procfs interface functions */
#ifdef NOMOUNT_FS_PROC
int nomount_umount_proc_init(void);
void nomount_umount_proc_exit(void);
#else
static inline int nomount_umount_proc_init(void) { return 0; }
static inline void nomount_umount_proc_exit(void) { }
#endif

/* List management functions */
int nomount_umount_add(const char *path, unsigned int flags);
int nomount_umount_del(const char *path);
int nomount_umount_wipe(void);
int nomount_umount_list(char *buf, size_t buf_size);

/* Helper to check if uid should trigger umount */
bool nomount_uid_should_umount(uid_t uid);

/* Check if path is already in umount list */
bool nomount_umount_path_exists(const char *path);

#endif /* __NOMOUNT_UMOUNT_H__ */
