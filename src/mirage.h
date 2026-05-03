/*
 * Mirage: A stackable virtual file system for Android content redirection.
 *   Based on WrapFS by Erez Zadok.
 *   This file contains the private declarations for mirage.
 */

#ifndef _MIRAGE_H_
#define _MIRAGE_H_

#include <linux/dcache.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/hashtable.h>
#include <linux/hash.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/seq_file.h>
#include <linux/statfs.h>
#include <linux/fs_stack.h>
#include <linux/magic.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/xattr.h>
#include <linux/limits.h>
#include <linux/exportfs.h>
#include <linux/rcupdate.h>
#ifdef MIRAGE_KERNEL_UMOUNT
/* Include kernel umount support */
#include "kernel_umount.h"
#endif
#include "compat.h"

/* The file system name for 'mount -t mirage' */
#define MIRAGE_NAME "mirage"

/* Mirage root inode number */
#define MIRAGE_ROOT_INO     1

/* Mirage magic number */
#define MIRAGE_FS_MAGIC 0xF18F

/* Max number of stacked directories */
#define MIRAGE_MAX_BRANCHES 5

/* Operations vectors defined in specific files */
extern struct file_system_type mirage_fs_type;
extern const struct file_operations mirage_main_fops;
extern const struct file_operations mirage_dir_fops;
extern const struct inode_operations mirage_main_iops;
extern const struct inode_operations mirage_dir_iops;
extern const struct inode_operations mirage_symlink_iops;
extern const struct super_operations mirage_sops;
extern const struct dentry_operations mirage_dops;
extern const struct address_space_operations mirage_aops;
extern const struct vm_operations_struct mirage_vm_ops;
extern const struct export_operations mirage_export_ops;
extern const struct xattr_handler *mirage_xattr_handlers[];

/* Cache and lifecycle management functions */
extern struct cred *mirage_cred;
extern int mirage_init_inode_cache(void);
extern void mirage_destroy_inode_cache(void);
extern struct kmem_cache *mirage_dentry_cachep;
extern int mirage_init_dentry_cache(void);
extern void mirage_destroy_dentry_cache(void);
extern struct kmem_cache *mirage_dirent_cachep;
extern int mirage_init_dirent_cache(void);
extern void mirage_destroy_dirent_cache(void);
extern int new_dentry_private_data(struct dentry *dentry);
extern void free_dentry_private_data(struct dentry *dentry);

/* Core lookup and interpose functions */
extern struct dentry *mirage_vfs_lookup(struct inode *dir, struct dentry *dentry,
				      				    unsigned int flags);
extern struct inode *mirage_iget(struct super_block *sb, struct inode *lower_inode);
extern struct dentry *__mirage_interpose(struct dentry *dentry,
					 					  struct super_block *sb,
					 					  struct path *lower_path);
extern int mirage_fill_super(struct super_block *sb, void *raw_data, int silent);
extern int mirage_vfs_statfs(struct dentry *dentry, struct kstatfs *buf);
#if defined(HAVE_ITERATE_SHARED) || defined(HAVE_ITERATE)
extern int mirage_vfs_iterate(struct file *file, struct dir_context *ctx);
#else
extern int mirage_vfs_readdir(struct file *file, void *dirent, filldir_t filldir);
#endif
extern int mirage_vfs_mmap(struct file *file, struct vm_area_struct *vma);
extern int mirage_test_inode(struct inode *inode, void *data);
extern int mirage_set_inode(struct inode *inode, void *data);

/* Tracepoint hook functions */
extern int mirage_init_tp_hooks(void);
extern void mirage_exit_tp_hooks(void);

/* Used to track already emitted dirents for deduplication */
struct mirage_dirent {
	struct hlist_node hash; /* For fast O(1) deduplication lookups */
	struct list_head list;  /* For ordered O(1) emissions */
	char name[NAME_MAX + 1];
	int len; u64 ino;
	unsigned int d_type;
};

