/*
 * NoMountFS: Inode operations
 * Full R/W support with xattr (SELinux), rename, symlink and legacy compatibility.
 */

#include "nomount.h"
#include "compat.h"
#include <linux/xattr.h>

static int nomount_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool want_excl)
{
	int err;
	struct dentry *alias;
	struct dentry *lower_parent_dentry = NULL;
	struct nomount_dentry_info *info = rcu_dereference_raw(dentry->d_fsdata);

        /* VFS holds parent i_mutex, so for that extract raw path is secure on this context. */
	struct path lower_path = info->lower_paths[0]; 
	struct dentry *lower_dentry = lower_path.dentry;

	lower_parent_dentry = lock_parent(lower_dentry);

	err = vfs_create(d_inode(lower_parent_dentry), lower_dentry, mode, want_excl);
	if (err) goto out;

	/* Interpose the new dentry - check for errors properly */
	alias = __nomount_interpose(dentry, dir->i_sb, &lower_path);
	if (IS_ERR(alias)) {
		err = PTR_ERR(alias);
		goto out;
	} else if (alias && alias != dentry) {
		/* If d_splice_alias returns a valid hashed alias from the cache,
		 * we must drop the extra reference. */
		dput(alias);
	}
	err = 0;

	fsstack_copy_attr_times(dir, nomount_lower_inode(dir));
	fsstack_copy_inode_size(dir, d_inode(lower_parent_dentry));

out:
	unlock_dir(lower_parent_dentry);
	return err;
}

static int nomount_unlink(struct inode *dir, struct dentry *dentry)
{
	int err;
	struct inode *lower_dir_inode = nomount_lower_inode(dir);
	struct dentry *lower_dir_dentry;
	struct nomount_dentry_info *info = rcu_dereference_raw(dentry->d_fsdata);

	struct dentry *lower_dentry = info->lower_paths[0].dentry;

	lower_dir_dentry = lock_parent(lower_dentry);

	err = vfs_unlink(lower_dir_inode, lower_dentry, NULL);
	if (err) goto out;

	if (d_inode(dentry)) {
		set_nlink(d_inode(dentry), nomount_lower_inode(d_inode(dentry))->i_nlink);
		/* Sync deleted file ctime for apps holding open file descriptors */
		fsstack_copy_attr_times(d_inode(dentry), nomount_lower_inode(d_inode(dentry)));
	}
	d_drop(dentry);
out:
	unlock_dir(lower_dir_dentry);
	return err;
}

static int nomount_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	int err;
	struct dentry *alias;
	struct dentry *lower_parent_dentry;
	struct nomount_dentry_info *info = rcu_dereference_raw(dentry->d_fsdata);
	
	struct path lower_path = info->lower_paths[0];
	struct dentry *lower_dentry = lower_path.dentry;

	lower_parent_dentry = lock_parent(lower_dentry);

	err = vfs_mkdir(d_inode(lower_parent_dentry), lower_dentry, mode);
	if (err) goto out;

	alias = __nomount_interpose(dentry, dir->i_sb, &lower_path);
	if (IS_ERR(alias)) {
		err = PTR_ERR(alias);
		goto out;
	} else if (alias && alias != dentry) {
		dput(alias);
	}
	err = 0;

	fsstack_copy_attr_times(dir, nomount_lower_inode(dir));
	fsstack_copy_inode_size(dir, d_inode(lower_parent_dentry));
	set_nlink(dir, nomount_lower_inode(dir)->i_nlink);

out:
	unlock_dir(lower_parent_dentry);
	return err;
}

static int nomount_rmdir(struct inode *dir, struct dentry *dentry)
{
	int err;
	struct dentry *lower_dir_dentry;
	struct nomount_dentry_info *info = rcu_dereference_raw(dentry->d_fsdata);
	
	struct dentry *lower_dentry = info->lower_paths[0].dentry;

	lower_dir_dentry = lock_parent(lower_dentry);

	err = vfs_rmdir(d_inode(lower_dir_dentry), lower_dentry);
	if (err) goto out;

	d_drop(dentry);
	if (d_inode(dentry)) {
		clear_nlink(d_inode(dentry));
		/* Sync deleted directory ctime */
		fsstack_copy_attr_times(d_inode(dentry), nomount_lower_inode(d_inode(dentry)));
	}
	
	fsstack_copy_attr_times(dir, d_inode(lower_dir_dentry));
	fsstack_copy_inode_size(dir, d_inode(lower_dir_dentry));
	set_nlink(dir, d_inode(lower_dir_dentry)->i_nlink);

out:
	unlock_dir(lower_dir_dentry);
	return err;
}

