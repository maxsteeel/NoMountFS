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
	struct dentry *lower_dentry;
	struct dentry *lower_parent_dentry = NULL;
	struct path lower_path;

	nomount_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_parent_dentry = lock_parent(lower_dentry);

	err = vfs_create(d_inode(lower_parent_dentry), lower_dentry, mode, want_excl);
	if (err) goto out;

	/* Interpose the new dentry - check for errors properly */
	lower_dentry = __nomount_interpose(dentry, dir->i_sb, &lower_path);
	if (IS_ERR(lower_dentry)) {
		err = PTR_ERR(lower_dentry);
		goto out;
	}
	err = 0;

	fsstack_copy_attr_times(dir, nomount_lower_inode(dir));
	fsstack_copy_inode_size(dir, d_inode(lower_parent_dentry));

out:
	unlock_dir(lower_parent_dentry);
	nomount_put_lower_path(dentry, &lower_path);
	return err;
}

static int nomount_unlink(struct inode *dir, struct dentry *dentry)
{
	int err;
	struct dentry *lower_dentry;
	struct inode *lower_dir_inode = nomount_lower_inode(dir);
	struct dentry *lower_dir_dentry;
	struct path lower_path;

	nomount_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	dget(lower_dentry);
	lower_dir_dentry = lock_parent(lower_dentry);

	err = vfs_unlink(lower_dir_inode, lower_dentry, NULL);
	if (err) goto out;

	fsstack_copy_attr_times(dir, lower_dir_inode);
	fsstack_copy_inode_size(dir, lower_dir_inode);
	set_nlink(d_inode(dentry), nomount_lower_inode(d_inode(dentry))->i_nlink);
	d_drop(dentry);
out:
	unlock_dir(lower_dir_dentry);
	dput(lower_dentry);
	nomount_put_lower_path(dentry, &lower_path);
	return err;
}

static int nomount_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	int err;
	struct dentry *lower_dentry;
	struct dentry *lower_parent_dentry = NULL;
	struct path lower_path;

	nomount_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_parent_dentry = lock_parent(lower_dentry);

	err = vfs_mkdir(d_inode(lower_parent_dentry), lower_dentry, mode);
	if (err) goto out;

	/* Interpose the new dentry - check for errors properly */
	lower_dentry = __nomount_interpose(dentry, dir->i_sb, &lower_path);
	if (IS_ERR(lower_dentry)) {
		err = PTR_ERR(lower_dentry);
		goto out;
	}
	err = 0;

	fsstack_copy_attr_times(dir, nomount_lower_inode(dir));
	fsstack_copy_inode_size(dir, d_inode(lower_parent_dentry));
	set_nlink(dir, nomount_lower_inode(dir)->i_nlink);

out:
	unlock_dir(lower_parent_dentry);
	nomount_put_lower_path(dentry, &lower_path);
	return err;
}

static int nomount_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct dentry *lower_dentry;
	struct dentry *lower_dir_dentry;
	int err;
	struct path lower_path;

	nomount_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_dir_dentry = lock_parent(lower_dentry);

	err = vfs_rmdir(d_inode(lower_dir_dentry), lower_dentry);
	if (err) goto out;

	d_drop(dentry);
	if (d_inode(dentry)) clear_nlink(d_inode(dentry));
	
	fsstack_copy_attr_times(dir, d_inode(lower_dir_dentry));
	fsstack_copy_inode_size(dir, d_inode(lower_dir_dentry));
	set_nlink(dir, d_inode(lower_dir_dentry)->i_nlink);

out:
	unlock_dir(lower_dir_dentry);
	nomount_put_lower_path(dentry, &lower_path);
	return err;
}

static int nomount_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	int err;
	struct dentry *lower_dentry;
	struct dentry *lower_parent_dentry = NULL;
	struct path lower_path;

	nomount_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_parent_dentry = lock_parent(lower_dentry);

	err = vfs_symlink(d_inode(lower_parent_dentry), lower_dentry, symname);
	if (err) goto out;

	/* Interpose the new dentry - check for errors properly */
	lower_dentry = __nomount_interpose(dentry, dir->i_sb, &lower_path);
	if (IS_ERR(lower_dentry)) {
		err = PTR_ERR(lower_dentry);
		goto out;
	}
	err = 0;

	fsstack_copy_attr_times(dir, nomount_lower_inode(dir));
	fsstack_copy_inode_size(dir, d_inode(lower_parent_dentry));