/* File private data: link to the real underlying file(s) */
struct mirage_file_info {
	struct file *lower_files[MIRAGE_MAX_BRANCHES];
	int num_lower_files;
	bool ghost_emitted;
};

/* Inode data in memory */
struct mirage_inode_info {
	/* vfs_inode at Offset 0. */
	struct inode vfs_inode;
	struct inode *lower_inode;

	/* Reordered strictly by alignment size to eliminate RAM padding */
	u64 cache_version;
	struct mutex readdir_mutex;
	/* For readdir deduplication (per-inode caching) */
	DECLARE_HASHTABLE(dirent_hashtable, 8);  /* 2^8 = 256 buckets */
	struct list_head dirents_list;           /* Ordered emission list */
	bool cache_populated;
};

/* Dentry data in memory */
struct mirage_dentry_info {
#ifdef MIRAGE_RCU_PATH_ACCESS
	struct rcu_head rcu;	/* For kfree_rcu() deferred freeing */
#endif
	spinlock_t lock;	/* Protects lower_paths (write side only) */
	struct path lower_paths[MIRAGE_MAX_BRANCHES];
	int num_lower_paths;
};

/* super-block data in memory */
struct mirage_sb_info {
	struct super_block *lower_sb;
	struct path lower_paths[MIRAGE_MAX_BRANCHES];
	int num_lower_paths;
	bool has_inject;
	bool has_upperdir;
	char inject_name[NAME_MAX]; /* Name of the file to intercept */
	size_t inject_name_len;     /* Precomputed length of inject_name */
	u32 inject_name_hash;       /* Precomputed hash of inject_name */
	struct path inject_path;    /* Actual path of the modified file */
	/* Original path strings saved for /proc/mounts show_options.
	 * d_path() on private vfsmount clones returns "/" — unusable.
	 * Store the strings from the mount options directly instead. */
	char lower_path_strs[MIRAGE_MAX_BRANCHES][PATH_MAX];
	char inject_path_str[PATH_MAX];
	struct file_system_type *fake_type; /* For export operations to identify the filesystem type */
};

/* Structure to safely pass assembly data to fill_super */
struct nomount_mount_data {
	const char *dev_name;
	void *raw_data;
};

/*
 * Inode to private data conversion.
 * Since struct inode is embedded, this will always return a valid pointer.
 */
static inline struct nomount_inode_info *mirage_inode(const struct inode *inode)
{
	return container_of(inode, struct mirage_inode_info, vfs_inode);
}

/* Helper macros for accessing private data */
#define mirage_dentry(dent) ((struct mirage_dentry_info *)(dent)->d_fsdata)
#define mirage_sb(super) ((struct mirage_sb_info *)(super)->s_fs_info)
#define mirage_file(file) ((struct mirage_file_info *)((file)->private_data))

/* Helper functions to get/set lower (real) objects */
static inline struct file *mirage_lower_file(const struct file *file)
{
	return mirage_file(file)->lower_files[0]; /* Return topmost file for general I/O */
}

static inline struct inode *mirage_lower_inode(const struct inode *inode)
{
	return mirage_inode(inode)->lower_inode;
}

/* Copying and handling lower paths safely */
static inline void pathcpy(struct path *dst, const struct path *src)
{
	/* Struct assignment allows the compiler to emit LDP/STP
	 * instructions, doing the copy in a single clock cycle
	 * instead of member-by-member.
	 */
	*dst = *src;
}

/* Safely retrieve the lower path while holding the dentry lock */
#ifdef MIRAGE_RCU_PATH_ACCESS
/*
 * RCU version: no spinlock needed for readers.
 *
 * SAFETY: lower_paths[] is IMMUTABLE after setup in nomount_lookup()/fill_super.
 * Writers only set lower_paths[] once during dentry creation, never modify after.
 * The spinlock in free_dentry_private_data() only protects the cleanup path.
 *
 * RCU protects: info pointer existence
 * Immutable:    info->lower_paths[], info->num_lower_paths
 * Race-free:    d_fsdata cleared under dentry->d_lock before kfree_rcu()
 */
