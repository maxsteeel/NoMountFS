/*
 *NoMountFS: Dentry operations
 *Managing path name cache.
 */

#include "nomount.h"
#include "compat.h"

/* *nomount_d_revalidate: checks if our cached dentry is still valid 
 *compared to the real one on disk.
 */
static int nomount_d_revalidate(struct dentry *dentry, unsigned int flags)
{
	struct path lower_path;
	struct dentry *lower_dentry;
	int valid = 1;

	if (flags & LOOKUP_RCU)
		return -ECHILD;

	nomount_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;

	/* If the lower filesystem dentry has its own revalidate, call it */
	if (lower_dentry->d_op && lower_dentry->d_op->d_revalidate)
		valid = lower_dentry->d_op->d_revalidate(lower_dentry, flags);

	/* Check if the lower inode has changed unexpectedly */
	if (d_really_is_positive(dentry) && d_really_is_positive(lower_dentry)) {
		if (d_inode(dentry)->i_ino != d_inode(lower_dentry)->i_ino)
			valid = 0;
	}

	nomount_put_lower_path(dentry, &lower_path);
	return valid;
}

/* *nomount_d_release: cleanup when a dentry is destroyed.
 */
static void nomount_d_release(struct dentry *dentry)
{
	/* Free the private data and release the lower path reference */
	free_dentry_private_data(dentry);
}

static struct dentry *nomount_d_real(struct dentry *dentry,
				     const struct inode *inode)
{
	struct dentry *lower_dentry;
	struct path lower_path;
	struct dentry *real_dentry;

	if (inode && d_inode(dentry) == inode)
		return dentry;

	nomount_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;

	if (lower_dentry && lower_dentry->d_op && lower_dentry->d_op->d_real) {
		real_dentry = lower_dentry->d_op->d_real(lower_dentry, inode);
	} else {
		real_dentry = lower_dentry;
	}

	nomount_put_lower_path(dentry, &lower_path);
	return real_dentry;
}

/* *nomount_d_delete: Decides if a dentry should be cached when its refcount hits 0.
 */
static int nomount_d_delete(const struct dentry *dentry)
{
	struct path lower_paths[NOMOUNT_MAX_BRANCHES];
	int num_lower_paths;
	struct dentry *lower_dentry;
	int err = 0;

	/*
	 * Use rcu_access_pointer() to safely check d_fsdata.
	 * This is RCU-safe without needing rcu_read_lock() because
	 * we're only checking for NULL, not dereferencing.
	 */
	if (!rcu_access_pointer(dentry->d_fsdata))
		return 1;

	num_lower_paths = nomount_get_all_lower_paths(dentry, lower_paths);
	
	if (num_lower_paths > 0) {
		lower_dentry = lower_paths[0].dentry; /* Topmost logic */

		if (lower_dentry) {
			/* If the original dentry is no longer cached (was deleted/moved),
			 * we must also disappear from the cache.
			 */
			if (d_unhashed(lower_dentry))
				err = 1;

			else if (lower_dentry->d_op && lower_dentry->d_op->d_delete)
				err = lower_dentry->d_op->d_delete(lower_dentry);
		}
	}

	nomount_put_all_lower_paths(dentry, lower_paths, num_lower_paths);
	return err;
}

/* Dentry operations vector */
const struct dentry_operations nomount_dops = {
	.d_revalidate	= nomount_d_revalidate,
	.d_release		= nomount_d_release,
	.d_real			= nomount_d_real,
	.d_delete	    = nomount_d_delete, 
};

/* Helper to allocate private dentry data */
int new_dentry_private_data(struct dentry *dentry)
{
	struct nomount_dentry_info *info;

	info = kzalloc(sizeof(struct nomount_dentry_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	spin_lock_init(&info->lock);
	/*
	 * Use rcu_assign_pointer() for consistency, though at init time
	 * the dentry isn't yet visible to other CPUs so this is just
	 * documenting the RCU annotation requirement.
	 */
	rcu_assign_pointer(dentry->d_fsdata, info);
	return 0;
}

void free_dentry_private_data(struct dentry *dentry)
{
    struct nomount_dentry_info *info = NOMOUNT_D(dentry);
	int i;

    if (info) {
		for (i = 0; i < info->num_lower_paths; i++) {
			if (info->lower_paths[i].dentry) {
				path_put(&info->lower_paths[i]);
				info->lower_paths[i].dentry = NULL;
			}
		}
		/*
		 * Clear d_fsdata under dentry->d_lock to prevent race with
		 * concurrent RCU readers. This matches the 9p fix pattern.
		 */
		spin_lock(&dentry->d_lock);
		rcu_assign_pointer(dentry->d_fsdata, NULL);
		spin_unlock(&dentry->d_lock);

#ifdef CONFIG_NOMOUNT_RCU_PATH_ACCESS
		/*
		 * Use kfree_rcu() to defer freeing until all RCU readers complete.
		 * The grace period ensures readers who already fetched the pointer
		 * can finish before the memory is freed.
		 */
		kfree_rcu(info, rcu);
#else
		kfree(info);
#endif
    }
}
