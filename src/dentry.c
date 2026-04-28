/*
 * NoMountFS: Dentry operations
 * Managing path name cache and RCU-walk safety.
 */

#include "nomount.h"
#include "compat.h"

/* nomount_d_revalidate: checks if our cached dentry is still valid 
 * compared to the real one on disk.
 */
static int nomount_d_revalidate(struct dentry *dentry, unsigned int flags)
{
	struct dentry *lower_dentry;
	struct nomount_dentry_info *info;
	int valid = 1;

	/* * RCU-safe extraction without redundant locks. The VFS guarantees
	 * either refcount protection or global rcu_read_lock() here.
	 */
	info = rcu_dereference_raw(dentry->d_fsdata);
	if (unlikely(!info || !info->lower_paths[0].dentry)) {
		return -ECHILD;
	}
	lower_dentry = info->lower_paths[0].dentry;

	/* If the lower filesystem dentry has its own revalidate, call it */
	if (lower_dentry->d_op && lower_dentry->d_op->d_revalidate) {
		valid = lower_dentry->d_op->d_revalidate(lower_dentry, flags);
		if (unlikely(valid <= 0))
			return valid;
	}

	/* * Use d_inode_rcu() to prevent NULL dereference 
	 *  Kernel Panics during lockless RCU-walks where inodes can be 
	 *  deleted concurrently by other threads.
	 */
	if (likely(d_really_is_positive(dentry))) {
		struct inode *inode = d_inode_rcu(dentry);
		struct inode *lower_inode = d_inode_rcu(lower_dentry);

		if (unlikely(!inode || !lower_inode || inode->i_ino != lower_inode->i_ino))
			valid = 0;
	}

	return valid;
}

/* nomount_d_release: cleanup when a dentry is destroyed.
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
	struct nomount_dentry_info *info;

	if (likely(inode && d_inode(dentry) == inode))
		return dentry;

	info = rcu_access_pointer(dentry->d_fsdata);
	if (unlikely(!info || !info->lower_paths[0].dentry))
		return dentry;

	lower_dentry = info->lower_paths[0].dentry;

	if (lower_dentry->d_op && lower_dentry->d_op->d_real)
		return lower_dentry->d_op->d_real(lower_dentry, inode);

	return lower_dentry;
}

/* nomount_d_delete: Decides if a dentry should be cached when its refcount hits 0.
 */
static int nomount_d_delete(const struct dentry *dentry)
{
	struct nomount_dentry_info *info;
	struct dentry *lower_dentry;
	int err = 0;

	/*
	 * Use rcu_access_pointer() to safely check d_fsdata.
	 * This is RCU-safe without needing rcu_read_lock() because
	 * we're only checking for NULL, not dereferencing.
	 */
	info = rcu_access_pointer(dentry->d_fsdata);
	if (unlikely(!info))
		return 1;

	/* Optimize for topmost branch (layer 0) without allocating arrays */
	lower_dentry = info->lower_paths[0].dentry;

	if (likely(lower_dentry)) {
		/* If the original dentry is no longer cached (was deleted/moved),
		 * we must also disappear from the cache.
		 */
		if (unlikely(d_unhashed(lower_dentry)))
			err = 1;
		else if (lower_dentry->d_op && lower_dentry->d_op->d_delete)
			err = lower_dentry->d_op->d_delete(lower_dentry);
	} else {
		/* Corrupt or incomplete dentry info, force deletion */
		err = 1;
	}

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

	info = kmem_cache_zalloc(nomount_dentry_cachep, GFP_KERNEL);
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

#ifdef NOMOUNT_RCU_PATH_ACCESS
static void nomount_dentry_rcu_free(struct rcu_head *head)
{
	struct nomount_dentry_info *info = container_of(head, struct nomount_dentry_info, rcu);
	kmem_cache_free(nomount_dentry_cachep, info);
}
#endif

void free_dentry_private_data(struct dentry *dentry)
{
    struct nomount_dentry_info *info = NOMOUNT_D(dentry);
	int i;

    if (likely(info)) {
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

#ifdef NOMOUNT_RCU_PATH_ACCESS
		/*
		 * Use kfree_rcu() to defer freeing until all RCU readers complete.
		 * The grace period ensures readers who already fetched the pointer
		 * can finish before the memory is freed.
		 */
		call_rcu(&info->rcu, nomount_dentry_rcu_free);
#else
		kmem_cache_free(nomount_dentry_cachep, info);
#endif
    }
}
