/*
 * NoMountFS: Superblock operations and lifecycle management.
 */

#include "nomount.h"
#include "compat.h"

static struct kmem_cache *nomount_inode_cachep;

/* * nomount_evict_inode: Called when an inode is being removed from memory.
 */
static void nomount_evict_inode(struct inode *inode)
{
	struct inode *lower_inode;

	truncate_inode_pages(&inode->i_data, 0);
	clear_inode(inode);

	/* Get the real inode and release it */
	lower_inode = nomount_lower_inode(inode);
	nomount_set_lower_inode(inode, NULL);
	iput(lower_inode);
}

/* * nomount_alloc_inode: Efficient allocation using our custom slab cache.
 */
static struct inode *nomount_alloc_inode(struct super_block *sb)
{
	struct nomount_inode_info *i;

	i = kmem_cache_alloc(nomount_inode_cachep, GFP_KERNEL);
	if (!i)
		return NULL;

	/* Initialize private data and VFS inode */
	memset(i, 0, offsetof(struct nomount_inode_info, vfs_inode));
	nomount_set_iversion(&i->vfs_inode, 1);

	return &i->vfs_inode;
}

static void nomount_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);
	kmem_cache_free(nomount_inode_cachep, NOMOUNT_I(inode));
}

static void nomount_destroy_inode(struct inode *inode)
{
	/* Use RCU to ensure no one is looking at the inode before freeing */
	call_rcu(&inode->i_rcu, nomount_i_callback);
}

/* * nomount_put_super: Final cleanup during unmount.
 */
void nomount_put_super(struct super_block *sb)
{
	struct nomount_sb_info *sbi = NOMOUNT_SB(sb);

	if (!sbi)
		return;

	if (sbi->has_inject) {
		path_put(&sbi->inject_path);
	}

	kfree(sbi);
	sb->s_fs_info = NULL;
}

int nomount_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct kstatfs lower_buf;
	struct path lower_paths[NOMOUNT_MAX_BRANCHES];
	int num_paths;
	int err;

	num_paths = nomount_get_all_lower_paths(dentry, lower_paths);
	if (num_paths == 0) return -ENOENT;

	/* Get the statfs of the lowest layer to show the physical system disk's free space */
	err = vfs_statfs(&lower_paths[num_paths - 1], &lower_buf);
	
	nomount_put_all_lower_paths(dentry, lower_paths, num_paths);
	if (err) return err;

	*buf = lower_buf;

	if (lower_buf.f_type != 0) {
		/* If the one below has a valid type, we use it to hide */
		buf->f_type = lower_buf.f_type;
	} else {
		/* Fallback: If for some reason there is no type, we use our own */
		buf->f_type = NOMOUNT_FS_MAGIC;
	}  

	return 0;
}

/* * nomount_show_options: Displays mount options in /proc/mounts.
 * Required for the system to identify the mount source.
 */
static int nomount_show_options(struct seq_file *m, struct dentry *root)
{
	struct nomount_sb_info *sbi;
	int i;

	if (!root) return 0;
	sbi = NOMOUNT_SB(root->d_sb);
	if (!sbi) return 0;

	if (sbi->num_lower_paths > 0) {
		/* Topmost layer could represent upperdir or first lowerdir */
		seq_show_option(m, "upperdir", sbi->lower_paths[0].dentry->d_name.name);
		
		if (sbi->num_lower_paths > 1) {
			seq_puts(m, ",lowerdir=");
			for (i = 1; i < sbi->num_lower_paths; i++) {
				if (i > 1) seq_puts(m, ":");
				/* Ideally we would construct full absolute paths here using d_path,
				   but a simple name matches the typical minimal fallback format */
				seq_escape(m, sbi->lower_paths[i].dentry->d_name.name, ", \t\n\\");
			}
		}
	}

	/* Also emit injection parameters so /proc/mounts is complete */
	if (sbi->has_inject) {
		seq_show_option(m, "inject_name", sbi->inject_name);
		if (sbi->inject_path.dentry)
			seq_show_option(m, "inject_path",
					sbi->inject_path.dentry->d_name.name);
	}

	return 0;
}