static int nomount_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	int err;
	struct dentry *alias;
	struct dentry *lower_parent_dentry;
	struct nomount_dentry_info *info = rcu_dereference_raw(dentry->d_fsdata);
	
	struct path lower_path = info->lower_paths[0];
	struct dentry *lower_dentry = lower_path.dentry;

	lower_parent_dentry = lock_parent(lower_dentry);

	err = vfs_symlink(d_inode(lower_parent_dentry), lower_dentry, symname);
	if (err) goto out;

	alias = __nomount_interpose(dentry, dir->i_sb, &lower_path);
	if (IS_ERR(alias)) {
		err = PTR_ERR(alias);
		goto out;
	} else if (alias && alias != dentry) {
		dput(alias);
	}
	err = 0;

	fsstack_copy_attr_times(dir, nomount_lower_inode(dir));
	fsstack_copy_inode_size(dir, d_inode(lower_parent_dentry));

out:
	unlock_dir(lower_parent_dentry);
	return err;
}

#ifdef RENAME_HAS_FLAGS
static int nomount_rename(struct inode *old_dir, struct dentry *old_dentry,
			  struct inode *new_dir, struct dentry *new_dentry,
			  unsigned int flags)
#else
static int nomount_rename(struct inode *old_dir, struct dentry *old_dentry,
			  struct inode *new_dir, struct dentry *new_dentry)
#endif
{
	int err = 0;
	struct dentry *lower_old_dir_dentry, *lower_new_dir_dentry;
	struct dentry *trap;

	struct nomount_dentry_info *old_info = rcu_dereference_raw(old_dentry->d_fsdata);
	struct nomount_dentry_info *new_info = rcu_dereference_raw(new_dentry->d_fsdata);

	struct dentry *lower_old_dentry = old_info->lower_paths[0].dentry;
	struct dentry *lower_new_dentry = new_info->lower_paths[0].dentry;

#ifdef RENAME_HAS_FLAGS
	/* * Allow RENAME_NOREPLACE. This is crucial for Android SQLite 
	 * databases (like WhatsApp or Contacts) to ensure atomic writes.
	 * We only reject exotic flags like RENAME_EXCHANGE which
	 * require complex VFS swapping logic not needed here.
	 */
	if (flags & ~(RENAME_NOREPLACE)) 
		return -EINVAL; 
#endif

	lower_old_dir_dentry = dget_parent(lower_old_dentry);
	lower_new_dir_dentry = dget_parent(lower_new_dentry);

	/* Strict locking to prevent deadlocks when moving folders */
	trap = lock_rename(lower_old_dir_dentry, lower_new_dir_dentry);
	if (trap == lower_old_dentry) { err = -EINVAL; goto out; }
	if (trap == lower_new_dentry) { err = -ENOTEMPTY; goto out; }

#ifdef RENAME_HAS_FLAGS
	/* Pass the flags down to the physical filesystem layer */
	err = vfs_rename(d_inode(lower_old_dir_dentry), lower_old_dentry,
			 d_inode(lower_new_dir_dentry), lower_new_dentry, NULL, flags);
#else
	err = vfs_rename(d_inode(lower_old_dir_dentry), lower_old_dentry,
			 d_inode(lower_new_dir_dentry), lower_new_dentry);
#endif

	if (err) goto out;

	fsstack_copy_attr_all(new_dir, d_inode(lower_new_dir_dentry));
	fsstack_copy_inode_size(new_dir, d_inode(lower_new_dir_dentry));
	if (new_dir != old_dir) {
		fsstack_copy_attr_all(old_dir, d_inode(lower_old_dir_dentry));
		fsstack_copy_inode_size(old_dir, d_inode(lower_old_dir_dentry));
	}

        /* * If the target dentry already existed (overwrite), sync its 
	 * metadata to prevent stale "ghost" dentries in the VFS cache. 
	 */
	if (d_inode(new_dentry) && d_inode(lower_new_dentry)) {
		fsstack_copy_attr_times(d_inode(new_dentry), d_inode(lower_new_dentry));
	}

out:
	unlock_rename(lower_old_dir_dentry, lower_new_dir_dentry);
	dput(lower_old_dir_dentry);
	dput(lower_new_dir_dentry);
	return err;
}

