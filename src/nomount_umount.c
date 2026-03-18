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
#include <linux/security.h>
#include "objsec.h"

#include "nomount.h"
#include "nomount_umount.h"

extern int path_umount(struct path *path, int flags);
struct cred *nmfs_cred;
/* Feature control */
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
/*
 * Check if uid is in the application UID range
 */
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
 */
bool nomount_umount_path_exists(const char *path)
{
	struct mount_entry *entry;
	bool found = false;

	down_read(&nomount_mount_list_lock);
	list_for_each_entry(entry, &nomount_mount_list, list) {
		if (strcmp(entry->umountable, path) == 0) {
			found = true;
			break;
		}
	}
	up_read(&nomount_mount_list_lock);

	return found;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 14, 0)
struct lsm_context {
    char *context;
    u32 len;
};

static int __security_secid_to_secctx(u32 secid, struct lsm_context *cp)
{
    return security_secid_to_secctx(secid, &cp->context, &cp->len);
}
static void __security_release_secctx(struct lsm_context *cp)
{
    security_release_secctx(cp->context, cp->len);
}
#else
#define __security_secid_to_secctx security_secid_to_secctx
#define __security_release_secctx security_release_secctx
#endif

static bool nmfs_is_zygote(const struct cred *cred)
{
	const char *fallback_context = "u:r:zygote:s0";
    if (!cred) {
        return false;
    }
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 18, 0)
    const struct task_security_struct *tsec = selinux_cred(cred);
#else
    const struct cred_security_struct *tsec = selinux_cred(cred);
#endif
    if (!tsec) {
        return false;
    }

    // Slow path fallback: string comparison (only before cache is initialized)
    struct lsm_context ctx;
    bool result;
    if (__security_secid_to_secctx(tsec->sid, &ctx)) {
        return false;
    }
    result = strncmp(fallback_context, ctx.context, ctx.len) == 0;
    __security_release_secctx(&ctx);
    return result;
}

static int transive_to_domain(const char *domain, struct cred *cred)
{
    u32 sid;
    int error;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 18, 0)
    struct task_security_struct *tsec;
#else
    struct cred_security_struct *tsec;
#endif
    tsec = selinux_cred(cred);
    if (!tsec) {
        return -1;
    }
    error = security_secctx_to_secid(domain, strlen(domain), &sid);
    if (!error) {
        tsec->sid = sid;
        tsec->create_sid = 0;
        tsec->keycreate_sid = 0;
        tsec->sockcreate_sid = 0;
    }
    return error;
}

void setup_nmfs_cred(void)
{
    if (!nmfs_cred) {
        return;
    }

    int err = transive_to_domain("u:r:su:s0", nmfs_cred);
    if (err) {
    }
}

static void nmfs_umount_mnt(struct path *path, int flags)
{
    int err = path_umount(path, flags);
    if (err) {
        pr_err("nomount: umount %s failed: %d\n", path->dentry->d_iname, err);
    }
}

static void try_umount(const char *mnt, int flags)
{
    struct path path;
    int err = kern_path(mnt, 0, &path);
    if (err) {
        return;
    }

    if (path.dentry != path.mnt->mnt_root) {
        path_put(&path);
        return;
    }

    nmfs_umount_mnt(&path, flags);
}

struct nmfs_umount_tw {
    struct callback_head cb;
};

static void umount_tw_func(struct callback_head *cb)
{
    struct nmfs_umount_tw *tw = container_of(cb, struct nmfs_umount_tw, cb);
    const struct cred *saved = override_creds(nmfs_cred);
    struct mount_entry *entry;

	down_read(&nomount_mount_list_lock);
	list_for_each_entry(entry, &nomount_mount_list, list) {
		try_umount(entry->umountable, entry->flags);
	}
	up_read(&nomount_mount_list_lock);

    revert_creds(saved);

    kfree(tw);
}

int nmfs_handle_umount(uid_t old_uid, uid_t new_uid)
{
    struct nmfs_umount_tw *tw;
    bool is_zygote_child;
    int err;

    setup_nmfs_cred();

	if (nmfs_cred) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 18, 0)
		struct task_security_struct *tsec = selinux_cred(nmfs_cred);
#else
		struct cred_security_struct *tsec = selinux_cred(nmfs_cred);
#endif
	}

    if (!nomount_kernel_umount_enabled) {
        return 0;
    }

    if (!nmfs_cred) {
        return 0;
    }
    if (!is_appuid(new_uid) && !is_isolated_process(new_uid)) {
        return 0;
    }

    if (!nomount_uid_should_umount(new_uid) && !is_isolated_process(new_uid)) {
        return 0;
    }

    is_zygote_child = nmfs_is_zygote(get_current_cred());
    if (!is_zygote_child) {
        return 0;
    }

    tw = kzalloc(sizeof(*tw), GFP_ATOMIC);
    if (!tw) {
        return 0;
    }

    tw->cb.func = umount_tw_func;

    err = task_work_add(current, &tw->cb, TWA_RESUME);
    if (err) {
        kfree(tw);
    }

    return 0;
}

int nmfs_handle_setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
    uid_t new_uid = ruid;
    uid_t old_uid = current_uid().val;

	if (0 != old_uid) {
		return 0;
	}

	if (!nomount_uid_should_umount(new_uid)) {
		return 0;
	}

	if (!is_appuid(new_uid) && !is_isolated_process(new_uid)) {
        return 0;
    }

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
	int ret = 0;

	if (!path)
		return -EINVAL;

	if (nomount_umount_path_exists(path)) {
		return -EEXIST;
	}

	new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
	if (!new_entry) {
		return -ENOMEM;
	}

	path_copy = kstrdup(path, GFP_KERNEL);
	if (!path_copy) {
		kfree(new_entry);
		return -ENOMEM;
	}

	new_entry->umountable = path_copy;
	new_entry->flags = flags;

	down_write(&nomount_mount_list_lock);
	list_add_tail(&new_entry->list, &nomount_mount_list);
	up_write(&nomount_mount_list_lock);

	pr_info("nomount: ADDED to umount list: %s (flags=0x%x)\n", path, flags);

	return ret;
}

/*
 * Remove a path from the umount list
 */
int nomount_umount_del(const char *path)
{
	struct mount_entry *entry, *tmp;
	int found = 0;

	if (!path)
		return -EINVAL;

	down_write(&nomount_mount_list_lock);
	list_for_each_entry_safe(entry, tmp, &nomount_mount_list, list) {
		if (strcmp(entry->umountable, path) == 0) {
			list_del(&entry->list);
			kfree(entry->umountable);
			kfree(entry);
			found++;
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
	}
	up_write(&nomount_mount_list_lock);

	return 0;
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
#ifdef CONFIG_NOMOUNT_FS_PROC

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
	size_t len;

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

	if (nomount_umount_add(path, NOMOUNT_UMOUNT_FLAG_NONE) < 0) {
		return -EEXIST;
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
	size_t len;

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
	size_t len;

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
	size_t len;

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

#endif /* CONFIG_NOMOUNT_FS_PROC */

/*
 * Initialize the kernel umount subsystem
 */
void nomount_kernel_umount_init(void)
{
	INIT_LIST_HEAD(&nomount_mount_list);

#ifdef CONFIG_NOMOUNT_FS_PROC
	nomount_umount_proc_init();
#endif
}

/*
 * Cleanup the kernel umount subsystem
 */
void nomount_kernel_umount_exit(void)
{
#ifdef CONFIG_NOMOUNT_FS_PROC
	nomount_umount_proc_exit();
#endif

	nomount_umount_wipe();
}