static inline void get_lower_path(const struct dentry *dent, struct path *lower_path)
{
	struct mirage_dentry_info *info;

	rcu_read_lock();
	info = rcu_dereference(dent->d_fsdata);
	if (info && info->num_lower_paths > 0) {
		pathcpy(lower_path, &info->lower_paths[0]);
		path_get(lower_path);
	} else {
		lower_path->dentry = NULL;
		lower_path->mnt = NULL;
	}
	rcu_read_unlock();
}

/* RCU version: no spinlock needed for readers - see get_lower_path() for safety */
static inline int get_all_lower_paths(const struct dentry *dent, struct path *lower_paths)
{
	int i, num;
	struct mirage_dentry_info *info;

	rcu_read_lock();
	info = rcu_dereference(dent->d_fsdata);
	num = info ? info->num_lower_paths : 0;
	for (i = 0; i < num; i++) {
		pathcpy(&lower_paths[i], &info->lower_paths[i]);
		path_get(&lower_paths[i]);
	}
	rcu_read_unlock();
	return num;
}
#else
/* Legacy spinlock version */
static inline void get_lower_path(const struct dentry *dent, struct path *lower_path)
{
	struct mirage_dentry_info *info;

	info = mirage_dentry(dent);
	if (!info) {
		lower_path->dentry = NULL;
		lower_path->mnt = NULL;
		return;
	}
	spin_lock(&info->lock);
	if (info->num_lower_paths > 0) {
		pathcpy(lower_path, &info->lower_paths[0]);
		path_get(lower_path);
	} else {
		lower_path->dentry = NULL;
		lower_path->mnt = NULL;
	}
	spin_unlock(&info->lock);
}

static inline int get_all_lower_paths(const struct dentry *dent, struct path *lower_paths)
{
	int i, num;
	struct mirage_dentry_info *info;

	info = mirage_dentry(dent);
	if (!info)
		return 0;

	spin_lock(&info->lock);
	num = info->num_lower_paths;
	for (i = 0; i < num; i++) {
		pathcpy(&lower_paths[i], &info->lower_paths[i]);
		path_get(&lower_paths[i]);
	}
	spin_unlock(&info->lock);
	return num;
}
#endif

static inline void put_lower_path(const struct dentry *dent, struct path *lower_path)
{
	if (lower_path->dentry)
		path_put(lower_path);
}

static inline void put_all_lower_paths(const struct dentry *dent, struct path *lower_paths, int num)
{
	int i;
	for (i = 0; i < num; i++) {
		if (lower_paths[i].dentry)
			path_put(&lower_paths[i]);
	}
}

/* Helpers to set private data - MUST only be called before dentry is visible */
static inline void set_lower_inode(struct inode *inode, struct inode *lowernode)
{
	mirage_inode(inode)->lower_inode = lowernode;
}

/*
 * Set lower path(s) for a dentry.
 *
 * SAFETY: Must only be called during dentry creation (nomount_lookup/fill_super)
 * BEFORE the dentry is made visible via d_add() or d_splice_alias().
 * At that point, no RCU readers can exist, so no locking is needed.
 *
 * Called from:
 * - nomount_lookup(): after new_dentry_private_data(), before d_add/d_splice_alias
 * - nomount_fill_super(): for root dentry, before sb->s_root assignment
 */
static inline void set_lower_paths(struct dentry *dent, struct path *lower_paths, int num_paths)
{
	int i;
	for (i = 0; i < num_paths; i++) {
		pathcpy(&mirage_dentry(dent)->lower_paths[i], &lower_paths[i]);
	}
	mirage_dentry(dent)->num_lower_paths = num_paths;
}

/* Locking helpers for directory operations */
static inline struct dentry *lock_parent(struct dentry *dentry)
{
	struct dentry *dir = dget_parent(dentry);
	inode_lock(d_inode(dir));
	return dir;
}

static inline void unlock_dir(struct dentry *dir)
{
	inode_unlock(d_inode(dir));
	dput(dir);
}

#endif	/* _MIRAGE_H_ */