out:
	unlock_dir(lower_parent_dentry);
	nomount_put_lower_path(dentry, &lower_path);
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
	struct dentry *lower_old_dentry, *lower_new_dentry;
	struct dentry *lower_old_dir_dentry, *lower_new_dir_dentry;
	struct dentry *trap;
	struct path lower_old_path, lower_new_path;

#ifdef RENAME_HAS_FLAGS
	if (flags) return -EINVAL; /* No support for advanced flags like RENAME_EXCHANGE yet */
#endif

	nomount_get_lower_path(old_dentry, &lower_old_path);
	nomount_get_lower_path(new_dentry, &lower_new_path);
	lower_old_dentry = lower_old_path.dentry;
	lower_new_dentry = lower_new_path.dentry;
	lower_old_dir_dentry = dget_parent(lower_old_dentry);
	lower_new_dir_dentry = dget_parent(lower_new_dentry);

	/* Strict locking to prevent deadlocks when moving folders */
	trap = lock_rename(lower_old_dir_dentry, lower_new_dir_dentry);
	if (trap == lower_old_dentry) { err = -EINVAL; goto out; }
	if (trap == lower_new_dentry) { err = -ENOTEMPTY; goto out; }

#ifdef RENAME_HAS_FLAGS
	err = vfs_rename(d_inode(lower_old_dir_dentry), lower_old_dentry,
			 d_inode(lower_new_dir_dentry), lower_new_dentry, NULL, 0);
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

out:
	unlock_rename(lower_old_dir_dentry, lower_new_dir_dentry);
	dput(lower_old_dir_dentry);
	dput(lower_new_dir_dentry);
	nomount_put_lower_path(old_dentry, &lower_old_path);
	nomount_put_lower_path(new_dentry, &lower_new_path);
	return err;
}

static int nomount_setattr(struct dentry *dentry, struct iattr *ia)
{
	int err;
	struct dentry *lower_dentry;
	struct path lower_path;
	struct inode *inode = d_inode(dentry);
	struct inode *lower_inode = nomount_lower_inode(inode);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
	err = setattr_prepare(&init_user_ns, dentry, ia);
#else
	err = setattr_prepare(dentry, ia);
#endif
	if (err) return err;

	nomount_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;

	inode_lock(lower_inode);

	if (ia->ia_valid & ATTR_SIZE) {
		err = inode_newsize_ok(inode, ia->ia_size);
		if (err) {
			inode_unlock(lower_inode);
			goto out;
		}
		/*
		 * Truncate the lower inode first (via notify_change), then
		 * sync the size up to the upper inode. Doing truncate_setsize
		 * on the upper before the lower creates a window where the VFS
		 * sees a larger size than what is actually on disk.
		 */
		ia->ia_valid |= ATTR_FILE;
	}

	err = notify_change(lower_dentry, ia, NULL);
	inode_unlock(lower_inode);

	if (!err) {
		if (ia->ia_valid & ATTR_SIZE)
			truncate_setsize(inode, ia->ia_size);
		fsstack_copy_attr_all(inode, lower_inode);
	}

out:
	nomount_put_lower_path(dentry, &lower_path);
	return err;
}

/* --- XATTR Handling --- */