static int nomount_getattr(const struct path *path, struct kstat *stat,
			   u32 request_mask, unsigned int query_flags)
{
	struct nomount_dentry_info *info;
	struct path lower_path;
	int err = -ENOENT;

	/* We extract the active path */
	info = rcu_access_pointer(path->dentry->d_fsdata);
	
	if (likely(info && info->lower_paths[0].dentry)) {
		lower_path = info->lower_paths[0];
		err = vfs_getattr(&lower_path, stat, request_mask, query_flags);
		
		/*
		 * We MUST sync the stat->dev with our virtual superblock's s_dev.
		 * If super.c assigned an anonymous block (0:XX) or a real physical 
		 * block (259:1), getattr must mirror it perfectly. 
		 * Otherwise, userspace apps reading /proc/self/mountinfo will detect 
		 * a mismatch against stat().
		 */

		if (likely(!err)) {
			stat->dev = path->dentry->d_sb->s_dev;
		}
	} else {
		generic_fillattr(d_inode(path->dentry), stat);
		err = 0;
	}
	
	return err;
}

static int nomount_setattr(struct dentry *dentry, struct iattr *ia)
{
	int err;
	struct inode *inode = d_inode(dentry);
	struct inode *lower_inode = nomount_lower_inode(inode);
	struct nomount_dentry_info *info = rcu_dereference_raw(dentry->d_fsdata);
	struct dentry *lower_dentry = info->lower_paths[0].dentry;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
	err = setattr_prepare(&init_user_ns, dentry, ia);
#else
	err = setattr_prepare(dentry, ia);
#endif
	if (err) return err;

	inode_lock(lower_inode);

	if (ia->ia_valid & ATTR_SIZE) {
		err = inode_newsize_ok(inode, ia->ia_size);
		if (err) {
			inode_unlock(lower_inode);
			goto out;
		}
	}

	err = notify_change(lower_dentry, ia, NULL);
	inode_unlock(lower_inode);

	if (!err) {
		if (ia->ia_valid & ATTR_SIZE)
			truncate_setsize(inode, ia->ia_size);
		fsstack_copy_attr_all(inode, lower_inode);
	}

out:
	return err;
}

/* --- XATTR Handling --- */

static ssize_t nomount_getxattr(struct dentry *dentry, struct inode *inode,
				const char *name, void *buffer, size_t size)
{
	struct nomount_dentry_info *info;
	struct dentry *ld;
	ssize_t err = -EOPNOTSUPP;

	info = rcu_dereference_raw(dentry->d_fsdata);
	if (unlikely(!info || !info->lower_paths[0].dentry))
		return err;

	ld = info->lower_paths[0].dentry;

	if (likely(d_is_positive(ld))) {
		/* First check if the physical inode supports XATTRs using its native flag.
		 * Then delegate the public call directly to the physical system. */
		if (d_inode(ld)->i_opflags & IOP_XATTR) {
			err = vfs_getxattr(ld, name, buffer, size);
		}
	}

	return err;
}

static int nomount_setxattr(struct dentry *dentry, struct inode *inode, const char *name,
			    const void *value, size_t size, int flags)
{
	struct nomount_dentry_info *info;
	struct dentry *ld;
	int err = -EOPNOTSUPP;

	/* Extract raw pointer without atomic locks */
	info = rcu_dereference_raw(dentry->d_fsdata);
	if (unlikely(!info || !info->lower_paths[0].dentry))
		return err;

	ld = info->lower_paths[0].dentry;

	if (likely(d_inode(ld)->i_opflags & IOP_XATTR)) {
		err = vfs_setxattr(ld, name, value, size, flags);
		if (!err) 
			fsstack_copy_attr_all(inode, d_inode(ld));
	}
	return err;
}

static int nomount_removexattr(struct dentry *dentry, struct inode *inode, const char *name)
{
	struct nomount_dentry_info *info;
	struct dentry *ld;
	int err = -EOPNOTSUPP;

	/* Extract raw pointer without atomic locks */
	info = rcu_dereference_raw(dentry->d_fsdata);
	if (unlikely(!info || !info->lower_paths[0].dentry))
		return err;

	ld = info->lower_paths[0].dentry;

	if (likely(d_inode(ld)->i_opflags & IOP_XATTR)) {
		err = vfs_removexattr(ld, name);
		if (!err) 
			fsstack_copy_attr_all(inode, d_inode(ld));
	}
	return err;
}

