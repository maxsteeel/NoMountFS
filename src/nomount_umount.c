/*
 * NoMountFS: Kernel-level umount support.
 *
 * This file implements the kernel umount functionality for NoMountFS,
 * allowing automatic unmounting of paths for app processes without
 * depending on KernelSU.
 */

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/nsproxy.h>
#include <linux/path.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/uidgid.h>
#include <linux/version.h>
#include <linux/user_namespace.h>
#include <linux/pid.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/proc_ns.h>
#include <linux/sched/task.h>

#include "nomount.h"
#include "nomount_umount.h"

struct cred *nmfs_cred = NULL;
bool nomount_kernel_umount_enabled = true;

/* Global umount list and lock */
struct list_head nomount_mount_list = LIST_HEAD_INIT(nomount_mount_list);
DECLARE_RWSEM(nomount_mount_list_lock);

/* UID range constants */
#define PER_USER_RANGE 100000
#define FIRST_APPLICATION_UID 10000
#define LAST_APPLICATION_UID 19999
#define FIRST_ISOLATED_UID 99000
#define LAST_ISOLATED_UID 99999

struct nmfs_umount_work {
	struct callback_head work;
};

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

/*
 * Check if a uid should trigger umount
 * For now, all app UIDs should umount
 */
bool nomount_uid_should_umount(uid_t uid)
{
	/* Always umount for app UIDs and isolated processes */
	if (is_appuid(uid) || is_isolated_process(uid)) {
		return true;
	}
	return false;
}

/*
 * Check if a path already exists in the umount list
 * Assumes nomount_mount_list_lock is already held (read or write)
 */
static bool __nomount_umount_path_exists_locked(const char *path)
{
	struct mount_entry *entry;

	list_for_each_entry(entry, &nomount_mount_list, list) {
		if (strcmp(entry->umountable, path) == 0) {
			return true;
		}
	}

	return false;
}

/*
 * Check if a path already exists in the umount list
 */
bool nomount_umount_path_exists(const char *path)
{
	bool found;

	down_read(&nomount_mount_list_lock);
	found = __nomount_umount_path_exists_locked(path);
	up_read(&nomount_mount_list_lock);

	return found;
}

/*
 * Replaced manual SELinux domain transitions with prepare_kernel_cred().
 * This dynamically allocates a credential struct with full root privileges 
 * (including CAP_SYS_ADMIN), satisfying VFS permission checks natively.
 */
void setup_nmfs_cred(void)
{
	if (!nmfs_cred) {
		/* prepare_kernel_cred(NULL) is a fully exported symbol */
		nmfs_cred = prepare_kernel_cred(NULL);
		if (!nmfs_cred) {
			pr_err("nomount: failed to allocate kernel credentials\n");
		}
	}
}

static void try_umount(const char *mnt, int flags)
{
	struct path path;
	int err;

	/* FORCE MNT_DETACH (2) regardless of what userspace requested */
	flags |= MNT_DETACH;

	/* The native path structure of the kernel is obtained */
	err = kern_path(mnt, 0, &path);
	if (err) {
		pr_err("nomount: kern_path failed for %s (err: %d)\n", mnt, err);
		return;
	}

	/* Ensure that we are unmounting the actual root of the mount point */
	if (path.dentry != path.mnt->mnt_root) {
		pr_err("nomount: %s is not a mount root, skipping.\n", mnt);
		path_put(&path);
		return;
	}

	/* Execute the native unmount */
	err = path_umount(&path, flags);
	
	if (err && err != -ENOENT && err != -EINVAL) {
		pr_err("nomount: umount %s failed: %d\n", mnt, err);
	} else if (err == 0) {
		pr_debug("nomount: SUCCESS! %s unmounted from app namespace.\n", mnt);
	}
}

static void nmfs_do_umount_work(struct callback_head *work)
{
	struct nmfs_umount_work *nw = container_of(work, struct nmfs_umount_work, work);
	const struct cred *saved;
	struct mount_entry *entry;

	setup_nmfs_cred();
	if (nmfs_cred) {
		saved = override_creds(nmfs_cred);

		down_read(&nomount_mount_list_lock);
		list_for_each_entry(entry, &nomount_mount_list, list) {
			try_umount(entry->umountable, entry->flags);
		}
		up_read(&nomount_mount_list_lock);

		revert_creds(saved);
	}

	kfree(nw);
}

