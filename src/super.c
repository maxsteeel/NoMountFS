/*
 * NoMountFS: Superblock operations and lifecycle management.
 */

#include "nomount.h"
#include "compat.h"

#include <linux/security.h>

extern void selinux_sb_copy_sid_from(struct super_block *dst, struct super_block *src);

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
	int i;

	if (!sbi)
		return;

	/* Release all lower path references acquired during fill_super */
	for (i = 0; i < sbi->num_lower_paths; i++) {
		if (sbi->lower_paths[i].dentry)
			path_put(&sbi->lower_paths[i]);
	}

	if (sbi->has_inject)
		path_put(&sbi->inject_path);

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

	if (sbi->num_lower_paths > 0 && sbi->lower_path_strs[0][0]) {
		/*
		 * Emit the original path strings saved during fill_super.
		 * d_path() on private vfsmount clones returns "/" (the clone
		 * is detached from the mount namespace and has no resolvable
		 * path), which causes init to remount with upperdir=/ and
		 * corrupt the filesystem view. Use the saved strings instead.
		 */
		seq_show_option(m, "upperdir", sbi->lower_path_strs[0]);

		if (sbi->num_lower_paths > 1) {
			seq_puts(m, ",lowerdir=");
			for (i = 1; i < sbi->num_lower_paths; i++) {
				if (i > 1) seq_puts(m, ":");
				if (sbi->lower_path_strs[i][0])
					seq_escape(m, sbi->lower_path_strs[i],
						   ", \t\n\\");
			}
		}
	}

	if (sbi->has_inject) {
		seq_show_option(m, "inject_name", sbi->inject_name);
		if (sbi->inject_path_str[0])
			seq_show_option(m, "inject_path", sbi->inject_path_str);
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

	/*
	 * 3. For direct file injection (source= + target=), map source= into
	 * path_to_mount so the target= branch at line ~257 can use it via
	 * kern_path(path_to_mount, ...). Without this, path_to_mount stays NULL
	 * and kern_path(NULL) panics.
	 */
	if (source_str && *source_str && !path_to_mount)
		path_to_mount = source_str;

	/* 4. Fallback: If there is no 'lowerdir', use the original dev_name */
	if (!path_to_mount && dev_name && *dev_name) {
		/* Ignore generic words that are often used as filler */
		if (strcmp(dev_name, "none") != 0 && strcmp(dev_name, "nomountfs") != 0 && strcmp(dev_name, "KSU") != 0 && strcmp(dev_name, "APatch") != 0 && strcmp(dev_name, "magisk") != 0 && strcmp(dev_name, "worker") != 0) {
			path_to_mount = (char *)dev_name;
		}
	}

	/* 5. Abort cleanly if there is nothing to mount.
	 *    Exception: source= + target= is a valid direct-injection mount
	 *    with no lowerdir required.
	 */
	if ((!path_to_mount || !*path_to_mount) && !(source_str && target_str)) {
		pr_err("NoMountFS: Missing source path.\n");
		return -EINVAL;
	}

	/* Update superblock ID so /proc/mounts displays it pretty */
	if (path_to_mount && *path_to_mount)
		strlcpy(sb->s_id, path_to_mount, sizeof(sb->s_id));
	else if (source_str && *source_str)
		strlcpy(sb->s_id, source_str, sizeof(sb->s_id));

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
		struct path parent_path;
		
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
		 * Use a private vfsmount clone so this path isn't affected by
		 * nomountfs being installed over the same directory.
		 */
		parent_dentry = dget_parent(target_path.dentry);
		parent_path.dentry = parent_dentry;
		parent_path.mnt = mntget(target_path.mnt);  // same mnt, parent dentry
		sbi->lower_paths[0].dentry = parent_dentry;
		sbi->lower_paths[0].mnt = nomount_clone_private_mount(&parent_path);
		mntput(parent_path.mnt);
		if (IS_ERR(sbi->lower_paths[0].mnt)) {
			err = PTR_ERR(sbi->lower_paths[0].mnt);
			sbi->lower_paths[0].mnt = NULL;
			dput(parent_dentry);
			path_put(&sbi->inject_path);
			sbi->has_inject = false;
			path_put(&target_path);
			goto out_free_sbi;
		}
		sbi->num_lower_paths = 1;

		/* Release the original file-level reference; we hold parent now */
		path_put(&target_path);
	} else {
		sbi->num_lower_paths = 0;

		/* Process upperdir (layer 0) if provided, mimicking OverlayFS */
		if (upperdir_str && *upperdir_str) {
			struct path raw;
			err = kern_path(upperdir_str, LOOKUP_FOLLOW, &raw);
			if (err) {
				pr_err("NoMountFS: Could not find upperdir path: %s\n", upperdir_str);
				goto out_put_path;
			}
			/*
			 * Clone a private vfsmount for each layer path.
			 * This detaches our reference from the public mount namespace,
			 * so if nomountfs is mounted over the same path as one of its
			 * layers, dentry_open and iterate_dir on that layer always
			 * reach the real underlying fs — never loop back into us.
			 */
			sbi->lower_paths[sbi->num_lower_paths].dentry = dget(raw.dentry);
			sbi->lower_paths[sbi->num_lower_paths].mnt =
				nomount_clone_private_mount(&raw);
			if (IS_ERR(sbi->lower_paths[sbi->num_lower_paths].mnt)) {
				err = PTR_ERR(sbi->lower_paths[sbi->num_lower_paths].mnt);
				sbi->lower_paths[sbi->num_lower_paths].mnt = NULL;
				path_put(&raw);
				goto out_put_path;
			}
			/* raw.mnt ref is now owned by the private clone; release it */
			mntput(raw.mnt);
			strlcpy(sbi->lower_path_strs[sbi->num_lower_paths],
				upperdir_str, PATH_MAX);
			sbi->num_lower_paths++;
		}

		/* Process lowerdir(s), splitting by : for union branches */
		if (path_to_mount && *path_to_mount) {
			branch_ptr = path_to_mount;
			while ((branch_str = strsep(&branch_ptr, ":")) != NULL) {
				struct path raw;
				if (!*branch_str) continue;
				if (sbi->num_lower_paths >= NOMOUNT_MAX_BRANCHES) {
					pr_err("NoMountFS: Maximum stack depth reached (%d)\n", NOMOUNT_MAX_BRANCHES);
					err = -EINVAL;
					goto out_put_path;
				}
				err = kern_path(branch_str, LOOKUP_FOLLOW, &raw);
				if (err) {
					pr_err("NoMountFS: Could not find lowerdir path: %s\n", branch_str);
					goto out_put_path;
				}
				sbi->lower_paths[sbi->num_lower_paths].dentry = dget(raw.dentry);
				sbi->lower_paths[sbi->num_lower_paths].mnt =
					nomount_clone_private_mount(&raw);
				if (IS_ERR(sbi->lower_paths[sbi->num_lower_paths].mnt)) {
					err = PTR_ERR(sbi->lower_paths[sbi->num_lower_paths].mnt);
					sbi->lower_paths[sbi->num_lower_paths].mnt = NULL;
					path_put(&raw);
					goto out_put_path;
				}
				mntput(raw.mnt);
				strlcpy(sbi->lower_path_strs[sbi->num_lower_paths],
					branch_str, PATH_MAX);
				sbi->num_lower_paths++;
			}
		}
		
		if (sbi->num_lower_paths == 0) {
			pr_err("NoMountFS: Missing valid upperdir or lowerdir options.\n");
			err = -EINVAL;
			goto out_free_sbi;
		}

		/*
		 * Guard: reject if any lowerdir is the exact same directory as
		 * upperdir (same inode, same superblock). This would cause
		 * nomount_lookup to see identical entries in both layers and
		 * produce confusing duplicate results.
		 *
		 * The more dangerous case — lowerdir being the same path as the
		 * VFS mount point — cannot be detected here because fill_super
		 * runs before the mount is installed. That case is handled safely
		 * in nomount_lookup via follow_down(), which traverses past any
		 * mount point sitting on a lower dentry, ensuring we always reach
		 * the real underlying filesystem rather than looping back into
		 * ourselves.
		 */
		if (upperdir_str && sbi->num_lower_paths > 1) {
			struct inode *upper_inode = d_inode(sbi->lower_paths[0].dentry);
			int li;
			for (li = 1; li < sbi->num_lower_paths; li++) {
				if (d_inode(sbi->lower_paths[li].dentry) == upper_inode) {
					pr_err("NoMountFS: lowerdir[%d] is identical to "
					       "upperdir — redundant layer rejected\n", li - 1);
					err = -EINVAL;
					goto out_put_path;
				}
			}
		}

		/* Handle Magic Mount injection options */
		if (inject_name_str && inject_path_str) {
			struct path raw;
			err = kern_path(inject_path_str, LOOKUP_FOLLOW, &raw);
			if (err) {
				pr_err("NoMountFS: Error accessing injected file '%s'\n", inject_path_str);
				goto out_put_path;
			}
			sbi->inject_path.dentry = dget(raw.dentry);
			sbi->inject_path.mnt = nomount_clone_private_mount(&raw);
			if (IS_ERR(sbi->inject_path.mnt)) {
				err = PTR_ERR(sbi->inject_path.mnt);
				sbi->inject_path.mnt = NULL;
				path_put(&raw);
				goto out_put_path;
			}
			mntput(raw.mnt);
			strlcpy(sbi->inject_name, inject_name_str, sizeof(sbi->inject_name));
			strlcpy(sbi->inject_path_str, inject_path_str, PATH_MAX);
			sbi->has_inject = true;
		}
	}

	/*
	 * lower_sb must come from the REAL partition (last lower path), not
	 * the module's upperdir. The upperdir lives on /data (f2fs/ext4) while
	 * the real partition is /system (ext4). We clone security opts from the
	 * real partition's superblock — that's the one with fs_use_xattr set up
	 * correctly for system_file labels. Using the module dir's sb here would
	 * pass the wrong sb type to security_sb_clone_mnt_opts and could BUG().
	 */
	if (sbi->num_lower_paths == 0) {
		pr_err("NoMountFS: No lower paths configured — cannot initialize superblock\n");
		err = -EINVAL;
		goto out_put_inject;
	}

	sbi->lower_sb = sbi->lower_paths[sbi->num_lower_paths - 1].dentry->d_sb;
	if (!sbi->lower_sb) {
		pr_err("NoMountFS: Lower superblock is NULL — cannot proceed\n");
		err = -EINVAL;
		goto out_put_inject;
	}

	sb->s_op = &nomount_sops;
	sb->s_d_op = &nomount_dops;
	sb->s_export_op = &nomount_export_ops;
	sb->s_xattr = nomount_xattr_handlers;

	/*
	 * NOTE: security_sb_clone_mnt_opts() call has been removed.
	 * 
	 * The selinux_sb_clone_mnt_opts() function internally calls sb_finish_set_opts()
	 * which can dereference NULL pointers in the superblock's security structures
	 * on certain Android kernel versions (like 5.4). The crash occurs at offset 0x30
	 * in sb_finish_set_opts, indicating access to an uninitialized security blob.
	 *
	 * SELinux labeling still works via genfscon rules in sepolicy.rule which label
	 * inodes based on filesystem path (e.g., /system/ gets system_file labels).
	 * This is the standard mechanism for read-only filesystems and doesn't require
	 * cloning mount options.
	 *
	 * Our xattr handler forwards security.selinux getxattr to the lower inode,
	 * so existing xattr-based labels are preserved when available.
	 */

	/*
	 * Always use our own magic number. Spoofing the lower filesystem's
	 * magic (e.g. ext4's 0xEF53) confuses SELinux's genfscon lookup,
	 * which matches on both fs_type->name AND s_magic. With a spoofed
	 * magic the kernel finds no matching genfscon entry for "nomountfs"
	 * and falls back to unlabeled — defeating any sepolicy rules.
	 */
	sb->s_magic = NOMOUNT_FS_MAGIC;

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

	root_inode = nomount_iget(sb, d_inode(sbi->lower_paths[sbi->num_lower_paths - 1].dentry));
	if (IS_ERR(root_inode)) {
		err = PTR_ERR(root_inode);
		pr_err("NoMountFS: Failed to get root inode: %d\n", err);
		goto out_put_inject;
	}

	/* Create the virtual root dentry */
	root = d_make_root(root_inode);
	if (!root) {
		pr_err("NoMountFS: Failed to create root dentry\n");
		err = -ENOMEM;
		/* d_make_root frees the inode on failure */
		goto out_put_inject;
	}

	/* Setup root private data */
	err = new_dentry_private_data(root);
	if (err) {
		pr_err("NoMountFS: Failed to allocate root private data: %d\n", err);
		dput(root);
		goto out_put_inject;
	}

	nomount_set_lower_paths(root, sbi->lower_paths, sbi->num_lower_paths);
	sb->s_root = root;
	d_set_d_op(sb->s_root, &nomount_dops);

	/* d_make_root already hashes the dentry, no need to rehash */

	err = security_sb_set_mnt_opts(sb, NULL, 0, NULL);
	if (err && err != -EOPNOTSUPP) {
		pr_err("NoMountFS: security_sb_set_mnt_opts failed: %d\n", err);
		err = 0;
	}
	selinux_sb_copy_sid_from(sb,
		sbi->lower_paths[sbi->num_lower_paths - 1].dentry->d_sb);

	/* Now that SBLABEL_MNT is set, label the root inode from lower */
	{
		struct inode *root_lower = d_inode(sbi->lower_paths[sbi->num_lower_paths - 1].dentry);
		char *ctx = NULL;
		unsigned int ctxlen = 0;
		int sec_err = security_inode_getsecctx(root_lower, (void **)&ctx, &ctxlen);
		if (sec_err == 0 && ctx) {
			security_inode_notifysecctx(sb->s_root->d_inode, ctx, ctxlen);
			security_release_secctx(ctx, ctxlen);
		}
	}

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
