/*
 * Mirage: Kernel-level umount support.
 *
 * This file implements the kernel umount functionality for Mirage,
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

#include "mirage.h"
#include "kernel_umount.h"

bool mirage_kernel_umount_enabled = true;

/* Global umount list and lock */
struct list_head mirage_mount_list = LIST_HEAD_INIT(mirage_mount_list);
DECLARE_RWSEM(mirage_mount_list_lock);

struct mirage_umount_work {
	struct callback_head work;
};

/*
 * Check if a uid should trigger umount
 * For now, all app UIDs should umount
 */
bool mirage_uid_should_umount(uid_t uid)
{
	/* Always umount for app UIDs and isolated processes */
	if (is_appuid(uid) || is_isolated_process(uid)) {
		return true;
	}
	return false;
}

/*
 * Check if a path already exists in the umount list
 * Assumes mirage_mount_list_lock is already held (read or write)
 */
static bool __mirage_umount_path_exists_locked(const char *path)
{
	struct mount_entry *entry;

	list_for_each_entry(entry, &mirage_mount_list, list) {
		if (strcmp(entry->umountable, path) == 0) {
			return true;
		}
	}

	return false;
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
		pr_err("mirage: kern_path failed for %s (err: %d)\n", mnt, err);
		return;
	}

	/* Ensure that we are unmounting the actual root of the mount point */
	if (path.dentry != path.mnt->mnt_root) {
		pr_err("mirage: %s is not a mount root, skipping.\n", mnt);
		path_put(&path);
		return;
	}

	/* Execute the native unmount */
	err = path_umount(&path, flags);
	
	if (err && err != -ENOENT && err != -EINVAL) {
		pr_err("mirage: umount %s failed: %d\n", mnt, err);
	} else if (err == 0) {
		pr_debug("mirage: SUCCESS! %s unmounted from app namespace.\n", mnt);
	}
}

static void mirage_do_umount_work(struct callback_head *work)
{
	struct mirage_umount_work *mw = container_of(work, struct mirage_umount_work, work);
	const struct cred *saved;
	struct mount_entry *entry;

	saved = override_creds(mirage_cred);

	down_read(&mirage_mount_list_lock);
	list_for_each_entry(entry, &mirage_mount_list, list) {
		try_umount(entry->umountable, entry->flags);
	}
	up_read(&mirage_mount_list_lock);

	revert_creds(saved);
	kfree(mw);
}

void mirage_handle_umount(uid_t old_uid, uid_t new_uid)
{
	struct mirage_umount_work *mw;

	if (!mirage_kernel_umount_enabled) {
		pr_debug("mirage: Kernel umount disabled, skipping umount handling.\n");
		return;
	}

	if (!is_appuid(new_uid) && !is_isolated_process(new_uid)) {
		pr_debug("mirage: New UID %u is not an app or isolated UID, skipping umount.\n", new_uid);
		return;
	}

	if (!mirage_uid_should_umount(new_uid) && !is_isolated_process(new_uid)) {
		pr_debug("mirage: New UID %u does not require umount, skipping.\n", new_uid);
		return;
	}

	if (old_uid != 0) {
		pr_debug("mirage: Old UID %u is not root, skipping umount handling.\n", old_uid);
		return;
	}

	/* Fast path: Check if the list is empty without locking.
	 * list_empty is safe to call locklessly as it just compares pointers.
	 * Since umount entries are rarely added/removed during normal operation,
	 * this saves expensive override_creds and down_read calls.
	 */
	if (list_empty(&mirage_mount_list)) {
		return;
	}

	mw = kzalloc(sizeof(*mw), GFP_ATOMIC);
	if (!mw) return;

	init_task_work(&mw->work, mirage_do_umount_work);

	if (task_work_add(current, &mw->work, TWA_RESUME)) {
		pr_err("mirage: task_work_add failed!\n");
		kfree(mw);
	} else {
		pr_debug("mirage: App Launch Detected! Deferring umount work...\n");
	}

	return;
}

void mirage_handle_setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
	uid_t new_uid = ruid;
	uid_t old_uid = current_uid().val;

	if (old_uid != 0)
		return;

	mirage_handle_umount(old_uid, new_uid);

	return;
}

/*
 * Add a path to the umount list
 */
