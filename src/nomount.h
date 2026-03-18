/*
 * NoMountFS: A stackable file system for Android content redirection.
 *   Based on WrapFS by Erez Zadok.
 *   This file contains the private declarations for nomountfs.
 */

#ifndef _NOMOUNT_H_
#define _NOMOUNT_H_

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
#include <linux/exportfs.h>
#include <linux/rcupdate.h>
/* Include kernel umount support */
#include "nomount_umount.h"

#ifndef TWA_RESUME
#define TWA_RESUME true
#endif

/* The file system name for 'mount -t nomountfs' */
#define NOMOUNT_NAME "nomountfs"

/* Nomountfs root inode number */
#define NOMOUNT_ROOT_INO     1

/* Nomountfs magic number */
#define NOMOUNT_FS_MAGIC 0xF18F

/* Max number of stacked directories */
#define NOMOUNT_MAX_BRANCHES 5

/* Operations vectors defined in specific files */
extern struct cred *nmfs_cred;
extern const struct file_operations nomount_main_fops;
extern const struct file_operations nomount_dir_fops;
extern const struct inode_operations nomount_main_iops;
extern const struct inode_operations nomount_dir_iops;
extern const struct inode_operations nomount_symlink_iops;
extern const struct super_operations nomount_sops;
extern const struct dentry_operations nomount_dops;
extern const struct address_space_operations nomount_aops;
extern const struct vm_operations_struct nomount_vm_ops;
extern const struct export_operations nomount_export_ops;
extern const struct xattr_handler *nomount_xattr_handlers[];

/* Cache and lifecycle management functions */
void setup_nmfs_cred(void);
extern int nomount_init_inode_cache(void);
extern void nomount_destroy_inode_cache(void);
extern int nomount_init_dentry_cache(void);
extern void nomount_destroy_dentry_cache(void);
extern int new_dentry_private_data(struct dentry *dentry);
extern void free_dentry_private_data(struct dentry *dentry);

/* Core lookup and interpose functions */
extern struct dentry *nomount_lookup(struct inode *dir, struct dentry *dentry,
				      				  unsigned int flags);
extern struct inode *nomount_iget(struct super_block *sb, struct inode *lower_inode);
extern struct dentry *__nomount_interpose(struct dentry *dentry,
					 					  struct super_block *sb,
					 					  struct path *lower_path);
extern int nomount_fill_super(struct super_block *sb, void *raw_data, int silent);
extern int nomount_statfs(struct dentry *dentry, struct kstatfs *buf);
#if defined(HAVE_ITERATE_SHARED) || defined(HAVE_ITERATE)
extern int nomount_iterate(struct file *file, struct dir_context *ctx);
#else
extern int nomount_readdir(struct file *file, void *dirent, filldir_t filldir);
#endif
extern int nomount_mmap(struct file *file, struct vm_area_struct *vma);
extern int nomount_test_inode(struct inode *inode, void *data);
extern int nomount_set_inode(struct inode *inode, void *data);

/* Used to track already emitted dirents for deduplication */
struct nomount_dirent {
	struct hlist_node hash; /* For fast O(1) deduplication lookups */
	struct list_head list;  /* For ordered O(1) emissions */
	char *name;
	int len;
	u64 ino;
	unsigned int d_type;
};

/* File private data: link to the real underlying file(s) */
struct nomount_file_info {
	struct file *lower_files[NOMOUNT_MAX_BRANCHES];
	int num_lower_files;
	const struct vm_operations_struct *lower_vm_ops;
	bool ghost_emitted;
	
	/* For readdir deduplication */
	DECLARE_HASHTABLE(dirent_hashtable, 8); /* 2^8 = 256 buckets */
	struct list_head dirents_list;          /* Ordered emission list */
	struct mutex readdir_mutex;
};

/* Nomountfs inode data in memory */
struct nomount_inode_info {
	struct inode *lower_inode;
	struct inode vfs_inode;
};

/* Nomountfs dentry data in memory */
struct nomount_dentry_info {
#ifdef CONFIG_NOMOUNT_RCU_PATH_ACCESS
	struct rcu_head rcu;	/* For kfree_rcu() deferred freeing */
#endif
	spinlock_t lock;	/* Protects lower_paths (write side only) */
	struct path lower_paths[NOMOUNT_MAX_BRANCHES];
	int num_lower_paths;
};

