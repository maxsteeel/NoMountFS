/*
 * NoMountFS: Path Resolution and Inode Management
 */

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
	struct super_block *lower_sb = NOMOUNT_SB(sb)->lower_sb;

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
	struct path lower_parent_path, lower_path;
	struct nomount_sb_info *sbi;
	struct qstr name;
	int err;

	parent = dget_parent(dentry);
	nomount_get_lower_path(parent, &lower_parent_path);

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
		pathcpy(&lower_path, &sbi->inject_path);
		path_get(&lower_path);
		lower_dentry = lower_path.dentry;
	} else {
		lower_dentry = lookup_one_len(name.name, lower_parent_path.dentry, name.len);
		if (IS_ERR(lower_dentry)) {
			ret = ERR_CAST(lower_dentry);
			goto out_free_dentry;
		}
		lower_path.dentry = lower_dentry;
		lower_path.mnt = mntget(lower_parent_path.mnt);
	}

	nomount_set_lower_path(dentry, &lower_path);

	if (d_really_is_negative(lower_dentry)) {
		/* Negative dentry: file doesn't exist. Map to NULL */
		d_add(dentry, NULL);
		ret = NULL;
	} else {
		/* Positive dentry: Interpose virtual dentry with real inode */
		ret = __nomount_interpose(dentry, dentry->d_sb, &lower_path);
		if (IS_ERR(ret)) {
			/* Interpose failed: release private data to avoid a leak */
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

out_free_dentry:
	/* If the lookup fail, release the private_data */
	if (IS_ERR(lower_dentry))
		free_dentry_private_data(dentry);

out_put_parent:
	nomount_put_lower_path(parent, &lower_parent_path);
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
