/*
 * NoMountFS: Path Resolution and Inode Management
 */
#include <linux/security.h>
#include "nomount.h"
#include "compat.h"

static struct kmem_cache *nomount_dentry_cachep;

/* * Inode Cache: Handles the mapping between virtual and real inodes.
 */
static int nomount_inode_test(struct inode *inode, void *candidate_lower_inode)
{
	struct inode *current_lower_inode = nomount_lower_inode(inode);
	return (current_lower_inode == (struct inode *)candidate_lower_inode);
}

static int nomount_inode_set(struct inode *inode, void *lower_inode)
{
	/* Initialization is done in nomount_iget */
	return 0;
}

/* * nomount_iget: Retrieves an existing virtual inode or creates a new one.
 */
struct inode *nomount_iget(struct super_block *sb, struct inode *lower_inode)
{
	struct inode *inode;

	if (!igrab(lower_inode))
		return ERR_PTR(-ESTALE);

	/* Use hash lookup to find if we already have a virtual inode for this real one */
	inode = iget5_locked(sb, lower_inode->i_ino,
			     nomount_inode_test, nomount_inode_set, lower_inode);

	if (!inode) {
		iput(lower_inode);
		return ERR_PTR(-ENOMEM);
	}

	/* If it's not a new inode, we already found it in cache */
	if (!(inode->i_state & I_NEW)) {
		iput(lower_inode);
		return inode;
	}

	/* Setup the new inode */
	inode->i_ino = lower_inode->i_ino;
	nomount_set_lower_inode(inode, lower_inode);
	nomount_inc_iversion(inode);

	/* Inherit operations based on type */
	if (S_ISDIR(lower_inode->i_mode)) {
		inode->i_op = &nomount_dir_iops;
		inode->i_fop = &nomount_dir_fops;
	} else if (S_ISLNK(lower_inode->i_mode)) {
		inode->i_op = &nomount_symlink_iops;
		inode->i_fop = &nomount_main_fops;
	} else {
		inode->i_op = &nomount_main_iops;
		inode->i_fop = &nomount_main_fops;
	}

	inode->i_mapping->a_ops = &nomount_aops;

	/* Properly initialize special devices (char, block, fifo) */
	if (S_ISBLK(lower_inode->i_mode) || S_ISCHR(lower_inode->i_mode) ||
	    S_ISFIFO(lower_inode->i_mode) || S_ISSOCK(lower_inode->i_mode))
		init_special_inode(inode, lower_inode->i_mode, lower_inode->i_rdev);

	/* Sync metadata */
	fsstack_copy_attr_all(inode, lower_inode);
	fsstack_copy_inode_size(inode, lower_inode);

	/* Copy SELinux label from lower inode.
	 * Works for all inodes accessed after fill_super completes
	 * (SBLABEL_MNT set). Root inode is relabeled separately in
	 * fill_super after security_sb_set_mnt_opts.
	 */
	{
		char *ctx = NULL;
		unsigned int ctxlen = 0;
		if (security_inode_getsecctx(lower_inode, (void **)&ctx, &ctxlen) == 0 && ctx) {
			security_inode_notifysecctx(inode, ctx, ctxlen);
			security_release_secctx(ctx, ctxlen);
		}
	}

	unlock_new_inode(inode);
	return inode;
}

/* * __nomount_interpose: Links a virtual dentry with its inode.
 */
struct dentry *__nomount_interpose(struct dentry *dentry,
					 struct super_block *sb,
					 struct path *lower_path)
{
	struct inode *inode;
	struct inode *lower_inode = d_inode(lower_path->dentry);

	inode = nomount_iget(sb, lower_inode);
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	/* Use d_splice_alias to handle existing dentries safely */
	return d_splice_alias(inode, dentry);
}

/* * nomount_lookup: The main entry point for path resolution.
 */