int nmfs_handle_umount(uid_t old_uid, uid_t new_uid)
{
	struct nmfs_umount_work *nw;

	if (!nomount_kernel_umount_enabled) {
		pr_info("nomount: Kernel umount disabled, skipping umount handling.\n");
		return 0;
	}

	if (!is_appuid(new_uid) && !is_isolated_process(new_uid)) {
		pr_info("nomount: New UID %u is not an app or isolated UID, skipping umount.\n", new_uid);
		return 0;
	}

	if (!nomount_uid_should_umount(new_uid) && !is_isolated_process(new_uid)) {
		pr_info("nomount: New UID %u does not require umount, skipping.\n", new_uid);
		return 0;
	}

	if (old_uid != 0) {
		pr_info("nomount: Old UID %u is not root, skipping umount handling.\n", old_uid);
		return 0;
	}

	/* Fast path: Check if the list is empty without locking.
	 * list_empty is safe to call locklessly as it just compares pointers.
	 * Since umount entries are rarely added/removed during normal operation,
	 * this saves expensive override_creds and down_read calls.
	 */
	if (list_empty(&nomount_mount_list)) {
		return 0;
	}

	nw = kzalloc(sizeof(*nw), GFP_ATOMIC);
	if (!nw) return 0;

	init_task_work(&nw->work, nmfs_do_umount_work);

	if (task_work_add(current, &nw->work, TWA_RESUME)) {
		pr_err("nomount: task_work_add failed!\n");
		kfree(nw);
	} else {
		pr_debug("nomount: App Launch Detected! Deferring umount work...\n");
	}

	return 0;
}

int nmfs_handle_setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
	uid_t new_uid = ruid;
	uid_t old_uid = current_uid().val;

	if (old_uid != 0)
		return 0;

	nmfs_handle_umount(old_uid, new_uid);

	return 0;
}

/*
 * Add a path to the umount list
 */
int nomount_umount_add(const char *path, unsigned int flags)
{
	struct mount_entry *new_entry;
	char *path_copy;

	if (!path) {
		pr_err("nomount: umount_add called with NULL path\n");
		return -EINVAL;
	}

	new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
	if (!new_entry) {
		pr_err("nomount: failed to allocate memory for mount entry\n");
		return -ENOMEM;
	}

	path_copy = kstrdup(path, GFP_KERNEL);
	if (!path_copy) {
		pr_err("nomount: failed to duplicate path string\n");
		kfree(new_entry);
		return -ENOMEM;
	}

	new_entry->umountable = path_copy;
	new_entry->flags = flags;

	down_write(&nomount_mount_list_lock);

	/* Double-check existence while holding the lock to avoid race conditions (TOCTOU) */
	if (__nomount_umount_path_exists_locked(path)) {
		pr_info("nomount: Path already in umount list, skipping add: %s\n", path);
		up_write(&nomount_mount_list_lock);
		kfree(path_copy);
		kfree(new_entry);
		return -EEXIST;
	}

	list_add_tail(&new_entry->list, &nomount_mount_list);
	up_write(&nomount_mount_list_lock);

	pr_info("nomount: ADDED to umount list: %s (flags=0x%x)\n", path, flags);

	return 0;
}

/*
 * Remove a path from the umount list
 */
int nomount_umount_del(const char *path)
{
	struct mount_entry *entry, *tmp;
	int found = 0;

	if (!path) {
		pr_err("nomount: umount_del called with NULL path\n");
		return -EINVAL;
	}

	down_write(&nomount_mount_list_lock);
	list_for_each_entry_safe(entry, tmp, &nomount_mount_list, list) {
		if (strcmp(entry->umountable, path) == 0) {
			list_del(&entry->list);
			kfree(entry->umountable);
			kfree(entry);
			found++;
			pr_info("nomount: REMOVED from umount list: %s\n", path);
		}
	}
	up_write(&nomount_mount_list_lock);

	return found ? 0 : -ENOENT;
}

/*
 * Wipe/clear all entries from the umount list
 */
int nomount_umount_wipe(void)
{
	struct mount_entry *entry, *tmp;
	int count = 0;

	down_write(&nomount_mount_list_lock);
	list_for_each_entry_safe(entry, tmp, &nomount_mount_list, list) {
		list_del(&entry->list);
		kfree(entry->umountable);
		kfree(entry);
		count++;
		pr_info("nomount: WIPED from umount list: %s\n", entry->umountable);
	}
	up_write(&nomount_mount_list_lock);

	return count ? 0 : -ENOENT;
}