static ssize_t nomount_listxattr(struct dentry *dentry, char *buffer, size_t buffer_size)
{
	struct nomount_dentry_info *info;
	struct dentry *ld;
	ssize_t err = -EOPNOTSUPP;

	info = rcu_dereference_raw(dentry->d_fsdata);
	if (unlikely(!info || !info->lower_paths[0].dentry))
		return err;

	ld = info->lower_paths[0].dentry;

	if (likely(d_is_positive(ld) && (d_inode(ld)->i_opflags & IOP_XATTR))) {
		err = vfs_listxattr(ld, buffer, buffer_size);
		if (!err)
			fsstack_copy_attr_atime(d_inode(dentry), d_inode(ld));
	}

	return err;
}

/* --- Handlers XATTRs --- */
static int nomount_xattr_get(const struct xattr_handler *handler,
			    struct dentry *dentry, struct inode *inode,
			    const char *name, void *buffer, size_t size, int flags)
{
	return nomount_getxattr(dentry, inode, name, buffer, size);
}

static int nomount_xattr_set(const struct xattr_handler *handler,
			    struct dentry *dentry, struct inode *inode,
			    const char *name, const void *value, size_t size,
			    int flags)
{
	if (value)
		return nomount_setxattr(dentry, inode, name, value, size, flags);

	BUG_ON(flags != XATTR_REPLACE);
	return nomount_removexattr(dentry, inode, name);
}

const struct xattr_handler nomount_xattr_handler = {
	.prefix = "",		/* Match any attribute string */
	.get = nomount_xattr_get,
	.set = nomount_xattr_set,
};

const struct xattr_handler *nomount_xattr_handlers[] = {
	&nomount_xattr_handler,
	NULL
};

/* --- Symlink Management --- */

#ifdef LEGACY_FOLLOW_LINK
static void *nomount_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	struct inode *lower_inode = nomount_lower_inode(d_inode(dentry));
	if (lower_inode->i_op->follow_link)
		return lower_inode->i_op->follow_link(dentry, nd);
	return ERR_PTR(-EINVAL);
}
#else
static const char *nomount_get_link(struct dentry *dentry,
				   struct inode *inode,
				   struct delayed_call *done)
{
	struct nomount_dentry_info *info;

	/* RCU-walk mode (dentry is NULL) - Delegate directly to lower inode */
	if (unlikely(!dentry)) {
		struct inode *lower_inode = nomount_lower_inode(inode);
		if (!lower_inode->i_op->get_link)
			return ERR_PTR(-ECHILD);
		return lower_inode->i_op->get_link(NULL, lower_inode, done);
	}

	/* Standard Ref-walk mode */
	info = rcu_dereference_raw(dentry->d_fsdata);
	if (unlikely(!info || !info->lower_paths[0].dentry))
		return ERR_PTR(-ECHILD);

	return vfs_get_link(info->lower_paths[0].dentry, done);
}
#endif

static int nomount_permission(struct inode *inode, int mask)
{
	/* * inode_permission() natively handles MAY_NOT_BLOCK (RCU mode) correctly
	 * AND invokes the SELinux MAC hooks (security_inode_permission).
	 * Calling generic_permission() manually bypasses SELinux and breaks
	 * compilation on 5.12+ kernels.
	 */
	return inode_permission(nomount_lower_inode(inode), mask);
}

/* --- Operation Vectors --- */

const struct inode_operations nomount_dir_iops = {
	.getattr        = nomount_getattr,
	.create		= nomount_create,
	.lookup		= nomount_lookup,
	.unlink		= nomount_unlink,
	.mkdir		= nomount_mkdir,
	.rmdir		= nomount_rmdir,
	.symlink        = nomount_symlink,
	.rename		= nomount_rename,
	.permission	= nomount_permission,
	.setattr        	= nomount_setattr,
	.listxattr	= nomount_listxattr,
};

const struct inode_operations nomount_main_iops = {
	.getattr        = nomount_getattr,
	.permission	= nomount_permission,
	.setattr        	= nomount_setattr,
	.listxattr	= nomount_listxattr,
};

const struct inode_operations nomount_symlink_iops = {
	.getattr        = nomount_getattr,
#ifdef LEGACY_FOLLOW_LINK
	.follow_link	= nomount_follow_link,
#else
	.get_link	= nomount_get_link,
#endif
	.permission	= nomount_permission,
	.setattr        = nomount_setattr,
	.listxattr	= nomount_listxattr,
};