/* Operation vector */
const struct super_operations nomount_sops = {
	.alloc_inode	= nomount_alloc_inode,
	.destroy_inode	= nomount_destroy_inode,
	.evict_inode	= nomount_evict_inode,
	.drop_inode	= generic_delete_inode,
	.put_super	= nomount_put_super,
	.statfs		= nomount_statfs,
	.show_options = nomount_show_options,
};

int nomount_fill_super(struct super_block *sb, void *raw_data, int silent)
{
	struct dentry *root;
	struct inode *root_inode;
	struct nomount_sb_info *sbi;
	
	/* 1. Unpack the assembly mount_data */
	struct nomount_mount_data *mdata = (struct nomount_mount_data *)raw_data;
	const char *dev_name = mdata ? mdata->dev_name : NULL;
	char *opts = mdata ? (char *)mdata->raw_data : NULL;
	
	char *path_to_mount = NULL;
	char *upperdir_str = NULL;
	char *inject_name_str = NULL;
	char *inject_path_str = NULL;
	char *target_str = NULL;
	char *source_str = NULL;
	char *p;
	char *branch_ptr;
	char *branch_str;
	int err = 0;
	int i;

	/* 2. Try to extract the path from the options (e.g. -o lowerdir=/path/example) */
	if (opts) {
		while ((p = strsep(&opts, ",")) != NULL) {
			if (!*p) continue;
			if (!strncmp(p, "lowerdir=", 9)) {
				path_to_mount = p + 9; /* The word "lowerdir=" is skipped */
			} else if (!strncmp(p, "upperdir=", 9)) {
				upperdir_str = p + 9; 
			} else if (!strncmp(p, "inject_name=", 12)) {
				inject_name_str = p + 12;
			} else if (!strncmp(p, "inject_path=", 12)) {
				inject_path_str = p + 12;
			} else if (!strncmp(p, "target=", 7)) {
				target_str = p + 7;
			} else if (!strncmp(p, "source=", 7)) {
				source_str = p + 7;
			}
		}
	}

	/* 3. For direct file injection, use source= and target= options */
	if (source_str && target_str) {
		path_to_mount = source_str;
	}

	/* 4. Fallback: If there is no 'lowerdir' or 'source', use the original dev_name */
	if (!path_to_mount && dev_name && *dev_name) {
		/* Ignore generic words that are often used as filler */
		if (strcmp(dev_name, "none") != 0 && strcmp(dev_name, "nomountfs") != 0) {
			path_to_mount = (char *)dev_name;
		}
	}

	/* 5. Abort cleanly if there is nothing to mount */
	if (!path_to_mount || !*path_to_mount) {
		pr_err("NoMountFS: Missing source path.\n");
		return -EINVAL;
	}

	/* Update superblock ID so /proc/mounts displays it pretty */
	strlcpy(sb->s_id, path_to_mount, sizeof(sb->s_id));

	sbi = kzalloc(sizeof(struct nomount_sb_info), GFP_KERNEL);
	if (!sbi) return -ENOMEM;
	sb->s_fs_info = sbi;

	/* 5. Handle direct file injection with target= option
	 * Syntax: mount -t nomountfs none <mount_dir> -o source=<src>,target=<file_to_shadow>
	 * The lower_path must be the PARENT DIRECTORY of the target file, not the
	 * target file itself — the filesystem root must always be a directory inode.
	 */
	if (target_str && *target_str) {
		struct path target_path;
		struct dentry *parent_dentry;

		/* Resolve the target file path */
		err = kern_path(target_str, LOOKUP_FOLLOW, &target_path);
		if (err) {
			pr_err("NoMountFS: Could not find target file: %s\n", target_str);
			goto out_free_sbi;
		}

		/* Set up injection: source file (path_to_mount) shadows the target */
		err = kern_path(path_to_mount, LOOKUP_FOLLOW, &sbi->inject_path);
		if (err) {
			pr_err("NoMountFS: Could not find source file: %s\n", path_to_mount);
			path_put(&target_path);
			goto out_free_sbi;
		}

		/* Store the target filename for lookup interception */
		strlcpy(sbi->inject_name, target_path.dentry->d_name.name,
			sizeof(sbi->inject_name));
		sbi->has_inject = true;

		/*
		 * Bug fix: use the PARENT directory as the lower_path root, not
		 * the target file itself. d_make_root requires a directory inode.
		 */
		parent_dentry = dget_parent(target_path.dentry);
		sbi->lower_paths[0].dentry = parent_dentry;
		sbi->lower_paths[0].mnt = mntget(target_path.mnt);
		sbi->num_lower_paths = 1;

		/* Release the original file-level reference; we hold parent now */
		path_put(&target_path);
	} else {
		sbi->num_lower_paths = 0;

		/* Process upperdir (layer 0) if provided, mimicking OverlayFS */
		if (upperdir_str && *upperdir_str) {
			err = kern_path(upperdir_str, LOOKUP_FOLLOW, &sbi->lower_paths[sbi->num_lower_paths]);
			if (err) {
				pr_err("NoMountFS: Could not find upperdir path: %s\n", upperdir_str);
				goto out_put_path;
			}
			sbi->num_lower_paths++;
		}

		/* Process lowerdir(s), splitting by : for union branches */
		if (path_to_mount && *path_to_mount) {
			branch_ptr = path_to_mount;
			while ((branch_str = strsep(&branch_ptr, ":")) != NULL) {
				if (!*branch_str) continue;
				if (sbi->num_lower_paths >= NOMOUNT_MAX_BRANCHES) {
					pr_err("NoMountFS: Maximum stack depth reached (%d)\n", NOMOUNT_MAX_BRANCHES);
					err = -EINVAL;
					goto out_put_path;
				}
				err = kern_path(branch_str, LOOKUP_FOLLOW, &sbi->lower_paths[sbi->num_lower_paths]);
				if (err) {
					pr_err("NoMountFS: Could not find lowerdir path: %s\n", branch_str);
					goto out_put_path;
				}
				sbi->num_lower_paths++;
			}
		}
		
		if (sbi->num_lower_paths == 0) {
			pr_err("NoMountFS: Missing valid upperdir or lowerdir options.\n");
			err = -EINVAL;
			goto out_free_sbi;
		}

		/* Handle Magic Mount injection options */
		if (inject_name_str && inject_path_str) {
			err = kern_path(inject_path_str, LOOKUP_FOLLOW, &sbi->inject_path);
			if (err) {
				pr_err("NoMountFS: Error accessing injected file '%s'\n", inject_path_str);
				goto out_put_path;
			}
			strlcpy(sbi->inject_name, inject_name_str, sizeof(sbi->inject_name));
			sbi->has_inject = true;
		}
	}

	sbi->lower_sb = sbi->lower_paths[0].dentry->d_sb;
	sb->s_op = &nomount_sops;
	sb->s_d_op = &nomount_dops;
	sb->s_export_op = &nomount_export_ops;
	sb->s_xattr = nomount_xattr_handlers;

	if (sbi->lower_sb && sbi->lower_sb->s_magic) {
        sb->s_magic = sbi->lower_sb->s_magic;
    } else {
        /* Fallback if lower SB is weird */
        sb->s_magic = NOMOUNT_FS_MAGIC;
    }

	sb->s_maxbytes = sbi->lower_sb->s_maxbytes;
	
	/* Calculate maximum stack depth among all branches */
	sb->s_stack_depth = 0;
	for (i = 0; i < sbi->num_lower_paths; i++) {
		if (sbi->lower_paths[i].dentry->d_sb->s_stack_depth > sb->s_stack_depth) {
			sb->s_stack_depth = sbi->lower_paths[i].dentry->d_sb->s_stack_depth;
		}
	}
	sb->s_stack_depth++; /* Our layer counts as +1 */

	/* Check stack depth to prevent excessive stacking */
	if (sb->s_stack_depth > FILESYSTEM_MAX_STACK_DEPTH) {
		pr_err("NoMountFS: Maximum overall stack depth exceeded (%d > %d)\n",
		       sb->s_stack_depth, FILESYSTEM_MAX_STACK_DEPTH);
		err = -EINVAL;
		goto out_put_inject;
	}

	root_inode = nomount_iget(sb, d_inode(sbi->lower_paths[0].dentry));
	if (IS_ERR(root_inode)) {
		err = PTR_ERR(root_inode);
		goto out_put_inject;
	}

	/* Create the virtual root dentry */
	root = d_make_root(root_inode);
	if (!root) {
		pr_err("NoMountFS: Failed to create root dentry\n");
		err = -ENOMEM;
		goto out_put_inject;
	}

	/* Setup root private data */
	err = new_dentry_private_data(root);
	if (err) {
		dput(root);
		goto out_put_inject; /* d_make_root handles inode on failure if dput handles failure*/
	}

	nomount_set_lower_paths(root, sbi->lower_paths, sbi->num_lower_paths);
	sb->s_root = root;
	d_set_d_op(sb->s_root, &nomount_dops);

	/* d_make_root already hashes the dentry, no need to rehash */

	return 0;

out_put_inject:
	if (sbi->has_inject) path_put(&sbi->inject_path);
out_put_path:
	for (i = 0; i < sbi->num_lower_paths; i++) {
		if (sbi->lower_paths[i].dentry)
			path_put(&sbi->lower_paths[i]);
	}
out_free_sbi:
	kfree(sbi);
	sb->s_fs_info = NULL;
	return err;
}