/*
 * List all entries in the umount list
 * Returns the number of bytes written to buf
 */
int nomount_umount_list(char *buf, size_t buf_size)
{
	struct mount_entry *entry;
	size_t offset = 0;
	int count = 0;

	if (!buf || buf_size == 0)
		return -EINVAL;

	/* Write header */
	offset += snprintf(buf + offset, buf_size - offset,
			   "Mount Point\tFlags\n");
	offset += snprintf(buf + offset, buf_size - offset,
			   "----------\t-----\n");

	down_read(&nomount_mount_list_lock);
	list_for_each_entry(entry, &nomount_mount_list, list) {
		int written;

		written = snprintf(buf + offset, buf_size - offset,
				  "%s\t%u\n", entry->umountable, entry->flags);
		if (written < 0 || written >= (int)(buf_size - offset)) {
			break;
		}
		offset += written;
		count++;
	}
	up_read(&nomount_mount_list_lock);

	return offset;
}

/*
 * Procfs interface for managing umount list from userspace
 */
#ifdef NOMOUNT_FS_PROC

#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#define NOMOUNT_PROC_UMOUNT "nomountfs"

static struct proc_dir_entry *nomount_proc_dir;

/*
 * Show umount list in proc
 */
static int nomount_umount_list_show(struct seq_file *m, void *v)
{
	struct mount_entry *entry;

	down_read(&nomount_mount_list_lock);
	list_for_each_entry(entry, &nomount_mount_list, list) {
		seq_printf(m, "%s\t%u\n", entry->umountable, entry->flags);
	}
	up_read(&nomount_mount_list_lock);

	return 0;
}

static int nomount_umount_list_open(struct inode *inode, struct file *file)
{
	return single_open(file, nomount_umount_list_show, NULL);
}

/* proc_ops compatibility for kernels < 5.6 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops nomount_umount_list_proc_ops = {
	.proc_open	= nomount_umount_list_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};
#else
static const struct file_operations nomount_umount_list_proc_ops = {
	.open		= nomount_umount_list_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
#endif

/*
 * Write handler for umount_add
 */