struct dentry *nomount_lookup(struct inode *dir, struct dentry *dentry,
			     unsigned int flags)
{
	struct dentry *ret, *parent, *lower_dentry;
	struct path lower_parent_paths[NOMOUNT_MAX_BRANCHES];
	struct path found_paths[NOMOUNT_MAX_BRANCHES];
	int num_lower_parent_paths;
	int num_found_paths = 0;
	struct nomount_sb_info *sbi;
	struct qstr name;
	int err;
	int i;
	bool is_dir = false;

	parent = dget_parent(dentry);
	num_lower_parent_paths = nomount_get_all_lower_paths(parent, lower_parent_paths);

	/* Allocate private data for the dentry info */
	err = new_dentry_private_data(dentry);
	if (err) {
		ret = ERR_PTR(err);
		goto out_put_parent;
	}

	name = dentry->d_name;
	/* Get the superblock info from the dentry's superblock, not allocate new */
	sbi = NOMOUNT_SB(dentry->d_sb);

	if (sbi && sbi->has_inject && strcmp(name.name, sbi->inject_name) == 0) {
		pathcpy(&found_paths[0], &sbi->inject_path);
		path_get(&found_paths[0]);
		lower_dentry = found_paths[0].dentry;
		num_found_paths = 1;
	} else {
		struct path first_negative_path = { .dentry = NULL, .mnt = NULL };

		for (i = 0; i < num_lower_parent_paths; i++) {
			struct path lower_path;

			inode_lock_nested(d_inode(lower_parent_paths[i].dentry), I_MUTEX_PARENT);
			lower_dentry = lookup_one_len(name.name, lower_parent_paths[i].dentry, name.len);
			inode_unlock(d_inode(lower_parent_paths[i].dentry));

			if (IS_ERR(lower_dentry)) {
				continue;
			}

			/*
			 * If the child dentry is itself a mount point, follow_down
			 * traverses into the mounted filesystem. Without this, a
			 * union mount where lowerdir == mount point would recurse
			 * back into nomount_lookup infinitely, crashing the kernel.
			 *
			 * We build a full path so follow_down can update both the
			 * dentry and the vfsmount atomically.
			 */
			lower_path.dentry = lower_dentry;
			lower_path.mnt = mntget(lower_parent_paths[i].mnt);
			follow_down(&lower_path);
			lower_dentry = lower_path.dentry;
			/* lower_path.mnt may have changed — use it for found_paths */

			if (d_really_is_positive(lower_dentry)) {
				found_paths[num_found_paths].dentry = lower_dentry;
				found_paths[num_found_paths].mnt = lower_path.mnt;
				num_found_paths++;

				if (!S_ISDIR(d_inode(lower_dentry)->i_mode)) {
					is_dir = false;
					break; /* Shadowing: top file hides lower files/dirs */
				} else {
					is_dir = true;
				}
			} else {
				/* It's negative. Save the first one (from layer 0) for creations */
				if (!first_negative_path.dentry) {
					first_negative_path.dentry = lower_dentry;
					first_negative_path.mnt = lower_path.mnt;
				} else {
					path_put(&lower_path);
				}
			}
		}

		/* If we found nothing positive, but we found a negative dentry, use it */
		if (num_found_paths == 0 && first_negative_path.dentry) {
			found_paths[0] = first_negative_path;
			num_found_paths = 1;
		} else if (first_negative_path.dentry) {
			/* Found positive later, release the cached negative dentry */
			path_put(&first_negative_path);
		}
	}

	if (num_found_paths == 0) {
		/* Negative dentry: file doesn't exist AND no negative dentry available (rare error). */
		nomount_set_lower_paths(dentry, found_paths, 0); 
		d_add(dentry, NULL);
		ret = NULL;
	} else if (d_really_is_negative(found_paths[0].dentry)) {
		/* Proper negative dentry cached, ready for creation */
		nomount_set_lower_paths(dentry, found_paths, 1);
		d_add(dentry, NULL);
		ret = NULL;
	} else {
		nomount_set_lower_paths(dentry, found_paths, num_found_paths);
		/* Positive dentry: Interpose virtual dentry with real inode (from topmost branch) */
		ret = __nomount_interpose(dentry, dentry->d_sb, &found_paths[0]);
		if (IS_ERR(ret)) {
			/*
			 * Interpose failed. free_dentry_private_data releases all paths
			 * that were stored via nomount_set_lower_paths — do NOT put them
			 * again. Jump directly to parent cleanup.
			 */
			free_dentry_private_data(dentry);
			goto out_put_parent;
		}
	}

	/* Final metadata sync for the parent and current dentry */
	if (!IS_ERR_OR_NULL(ret))
		dentry = ret;

	if (d_inode(dentry))
		fsstack_copy_attr_times(d_inode(dentry), nomount_lower_inode(d_inode(dentry)));

	fsstack_copy_attr_atime(d_inode(parent), nomount_lower_inode(d_inode(parent)));

out_put_parent:
	nomount_put_all_lower_paths(parent, lower_parent_paths, num_lower_parent_paths);
	dput(parent);
	return ret;
}

/* --- Dentry Cache Initialization --- */

int nomount_init_dentry_cache(void)
{
	nomount_dentry_cachep = kmem_cache_create("nomount_dentry_cache",
					sizeof(struct nomount_dentry_info),
					0, SLAB_RECLAIM_ACCOUNT, NULL);
	return nomount_dentry_cachep ? 0 : -ENOMEM;
}

void nomount_destroy_dentry_cache(void)
{
	if (nomount_dentry_cachep)
		kmem_cache_destroy(nomount_dentry_cachep);
}