static ssize_t nomount_getxattr(struct dentry *dentry, struct inode *inode,
			       const char *name, void *buffer, size_t size)
{
	struct path lower_paths[NOMOUNT_MAX_BRANCHES];
	int num_paths;
	ssize_t err = -EOPNOTSUPP;

	num_paths = nomount_get_all_lower_paths(dentry, lower_paths);
	if (num_paths == 0)
		return -EOPNOTSUPP;

	if (!strcmp(name, XATTR_NAME_SELINUX)) {
	    struct dentry *ld = lower_paths[num_paths - 1].dentry;
	    
	    if (d_is_positive(ld) && d_inode(ld)->i_sb->s_xattr) {
	        err = __vfs_getxattr(ld, d_inode(ld), name, buffer, size, 0);
	    } else {
	        err = -EOPNOTSUPP;
	    }
	} else {
		struct dentry *ld = lower_paths[0].dentry;
		if (!d_is_positive(ld) ||
		    !(d_inode(ld)->i_opflags & IOP_XATTR))
			err = -EOPNOTSUPP;
		else
			err = vfs_getxattr(ld, name, buffer, size);
	}

	nomount_put_all_lower_paths(dentry, lower_paths, num_paths);
	return err;
}

static int nomount_setxattr(struct dentry *dentry, struct inode *inode, const char *name,
			    const void *value, size_t size, int flags)
{
	int err;
	struct dentry *lower_dentry;
	struct path lower_path;

	nomount_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	if (!(d_inode(lower_dentry)->i_opflags & IOP_XATTR)) {
		err = -EOPNOTSUPP;
		goto out;
	}

	err = vfs_setxattr(lower_dentry, name, value, size, flags);
	if (!err) fsstack_copy_attr_all(d_inode(dentry), d_inode(lower_path.dentry));
out:
	nomount_put_lower_path(dentry, &lower_path);
	return err;
}

static int nomount_removexattr(struct dentry *dentry, struct inode *inode, const char *name)
{
	int err;
	struct dentry *lower_dentry;
	struct inode *lower_inode = nomount_lower_inode(inode);
	struct path lower_path;

	nomount_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	if (!(lower_inode->i_opflags & IOP_XATTR)) {
		err = -EOPNOTSUPP;
		goto out;
	}

	err = vfs_removexattr(lower_dentry, name);
	if (!err) fsstack_copy_attr_all(d_inode(dentry), lower_inode);
out:
	nomount_put_lower_path(dentry, &lower_path);
	return err;
}

static ssize_t nomount_listxattr(struct dentry *dentry, char *buffer, size_t buffer_size)
{
	int err;
	struct dentry *lower_dentry;
	struct path lower_path;

	nomount_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	if (!(d_inode(lower_dentry)->i_opflags & IOP_XATTR)) {
		err = -EOPNOTSUPP;
		goto out;
	}

	err = vfs_listxattr(lower_dentry, buffer, buffer_size);
	if (!err) fsstack_copy_attr_atime(d_inode(dentry), d_inode(lower_path.dentry));
out:
	nomount_put_lower_path(dentry, &lower_path);
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
	struct dentry *lower_dentry;
	struct path lower_path;
	const char *link;

	if (!dentry) return ERR_PTR(-ECHILD);

	nomount_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;

	link = vfs_get_link(lower_dentry, done);
	nomount_put_lower_path(dentry, &lower_path);
	return link;
}
#endif

static int nomount_permission(struct inode *inode, int mask)
{
	return inode_permission(nomount_lower_inode(inode), mask);
}

/* --- Operation Vectors --- */

const struct inode_operations nomount_dir_iops = {
	.create		= nomount_create,
	.lookup		= nomount_lookup,
	.unlink		= nomount_unlink,
	.mkdir		= nomount_mkdir,
	.rmdir		= nomount_rmdir,
	.symlink	= nomount_symlink,
	.rename		= nomount_rename,
	.permission	= nomount_permission,
	.setattr	= nomount_setattr,
	.listxattr	= nomount_listxattr,
};

const struct inode_operations nomount_main_iops = {
	.permission	= nomount_permission,
	.setattr	= nomount_setattr,
	.listxattr	= nomount_listxattr,
};

const struct inode_operations nomount_symlink_iops = {
#ifdef LEGACY_FOLLOW_LINK
	.follow_link	= nomount_follow_link,
#else
	.get_link	= nomount_get_link,
#endif
	.permission	= nomount_permission,
	.setattr	= nomount_setattr,
	.listxattr	= nomount_listxattr,
};