static ssize_t nomount_umount_add_write(struct file *file,
					const char __user *buf,
					size_t count, loff_t *ppos)
{
	char path[256];
	ssize_t len;
	int ret;

	if (count >= sizeof(path))
		return -ENAMETOOLONG;

	len = simple_write_to_buffer(path, sizeof(path) - 1, ppos, buf, count);
	if (len <= 0)
		return len;

	path[len] = '\0';

	/* Remove trailing newline */
	while (len > 0 && (path[len-1] == '\n' || path[len-1] == '\r')) {
		path[--len] = '\0';
	}

	if (len == 0)
		return -EINVAL;

	ret = nomount_umount_add(path, NOMOUNT_UMOUNT_FLAG_NONE);
	if (ret < 0) {
		return ret;
	}

	return count;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops nomount_umount_add_proc_ops = {
	.proc_write	= nomount_umount_add_write,
};
#else
static const struct file_operations nomount_umount_add_proc_ops = {
	.write		= nomount_umount_add_write,
};
#endif

/*
 * Write handler for umount_del
 */
static ssize_t nomount_umount_del_write(struct file *file,
					const char __user *buf,
					size_t count, loff_t *ppos)
{
	char path[256];
	ssize_t len;

	if (count >= sizeof(path))
		return -ENAMETOOLONG;

	len = simple_write_to_buffer(path, sizeof(path) - 1, ppos, buf, count);
	if (len <= 0)
		return len;

	path[len] = '\0';

	/* Remove trailing newline */
	while (len > 0 && (path[len-1] == '\n' || path[len-1] == '\r')) {
		path[--len] = '\0';
	}

	if (len == 0)
		return -EINVAL;

	if (nomount_umount_del(path) < 0) {
		return -ENOENT;
	}

	return count;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops nomount_umount_del_proc_ops = {
	.proc_write	= nomount_umount_del_write,
};
#else
static const struct file_operations nomount_umount_del_proc_ops = {
	.write		= nomount_umount_del_write,
};
#endif

/*
 * Write handler for umount_clear
 */
static ssize_t nomount_umount_clear_write(struct file *file,
					  const char __user *buf,
					  size_t count, loff_t *ppos)
{
	char val[16];
	ssize_t len;

	len = simple_write_to_buffer(val, sizeof(val) - 1, ppos, buf, count);
	if (len <= 0)
		return len;

	val[len] = '\0';

	if (val[0] == '1') {
		nomount_umount_wipe();
	}

	return count;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops nomount_umount_clear_proc_ops = {
	.proc_write	= nomount_umount_clear_write,
};
#else
static const struct file_operations nomount_umount_clear_proc_ops = {
	.write		= nomount_umount_clear_write,
};
#endif

/*
 * Read/Write handler for umount_enabled
 */
static int nomount_umount_enabled_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", nomount_kernel_umount_enabled ? 1 : 0);
	return 0;
}

static int nomount_umount_enabled_open(struct inode *inode, struct file *file)
{
	return single_open(file, nomount_umount_enabled_show, NULL);
}

static ssize_t nomount_umount_enabled_write(struct file *file,
					    const char __user *buf,
					    size_t count, loff_t *ppos)
{
	char val[16];
	ssize_t len;

	len = simple_write_to_buffer(val, sizeof(val) - 1, ppos, buf, count);
	if (len <= 0)
		return len;

	val[len] = '\0';

	if (val[0] == '1') {
		nomount_kernel_umount_enabled = true;
	} else {
		nomount_kernel_umount_enabled = false;
	}

	return count;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops nomount_umount_enabled_proc_ops = {
	.proc_open	= nomount_umount_enabled_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= nomount_umount_enabled_write,
};
#else
static const struct file_operations nomount_umount_enabled_proc_ops = {
	.open		= nomount_umount_enabled_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= nomount_umount_enabled_write,
};
#endif

/*
 * Initialize procfs interface
 */
int nomount_umount_proc_init(void)
{
	struct proc_dir_entry *entry;

	nomount_proc_dir = proc_mkdir(NOMOUNT_PROC_UMOUNT, NULL);
	if (!nomount_proc_dir) {
		return -ENOMEM;
	}

	entry = proc_create("umount_list", 0444, nomount_proc_dir,
			    &nomount_umount_list_proc_ops);
	if (!entry) {
		goto out_remove_dir;
	}

	entry = proc_create("umount_add", 0200, nomount_proc_dir,
			    &nomount_umount_add_proc_ops);
	if (!entry) {
		goto out_remove_umount_list;
	}

	entry = proc_create("umount_del", 0200, nomount_proc_dir,
			    &nomount_umount_del_proc_ops);
	if (!entry) {
		goto out_remove_umount_add;
	}

	entry = proc_create("umount_clear", 0200, nomount_proc_dir,
			    &nomount_umount_clear_proc_ops);
	if (!entry) {
		goto out_remove_umount_del;
	}

	entry = proc_create("umount_enabled", 0644, nomount_proc_dir,
			    &nomount_umount_enabled_proc_ops);
	if (!entry) {
		goto out_remove_umount_clear;
	}

	return 0;

out_remove_umount_clear:
	remove_proc_entry("umount_clear", nomount_proc_dir);
out_remove_umount_del:
	remove_proc_entry("umount_del", nomount_proc_dir);
out_remove_umount_add:
	remove_proc_entry("umount_add", nomount_proc_dir);
out_remove_umount_list:
	remove_proc_entry("umount_list", nomount_proc_dir);
out_remove_dir:
	remove_proc_entry(NOMOUNT_PROC_UMOUNT, NULL);
	return -ENOMEM;
}

/*
 * Cleanup procfs interface
 */
void nomount_umount_proc_exit(void)
{
	remove_proc_entry("umount_enabled", nomount_proc_dir);
	remove_proc_entry("umount_clear", nomount_proc_dir);
	remove_proc_entry("umount_del", nomount_proc_dir);
	remove_proc_entry("umount_add", nomount_proc_dir);
	remove_proc_entry("umount_list", nomount_proc_dir);
	remove_proc_entry(NOMOUNT_PROC_UMOUNT, NULL);
}

#endif /* NOMOUNT_FS_PROC */

/*
 * Initialize the kernel umount subsystem
 */
void nomount_kernel_umount_init(void)
{
	INIT_LIST_HEAD(&nomount_mount_list);

#ifdef NOMOUNT_FS_PROC
	nomount_umount_proc_init();
#endif
}

/*
 * Cleanup the kernel umount subsystem
 */
void nomount_kernel_umount_exit(void)
{
#ifdef NOMOUNT_FS_PROC
	nomount_umount_proc_exit();
#endif

	nomount_umount_wipe();
}