/* Nomountfs super-block data in memory */
struct nomount_sb_info {
	struct super_block *lower_sb;
	struct path lower_paths[NOMOUNT_MAX_BRANCHES];
	int num_lower_paths;
	bool has_inject;
	char inject_name[NAME_MAX]; /* Name of the file to intercept */
	struct path inject_path;    /* Actual path of the modified file */
	/* Original path strings saved for /proc/mounts show_options.
	 * d_path() on private vfsmount clones returns "/" — unusable.
	 * Store the strings from the mount options directly instead. */
	char lower_path_strs[NOMOUNT_MAX_BRANCHES][PATH_MAX];
	char inject_path_str[PATH_MAX];
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
static inline struct nomount_inode_info *NOMOUNT_I(const struct inode *inode)
{
	return container_of(inode, struct nomount_inode_info, vfs_inode);
}

/* Helper macros for accessing private data */
#define NOMOUNT_D(dent) ((struct nomount_dentry_info *)(dent)->d_fsdata)
#define NOMOUNT_SB(super) ((struct nomount_sb_info *)(super)->s_fs_info)
#define NOMOUNT_F(file) ((struct nomount_file_info *)((file)->private_data))

/* Helper functions to get/set lower (real) objects */
static inline struct file *nomount_lower_file(const struct file *f)
{
	return NOMOUNT_F(f)->lower_files[0]; /* Return topmost file for general I/O */
}

static inline struct inode *nomount_lower_inode(const struct inode *i)
{
	return NOMOUNT_I(i)->lower_inode;
}

static inline struct super_block *nomount_lower_super(const struct super_block *sb)
{
	return NOMOUNT_SB(sb)->lower_sb;
}

/* Copying and handling lower paths safely */
static inline void pathcpy(struct path *dst, const struct path *src)
{
	dst->dentry = src->dentry;
	dst->mnt = src->mnt;
}

/* Safely retrieve the lower path while holding the dentry lock */
#ifdef CONFIG_NOMOUNT_RCU_PATH_ACCESS
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
static inline void nomount_get_lower_path(const struct dentry *dent, struct path *lower_path)
{
	struct nomount_dentry_info *info;

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
#else
/* Legacy spinlock version */
static inline void nomount_get_lower_path(const struct dentry *dent, struct path *lower_path)
{
	struct nomount_dentry_info *info;

	info = NOMOUNT_D(dent);
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
#endif

static inline void nomount_put_lower_path(const struct dentry *dent, struct path *lower_path)
{
	if (lower_path->dentry)
		path_put(lower_path);
}

#ifdef CONFIG_NOMOUNT_RCU_PATH_ACCESS
/* RCU version: no spinlock needed for readers - see nomount_get_lower_path() for safety */
static inline int nomount_get_all_lower_paths(const struct dentry *dent, struct path *lower_paths)
{
	int i, num;
	struct nomount_dentry_info *info;

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
static inline int nomount_get_all_lower_paths(const struct dentry *dent, struct path *lower_paths)
{
	int i, num;
	struct nomount_dentry_info *info;

	info = NOMOUNT_D(dent);
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

static inline void nomount_put_all_lower_paths(const struct dentry *dent, struct path *lower_paths, int num)
{
	int i;
	for (i = 0; i < num; i++) {
		if (lower_paths[i].dentry)
			path_put(&lower_paths[i]);
	}
}

/* Helpers to set private data - MUST only be called before dentry is visible */
static inline void nomount_set_lower_inode(struct inode *inode, struct inode *lowernode)
{
	NOMOUNT_I(inode)->lower_inode = lowernode;
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
static inline void nomount_set_lower_path(struct dentry *dent, struct path *lower_path)
{
	pathcpy(&NOMOUNT_D(dent)->lower_paths[0], lower_path);
	NOMOUNT_D(dent)->num_lower_paths = 1;
}

static inline void nomount_set_lower_paths(struct dentry *dent, struct path *lower_paths, int num_paths)
{
	int i;
	for (i = 0; i < num_paths; i++) {
		pathcpy(&NOMOUNT_D(dent)->lower_paths[i], &lower_paths[i]);
	}
	NOMOUNT_D(dent)->num_lower_paths = num_paths;
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

#endif	/* _NOMOUNT_H_ */