int mirage_umount_add(const char *path, unsigned int flags)
{
	struct mount_entry *new_entry;
	char *path_copy;

	if (!path) {
		pr_err("mirage: umount_add called with NULL path\n");
		return -EINVAL;
	}

	new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
	if (!new_entry) {
		pr_err("mirage: failed to allocate memory for mount entry\n");
		return -ENOMEM;
	}

	path_copy = kstrdup(path, GFP_KERNEL);
	if (!path_copy) {
		pr_err("mirage: failed to duplicate path string\n");
		kfree(new_entry);
		return -ENOMEM;
	}

	new_entry->umountable = path_copy;
	new_entry->flags = flags;

	down_write(&mirage_mount_list_lock);

	/* Double-check existence while holding the lock to avoid race conditions (TOCTOU) */
	if (__mirage_umount_path_exists_locked(path)) {
		pr_info("mirage: Path already in umount list, skipping add: %s\n", path);
		up_write(&mirage_mount_list_lock);
		kfree(path_copy);
		kfree(new_entry);
		return -EEXIST;
	}

	list_add_tail(&new_entry->list, &mirage_mount_list);
	up_write(&mirage_mount_list_lock);

	pr_info("mirage: ADDED to umount list: %s (flags=0x%x)\n", path, flags);

	return 0;
}

/*
 * Remove a path from the umount list
 */
int mirage_umount_del(const char *path)
{
	struct mount_entry *entry, *tmp;
	int found = 0;

	if (!path) {
		pr_err("mirage: umount_del called with NULL path\n");
		return -EINVAL;
	}

	down_write(&mirage_mount_list_lock);
	list_for_each_entry_safe(entry, tmp, &mirage_mount_list, list) {
		if (strcmp(entry->umountable, path) == 0) {
			list_del(&entry->list);
			kfree(entry->umountable);
			kfree(entry);
			found++;
			pr_info("mirage: REMOVED from umount list: %s\n", path);
		}
	}
	up_write(&mirage_mount_list_lock);

	return found ? 0 : -ENOENT;
}

/*
 * Wipe/clear all entries from the umount list
 */
int mirage_umount_wipe(void)
{
	struct mount_entry *entry, *tmp;
	int count = 0;

	down_write(&mirage_mount_list_lock);
	list_for_each_entry_safe(entry, tmp, &mirage_mount_list, list) {
		list_del(&entry->list);
		kfree(entry->umountable);
		kfree(entry);
		count++;
	}
	up_write(&mirage_mount_list_lock);

	return count ? 0 : -ENOENT;
}

/*
 * Procfs interface for managing umount list from userspace
 */
#ifdef MIRAGE_FS_PROC

#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#define MIRAGE_PROC_UMOUNT "mirage"

static struct proc_dir_entry *mirage_proc_dir;

/*
 * Show umount list in proc
 */
static int mirage_umount_list_show(struct seq_file *m, void *v)
{
	struct mount_entry *entry;

	down_read(&mirage_mount_list_lock);
	list_for_each_entry(entry, &mirage_mount_list, list) {
		seq_printf(m, "%s\t%u\n", entry->umountable, entry->flags);
	}
	up_read(&mirage_mount_list_lock);

	return 0;
}

static int mirage_umount_list_open(struct inode *inode, struct file *file)
{
	return single_open(file, mirage_umount_list_show, NULL);
}

/* proc_ops compatibility for kernels < 5.6 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops mirage_umount_list_proc_ops = {
	.proc_open	= mirage_umount_list_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};
#else
static const struct file_operations mirage_umount_list_proc_ops = {
	.open		= mirage_umount_list_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
#endif

/*
 * Write handler for umount_add
 */
