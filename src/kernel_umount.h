/*
 * Mirage: Kernel-level umount support.
 *
 * This file provides the kernel umount functionality for Mirage,
 * allowing automatic unmounting of paths for app processes without
 * depending on KernelSU.
 */

#ifndef __MIRAGE_UMOUNT_H__
#define __MIRAGE_UMOUNT_H__

#include <linux/types.h>
#include <linux/list.h>
#include <linux/rwsem.h>

/* Umount mode constants */
#define MIRAGE_UMOUNT_WIPE     0
#define MIRAGE_UMOUNT_ADD      1
#define MIRAGE_UMOUNT_DEL      2

/* Umount flags */
#define MIRAGE_UMOUNT_FLAG_NONE        0x0
#define MIRAGE_UMOUNT_FLAG_MNT_ONLY    0x1

/* Mount entry structure for umount list */
struct mount_entry {
	char *umountable;
	unsigned int flags;
	struct list_head list;
};

/* Global umount list and lock */
extern struct list_head mirage_mount_list;
extern struct rw_semaphore mirage_mount_list_lock;

/* Feature control */
extern bool mirage_kernel_umount_enabled;

/* UID range constants */
#define PER_USER_RANGE 100000
#define FIRST_APPLICATION_UID 10000
#define LAST_APPLICATION_UID 19999
#define FIRST_ISOLATED_UID 99000
#define LAST_ISOLATED_UID 99999

static inline bool is_appuid(uid_t uid)
{
	uid_t appid = uid % PER_USER_RANGE;
	return appid >= FIRST_APPLICATION_UID && appid <= LAST_APPLICATION_UID;
}

/*
 * Check if uid is an isolated process UID
 */
static inline bool is_isolated_process(uid_t uid)
{
	uid_t appid = uid % PER_USER_RANGE;
	return appid >= FIRST_ISOLATED_UID && appid <= LAST_ISOLATED_UID;
}

/* Initialization and cleanup */
void mirage_kernel_umount_init(void);
void mirage_kernel_umount_exit(void);

/* Main umount handler - called from setuid/setresuid hooks */
void mirage_handle_umount(uid_t old_uid, uid_t new_uid);
void mirage_handle_setresuid(uid_t ruid, uid_t euid, uid_t suid);

/* Procfs interface functions */
#ifdef MIRAGE_FS_PROC
int mirage_umount_proc_init(void);
void mirage_umount_proc_exit(void);
#else
static inline int mirage_umount_proc_init(void) { return 0; }
static inline void mirage_umount_proc_exit(void) { }
#endif

/* List management functions */
int mirage_umount_add(const char *path, unsigned int flags);
int mirage_umount_del(const char *path);
int mirage_umount_wipe(void);

/* Helper to check if uid should trigger umount */
bool mirage_uid_should_umount(uid_t uid);

#endif /* __MIRAGE_UMOUNT_H__ */
