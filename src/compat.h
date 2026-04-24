/*
 * NoMountFS: Compatibility layer for different Linux Kernel versions.
 * This ensures that NoMountFS can be compiled on Android kernels from 3.4 to 6.x.
 */

#ifndef _NOMOUNT_COMPAT_H_
#define _NOMOUNT_COMPAT_H_

#include <linux/version.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/path.h>
#include <linux/syscalls.h>

#ifndef TWA_RESUME
#define TWA_RESUME true
#endif

/* iversion.h was introduced in kernel 4.16; use raw i_version on older kernels */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
#include <linux/iversion.h>
#endif

/* * Inode locking: i_mutex (old) vs i_rwsem (new).
 * Kernel 4.5 renamed i_mutex and introduced inode_lock() helper.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0)
    #define inode_lock(inode) mutex_lock(&(inode)->i_mutex)
    #define inode_unlock(inode) mutex_unlock(&(inode)->i_mutex)
    #define inode_lock_nested(inode, subclass) mutex_lock_nested(&(inode)->i_mutex, (subclass))
#endif

/* * Dentry alias list handling.
 * Kernel 3.11 moved d_alias into the d_u union.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 11, 0)
    #define d_alias_list d_alias
#else
    #define d_alias_list d_u.d_alias
#endif

/* * Directory iteration: iterate_shared (new) vs readdir (old).
 * Introduced in Kernel 4.7.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0)
    #define HAVE_ITERATE_SHARED
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
    #define HAVE_ITERATE
#endif

/* * Symlink handling: get_link (new) vs follow_link (old).
 * The prototype changed significantly in Kernel 4.5.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0)
    #define LEGACY_FOLLOW_LINK
#endif

/* * User ID management.
 * Handling the transition to kuid_t for very old kernels.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 5, 0)
    #define current_uid_val() (current_fsuid())
#else
    #define current_uid_val() (from_kuid(&init_user_ns, current_fsuid()))
#endif

/* * File I/O: read_iter (new) vs aio_read/read (old).
 * read_iter was standardized around 3.16/3.19.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 16, 0)
    #define HAVE_LEGACY_IO
#endif

/* * Rename signature handling.
 * Kernel 4.9+ unified rename2 into rename and added flags.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
    #define RENAME_HAS_FLAGS
#endif

/* Compatibility for i_version in Kernel 5.4+ */
static inline void nomount_set_iversion(struct inode *inode, u64 val)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
	inode_set_iversion(inode, val);
#else
	inode->i_version = val;
#endif
}

static inline void nomount_inc_iversion(struct inode *inode)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
	inode_inc_iversion(inode);
#else
	inode->i_version++;
#endif
}

/* * VFS helper to check if an inode is a directory safely.
 */
static inline bool nomount_is_dir(const struct inode *inode)
{
	return S_ISDIR(inode->i_mode);
}

/* * Lookup flags: 'unsigned int' in new kernels, 'int' in old ones.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
    typedef unsigned int nomount_lookup_flags;
#else
    typedef int nomount_lookup_flags;
#endif

/* * d_really_is_positive/negative helpers.
 * These were introduced to handle RCU-walks in newer kernels.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 18, 0)
    static inline bool d_really_is_positive(const struct dentry *dentry)
    {
        return dentry->d_inode != NULL;
    }
    static inline bool d_really_is_negative(const struct dentry *dentry)
    {
        return dentry->d_inode == NULL;
    }
#endif

/*
 * nomount_clone_private_mount: clone a vfsmount privately so it is
 * detached from the mount namespace.
 *
 * A private clone is not visible to follow_mount/lookup_mnt in the
 * public namespace, so dentry_open through it always reaches the real
 * underlying filesystem directly — even when nomountfs is mounted over
 * the same path as one of its lower layers.
 *
 * clone_private_mount() was added in 3.18 but is not always exported
 * to out-of-tree modules on Android kernels. When NOMOUNT_NO_CLONE_PRIVATE_MOUNT
 * is defined (set by Makefile when the symbol isn't found in fs/namespace.c),
 * we resolve it at runtime via kallsyms_lookup_name instead.
 *
 * Returns ERR_PTR on error, never NULL.
 */
#include <linux/mount.h>
#include <linux/kallsyms.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 18, 0)

#ifdef NOMOUNT_NO_CLONE_PRIVATE_MOUNT
typedef struct vfsmount *(*clone_private_mount_fn_t)(const struct path *);
static inline struct vfsmount *nomount_clone_private_mount(struct path *path)
{
	static clone_private_mount_fn_t fn;
	struct vfsmount *mnt;
	if (!fn)
		fn = (clone_private_mount_fn_t)
			kallsyms_lookup_name("clone_private_mount");
	if (!fn) {
		mnt = mntget(path->mnt);
		return mnt ? mnt : ERR_PTR(-ENOMEM);
	}
	mnt = fn(path);
	return mnt ? mnt : ERR_PTR(-ENOMEM);
}
#else
static inline struct vfsmount *nomount_clone_private_mount(struct path *path)
{
	struct vfsmount *mnt = clone_private_mount(path);
	return mnt ? mnt : ERR_PTR(-ENOMEM);
}
#endif

#else /* kernel < 3.18 */
static inline struct vfsmount *nomount_clone_private_mount(struct path *path)
{
	struct vfsmount *mnt = mntget(path->mnt);
	return mnt ? mnt : ERR_PTR(-ENOMEM);
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0) && defined(CONFIG_NOMOUNT_FS_KERNEL_UMOUNT)
__weak int path_umount(struct path *path, int flags)
{
	char buf[256] = {0};
	int ret;

	// -1 on the size as implicit null termination
	// as we zero init the thing
	char *usermnt = d_path(path, buf, sizeof(buf) - 1);
	if (!(usermnt && usermnt != buf)) {
		ret = -ENOENT;
		goto out;
	}

	mm_segment_t old_fs = get_fs();
	set_fs(KERNEL_DS);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 17, 0)
	ret = ksys_umount((char __user *)usermnt, flags);
#else
	ret = (int)sys_umount((char __user *)usermnt, flags);
#endif

	set_fs(old_fs);

	// release ref here! user_path_at increases it
	// then only cleans for itself
out:
	path_put(path); 
	return ret;
}
#endif

#endif /* _NOMOUNT_COMPAT_H_ */