static ssize_t mirage_umount_add_write(struct file *file,
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

	ret = mirage_umount_add(path, MIRAGE_UMOUNT_FLAG_NONE);
	if (ret < 0) {
		return ret;
	}

	return count;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops mirage_umount_add_proc_ops = {
	.proc_write	= mirage_umount_add_write,
};
#else
static const struct file_operations mirage_umount_add_proc_ops = {
	.write		= mirage_umount_add_write,
};
#endif

/*
 * Write handler for umount_del
 */
static ssize_t mirage_umount_del_write(struct file *file,
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

	if (mirage_umount_del(path) < 0) {
		return -ENOENT;
	}

	return count;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops mirage_umount_del_proc_ops = {
	.proc_write	= mirage_umount_del_write,
};
#else
static const struct file_operations mirage_umount_del_proc_ops = {
	.write		= mirage_umount_del_write,
};
#endif

/*
 * Write handler for umount_clear
 */
static ssize_t mirage_umount_clear_write(struct file *file,
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
		mirage_umount_wipe();
	}

	return count;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops mirage_umount_clear_proc_ops = {
	.proc_write	= mirage_umount_clear_write,
};
#else
static const struct file_operations mirage_umount_clear_proc_ops = {
	.write		= mirage_umount_clear_write,
};
#endif

/*
 * Read/Write handler for umount_enabled
 */
static int mirage_umount_enabled_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", mirage_kernel_umount_enabled ? 1 : 0);
	return 0;
}

static int mirage_umount_enabled_open(struct inode *inode, struct file *file)
{
	return single_open(file, mirage_umount_enabled_show, NULL);
}

static ssize_t mirage_umount_enabled_write(struct file *file,
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
		mirage_kernel_umount_enabled = true;
	} else {
		mirage_kernel_umount_enabled = false;
	}

	return count;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops mirage_umount_enabled_proc_ops = {
	.proc_open	= mirage_umount_enabled_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= mirage_umount_enabled_write,
};
#else
static const struct file_operations mirage_umount_enabled_proc_ops = {
	.open		= mirage_umount_enabled_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= mirage_umount_enabled_write,
};
#endif

/*
 * Initialize procfs interface
 */
int mirage_umount_proc_init(void)
{
	struct proc_dir_entry *entry;

	mirage_proc_dir = proc_mkdir(MIRAGE_PROC_UMOUNT, NULL);
	if (!mirage_proc_dir) {
		return -ENOMEM;
	}

	entry = proc_create("umount_list", 0444, mirage_proc_dir,
			    &mirage_umount_list_proc_ops);
	if (!entry) {
		goto out_remove_dir;
	}

	entry = proc_create("umount_add", 0200, mirage_proc_dir,
			    &mirage_umount_add_proc_ops);
	if (!entry) {
		goto out_remove_umount_list;
	}

	entry = proc_create("umount_del", 0200, mirage_proc_dir,
			    &mirage_umount_del_proc_ops);
	if (!entry) {
		goto out_remove_umount_add;
	}

	entry = proc_create("umount_clear", 0200, mirage_proc_dir,
			    &mirage_umount_clear_proc_ops);
	if (!entry) {
		goto out_remove_umount_del;
	}

	entry = proc_create("umount_enabled", 0644, mirage_proc_dir,
			    &mirage_umount_enabled_proc_ops);
	if (!entry) {
		goto out_remove_umount_clear;
	}

	return 0;

out_remove_umount_clear:
	remove_proc_entry("umount_clear", mirage_proc_dir);
out_remove_umount_del:
	remove_proc_entry("umount_del", mirage_proc_dir);
out_remove_umount_add:
	remove_proc_entry("umount_add", mirage_proc_dir);
out_remove_umount_list:
	remove_proc_entry("umount_list", mirage_proc_dir);
out_remove_dir:
	remove_proc_entry(MIRAGE_PROC_UMOUNT, NULL);
	return -ENOMEM;
}

/*
 * Cleanup procfs interface
 */
void mirage_umount_proc_exit(void)
{
	remove_proc_entry("umount_enabled", mirage_proc_dir);
	remove_proc_entry("umount_clear", mirage_proc_dir);
	remove_proc_entry("umount_del", mirage_proc_dir);
	remove_proc_entry("umount_add", mirage_proc_dir);
	remove_proc_entry("umount_list", mirage_proc_dir);
	remove_proc_entry(MIRAGE_PROC_UMOUNT, NULL);
}

#endif /* MIRAGE_FS_PROC */

/*
 * Initialize the kernel umount subsystem
 */
void mirage_kernel_umount_init(void)
{
	INIT_LIST_HEAD(&mirage_mount_list);

#ifdef MIRAGE_FS_PROC
	mirage_umount_proc_init();
#endif
}

/*
 * Cleanup the kernel umount subsystem
 */
void mirage_kernel_umount_exit(void)
{
#ifdef MIRAGE_FS_PROC
	mirage_umount_proc_exit();
#endif

	mirage_umount_wipe();
}