/* --- Cache Management --- */

static void init_once(void *obj)
{
	struct nomount_inode_info *i = obj;
	inode_init_once(&i->vfs_inode);
}

int nomount_init_inode_cache(void)
{
	nomount_inode_cachep = kmem_cache_create("nomount_inode_cache",
				sizeof(struct nomount_inode_info), 0,
				SLAB_RECLAIM_ACCOUNT, init_once);
	return nomount_inode_cachep ? 0 : -ENOMEM;
}

void nomount_destroy_inode_cache(void)
{
	if (nomount_inode_cachep)
		kmem_cache_destroy(nomount_inode_cachep);
}

/* --- NFS / Export Operations ---
 * These allow NoMountFS to handle file handles, 
 * making it compatible with NFS and advanced tracing tools.
 */

static struct inode *nomount_nfs_get_inode(struct super_block *sb, u64 ino,
					  u32 generation)
{
	struct super_block *lower_sb = NOMOUNT_SB(sb)->lower_sb;
	struct inode *lower_inode;

	/* Find the inode in the lower filesystem */
	lower_inode = ilookup(lower_sb, (unsigned long)ino);
	if (!lower_inode)
		return ERR_PTR(-ESTALE);

	/* Wrap it into a NoMountFS inode */
	return nomount_iget(sb, lower_inode);
}

static struct dentry *nomount_fh_to_dentry(struct super_block *sb,
					  struct fid *fid, int fh_len,
					  int fh_type)
{
	/* Forward the handle-to-dentry conversion using our nfs_get_inode */
	return generic_fh_to_dentry(sb, fid, fh_len, fh_type,
				    nomount_nfs_get_inode);
}

static struct dentry *nomount_fh_to_parent(struct super_block *sb,
					  struct fid *fid, int fh_len,
					  int fh_type)
{
	/* Forward the handle-to-parent conversion */
	return generic_fh_to_parent(sb, fid, fh_len, fh_type,
				    nomount_nfs_get_inode);
}

const struct export_operations nomount_export_ops = {
	.fh_to_dentry	   = nomount_fh_to_dentry,
	.fh_to_parent	   = nomount_fh_to_parent,
};
