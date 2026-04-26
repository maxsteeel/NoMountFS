/*
 * NoMountFS: Superblock operations and lifecycle management.
 */

#include "nomount.h"
#include "compat.h"

#include <linux/security.h>
#include <linux/uidgid.h>

/* * Private SELinux superblock structure extracted from 
 * 	 security/selinux/include/objsec.h.
 * * We redefine it here to manipulate SELinux behavior directly 
 *   without patching the kernel source tree.
 */
struct superblock_security_struct {
	u32 sid;
	u32 def_sid;
	u16 behavior;
	u16 flags;
	struct mutex lock;
	struct list_head isec_head;
	spinlock_t isec_lock;
};

/* Private SELinux inode structure (from security/selinux/include/objsec.h) */
struct inode_security_struct {
	struct inode *inode;
	struct list_head list;
	u32 task_sid;
	u32 sid;
	u16 sclass;
	unsigned char initialized;
};

/* Constant to mark inode as properly initialized for SELinux */
#define LABEL_INITIALIZED 1

/* Constants from security/selinux/include/security.h */
#define SBLABEL_MNT 0x01

static struct kmem_cache *nomount_inode_cachep;

/* * nomount_evict_inode: Called when an inode is being removed from memory.
 */
static void nomount_evict_inode(struct inode *inode)
{
	struct inode *lower_inode;
	struct nomount_inode_info *nii = NOMOUNT_I(inode);
	struct nomount_dirent *nd, *tmp;

	/* Clean up cached directory entries */
	mutex_lock(&nii->readdir_mutex);
	list_for_each_entry_safe(nd, tmp, &nii->dirents_list, list) {
		hash_del(&nd->hash);
		list_del(&nd->list);
		kfree(nd);
	}
	mutex_unlock(&nii->readdir_mutex);

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
	if (unlikely(!i))
		return NULL;

	/* Initialize private data and VFS inode */
	memset(i, 0, offsetof(struct nomount_inode_info, vfs_inode));
	nomount_set_iversion(&i->vfs_inode, 1);

	hash_init(i->dirent_hashtable);
	INIT_LIST_HEAD(&i->dirents_list);
	mutex_init(&i->readdir_mutex);
	i->cache_populated = false;
	i->cache_version = 0;

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

	if (sbi->fake_type)
		kfree(sbi->fake_type);

	kfree(sbi);
	sb->s_fs_info = NULL;
}

int nomount_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct nomount_sb_info *sbi = NOMOUNT_SB(dentry->d_sb);
	int err;

	if (!sbi || sbi->num_lower_paths == 0) return -ENOSYS;

	/* We always measure the actual physical disk (the last layer), 
	 * ignoring if the current file comes from /data */
	err = vfs_statfs(&sbi->lower_paths[sbi->num_lower_paths - 1], buf);
	
	if (!err && sbi->lower_sb) {
		buf->f_type = sbi->lower_sb->s_magic;
	}
	
	return err;
}

/* * nomount_show_options: Displays mount options in /proc/mounts.
 * Required for the system to identify the mount source.
 */
static int nomount_show_options(struct seq_file *m, struct dentry *root)
{
	struct nomount_sb_info *sbi;
	uid_t uid;
	int i;

	if (!root) return 0;
	sbi = NOMOUNT_SB(root->d_sb);
	if (!sbi) return 0;

	uid = from_kuid_munged(current_user_ns(), current_uid());

	if (uid >= 1000) {
		return 0;
	}

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

/* Replace the word "none" in /proc/mounts with the actual physical block */
static int nomount_show_devname(struct seq_file *m, struct dentry *root)
{
	struct nomount_sb_info *sbi = NOMOUNT_SB(root->d_sb);
	if (sbi && sbi->lower_sb) {
		seq_printf(m, "/dev/block/%s", sbi->lower_sb->s_id);
	} else {
		seq_puts(m, "none");
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
	.show_devname = nomount_show_devname,
};

struct nomount_mount_opts {
	char *path_to_mount;
	char *upperdir_str;
	char *inject_name_str;
	char *inject_path_str;
	char *target_str;
	char *source_str;
};

/* Parse the mount options from the raw data passed to fill_super.
 * This supports both regular mounts and direct file injection mounts.
 */
static int nomount_parse_options(struct super_block *sb, struct nomount_mount_opts *parsed_opts, void *raw_data)
{
	struct nomount_mount_data *mdata = (struct nomount_mount_data *)raw_data;
	const char *dev_name = mdata ? mdata->dev_name : NULL;
	char *opts = mdata ? (char *)mdata->raw_data : NULL;
	char *p;

	/* Initialize the parsed options structure to 0 */
	memset(parsed_opts, 0, sizeof(*parsed_opts));

	/* Try to extract the path from the options (e.g. -o lowerdir=/path/example) */
	if (opts) {
		while ((p = strsep(&opts, ",")) != NULL) {
			if (!*p) continue;
			if (!strncmp(p, "lowerdir=", 9)) {
				parsed_opts->path_to_mount = p + 9; // The word "lowerdir=" is skipped
			} else if (!strncmp(p, "upperdir=", 9)) {
				parsed_opts->upperdir_str = p + 9; 
			} else if (!strncmp(p, "inject_name=", 12)) {
				parsed_opts->inject_name_str = p + 12;
			} else if (!strncmp(p, "inject_path=", 12)) {
				parsed_opts->inject_path_str = p + 12;
			} else if (!strncmp(p, "target=", 7)) {
				parsed_opts->target_str = p + 7;
			} else if (!strncmp(p, "source=", 7)) {
				parsed_opts->source_str = p + 7;
			}
		}
	}

	/*
	 * For direct file injection (source= + target=), map source= into
	 * path_to_mount so the target= branch at line ~257 can use it via
	 * kern_path(path_to_mount, ...). Without this, path_to_mount stays NULL
	 * and kern_path(NULL) panics.
	 */
	if (parsed_opts->source_str && *parsed_opts->source_str && !parsed_opts->path_to_mount)
		parsed_opts->path_to_mount = parsed_opts->source_str;

	/* Fallback: If there is no 'lowerdir', use the original dev_name */
	if (!parsed_opts->path_to_mount && dev_name && *dev_name) {
		/* Ignore generic words that are often used as filler */
		if (strcmp(dev_name, "none") != 0 && strcmp(dev_name, "nomountfs") != 0 && strcmp(dev_name, "KSU") != 0 && strcmp(dev_name, "APatch") != 0 && strcmp(dev_name, "magisk") != 0 && strcmp(dev_name, "worker") != 0) {
			parsed_opts->path_to_mount = (char *)dev_name;
		}
	}

	/*  Abort cleanly if there is nothing to mount.
	 *  Exception: source= + target= is a valid direct-injection mount
	 *  with no lowerdir required.
	 */
	if ((!parsed_opts->path_to_mount || !*parsed_opts->path_to_mount) &&
		!(parsed_opts->source_str && parsed_opts->target_str)) {
		return -EINVAL;
	}

	/* Update superblock ID so /proc/mounts displays it pretty */
	if (parsed_opts->path_to_mount && *parsed_opts->path_to_mount)
		strlcpy(sb->s_id, parsed_opts->path_to_mount, sizeof(sb->s_id));
	else if (parsed_opts->source_str && *parsed_opts->source_str)
		strlcpy(sb->s_id, parsed_opts->source_str, sizeof(sb->s_id));

	return 0;
}

/* Handle direct file injection with target= option
* Syntax: mount -t nomountfs none <mount_dir> -o source=<src>,target=<file_to_shadow>
* The lower_path must be the PARENT DIRECTORY of the target file, not the
* target file itself — the filesystem root must always be a directory inode.
*/
static int nomount_setup_direct_inject(struct nomount_sb_info *sbi, struct nomount_mount_opts *opts)
{
	struct path target_path;
	struct dentry *parent_dentry;
	struct path parent_path;
	int err;

	/* Resolve the target file path */
	err = kern_path(opts->target_str, LOOKUP_FOLLOW, &target_path);
	if (err) {
		return err;
	}

	/* Set up injection: source file (path_to_mount) shadows the target */
	err = kern_path(opts->path_to_mount, LOOKUP_FOLLOW, &sbi->inject_path);
	if (err) {
		path_put(&target_path);
		return err;
	}

	/* Store the target filename for lookup interception */
	strlcpy(sbi->inject_name, target_path.dentry->d_name.name,
		sizeof(sbi->inject_name));
	sbi->inject_name_len = strlen(sbi->inject_name);
	sbi->inject_name_hash = full_name_hash(NULL, sbi->inject_name, sbi->inject_name_len);
	sbi->has_inject = true;

	/*
	 * Use the PARENT directory as the lower_path root, not the target file itself.
	 * Use a private vfsmount clone so this path isn't affected by
	 * nomountfs being installed over the same directory.
	 */
	parent_dentry = dget_parent(target_path.dentry);
	parent_path.dentry = parent_dentry;
	parent_path.mnt = mntget(target_path.mnt); // same mnt, parent dentry
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
		return err;
	}
	sbi->num_lower_paths = 1;

	/* Release the original file-level reference; we hold parent now */
	path_put(&target_path);
	return 0;
}

static int nomount_setup_branches(struct nomount_sb_info *sbi, struct nomount_mount_opts *opts)
{
	char *branch_ptr;
	char *branch_str;
	int err;

	sbi->num_lower_paths = 0;

	/* Process upperdir (layer 0) if provided, mimicking OverlayFS */
	if (opts->upperdir_str && *opts->upperdir_str) {
		struct path raw;
		err = kern_path(opts->upperdir_str, LOOKUP_FOLLOW, &raw);
		if (err) {
			return err;
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
			return err;
		}
		/* raw.mnt ref is now owned by the private clone; release it */
		mntput(raw.mnt);
		strlcpy(sbi->lower_path_strs[sbi->num_lower_paths],
			opts->upperdir_str, PATH_MAX);
		sbi->num_lower_paths++;
	}

	/* Process lowerdir(s), splitting by : for union branches */
	if (opts->path_to_mount && *opts->path_to_mount) {
		branch_ptr = opts->path_to_mount;
		while ((branch_str = strsep(&branch_ptr, ":")) != NULL) {
			struct path raw;
			if (!*branch_str) continue;
			if (sbi->num_lower_paths >= NOMOUNT_MAX_BRANCHES) {
				return -EINVAL;
			}
			err = kern_path(branch_str, LOOKUP_FOLLOW, &raw);
			if (err) {
				return err;
			}
			sbi->lower_paths[sbi->num_lower_paths].dentry = dget(raw.dentry);
			sbi->lower_paths[sbi->num_lower_paths].mnt =
				nomount_clone_private_mount(&raw);
			if (IS_ERR(sbi->lower_paths[sbi->num_lower_paths].mnt)) {
				err = PTR_ERR(sbi->lower_paths[sbi->num_lower_paths].mnt);
				sbi->lower_paths[sbi->num_lower_paths].mnt = NULL;
				path_put(&raw);
				return err;
			}
			mntput(raw.mnt);
			strlcpy(sbi->lower_path_strs[sbi->num_lower_paths],
				branch_str, PATH_MAX);
			sbi->num_lower_paths++;
		}
	}
	
	if (sbi->num_lower_paths == 0) {
		return -EINVAL;
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
	if (opts->upperdir_str && sbi->num_lower_paths > 1) {
		struct inode *upper_inode = d_inode(sbi->lower_paths[0].dentry);
		int li;
		for (li = 1; li < sbi->num_lower_paths; li++) {
			if (d_inode(sbi->lower_paths[li].dentry) == upper_inode) {
				return -EINVAL;
			}
		}
	}

	/* Handle Magic Mount injection options */
	if (opts->inject_name_str && opts->inject_path_str) {
		struct path raw;
		err = kern_path(opts->inject_path_str, LOOKUP_FOLLOW, &raw);
		if (err) {
			return err;
		}
		sbi->inject_path.dentry = dget(raw.dentry);
		sbi->inject_path.mnt = nomount_clone_private_mount(&raw);
		if (IS_ERR(sbi->inject_path.mnt)) {
			err = PTR_ERR(sbi->inject_path.mnt);
			sbi->inject_path.mnt = NULL;
			path_put(&raw);
			return err;
		}
		mntput(raw.mnt);
		strlcpy(sbi->inject_name, opts->inject_name_str, sizeof(sbi->inject_name));
		sbi->inject_name_len = strlen(sbi->inject_name);
		sbi->inject_name_hash = full_name_hash(NULL, sbi->inject_name, sbi->inject_name_len);
		strlcpy(sbi->inject_path_str, opts->inject_path_str, PATH_MAX);
		sbi->has_inject = true;
	}

	return 0;
}

/*
 * lower_sb must come from the REAL partition (last lower path), not
 * the module's upperdir. The upperdir lives on /data (f2fs/ext4) while
 * the real partition is /system (ext4). We clone security opts from the
 * real partition's superblock — that's the one with fs_use_xattr set up
 * correctly for system_file labels. Using the module dir's sb here would
 * pass the wrong sb type to security_sb_clone_mnt_opts and could BUG().
 */
static int nomount_setup_superblock(struct super_block *sb, struct nomount_sb_info *sbi)
{
	struct super_block *lsb;
	struct inode *root_inode;
	struct dentry *root;
	int err;
	int i;

	sbi->lower_sb = sbi->lower_paths[sbi->num_lower_paths - 1].dentry->d_sb;
	if (!sbi->lower_sb) {
		return -EINVAL;
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
		lsb = sbi->lower_paths[i].dentry->d_sb;
		if (lsb->s_stack_depth > sb->s_stack_depth) {
			sb->s_stack_depth = lsb->s_stack_depth;
		}
	}
	sb->s_stack_depth++;

	/* Check stack depth to prevent excessive stacking */
	if (sb->s_stack_depth > FILESYSTEM_MAX_STACK_DEPTH) {
		return -EINVAL;
	}

	root_inode = nomount_iget(sb, d_inode(sbi->lower_paths[sbi->num_lower_paths - 1].dentry));
	if (IS_ERR(root_inode)) {
		return PTR_ERR(root_inode);
	}

	/*
	 * d_make_root calls SELinux immediately. If we use it, SELinux 
	 * will try to read our xattr BEFORE we have linked the 
	 * physical routes. Solution: Build the dentry manually.
	 */
	root = d_alloc_anon(sb);
	if (!root) {
		iput(root_inode);
		return -ENOMEM;
	}

	/* The private data of the dentry (physical paths) is prepared */
	err = new_dentry_private_data(root);
	if (err) {
		dput(root);
		iput(root_inode);
		return err;
	}
	nomount_set_lower_paths(root, sbi->lower_paths, sbi->num_lower_paths);

	/* Step B: The inode is instantiated to the dentry.
	 * SELinux wakes up here, it will call our nomount_getxattr, and 
	 * since the route is already set, you will get the correct label.
	 */
	d_instantiate(root, root_inode);

	sb->s_root = root;
	d_set_d_op(sb->s_root, &nomount_dops);

	return 0;
}

static void nomount_setup_selinux(struct super_block *sb, struct nomount_sb_info *sbi)
{
	struct inode *root_inode = d_inode(sb->s_root);
	struct inode *root_lower = d_inode(sbi->lower_paths[sbi->num_lower_paths - 1].dentry);
	char *ctx = NULL;
	unsigned int ctxlen = 0;
	unsigned long set_kern_flags = 0;
	int err;

	/* Clone mount options for SELinux */
	err = security_sb_clone_mnt_opts(sbi->lower_sb, sb, 0, &set_kern_flags);
	if (err) {
		pr_err("nomount: security_sb_clone_mnt_opts failed: %d\n", err);
	}

	/* Now that SBLABEL_MNT is set, label the root inode from lower */
	if (security_inode_getsecctx(root_lower, (void **)&ctx, &ctxlen) == 0 && ctx) {
		security_inode_notifysecctx(root_inode, ctx, ctxlen);
		security_release_secctx(ctx, ctxlen);
	}
}

int nomount_fill_super(struct super_block *sb, void *raw_data, int silent)
{
	struct nomount_sb_info *sbi;
	struct nomount_mount_opts opts;
	int err = 0;
	int i;

	/* 1. Unpack mount_data */
	err = nomount_parse_options(sb, &opts, raw_data);
	if (err) {
		return err;
	}

	sbi = kzalloc(sizeof(struct nomount_sb_info), GFP_KERNEL);
	if (!sbi) return -ENOMEM;
	sb->s_fs_info = sbi;

	if (opts.target_str && *opts.target_str) {
		err = nomount_setup_direct_inject(sbi, &opts);
		if (err) {
			goto out_free_sbi;
		}
	} else {
		err = nomount_setup_branches(sbi, &opts);
		if (err) {
			goto out_put_path;
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
		err = -EINVAL;
		goto out_put_inject;
	}

	err = nomount_setup_superblock(sb, sbi);
	if (err) {
		goto out_put_inject;
	}

	nomount_setup_selinux(sb, sbi);

	sbi->fake_type = kzalloc(sizeof(struct file_system_type), GFP_KERNEL);
	if (sbi->fake_type) {
		*sbi->fake_type = nomount_fs_type;
		sbi->fake_type->name = sbi->lower_sb->s_type->name;
		sb->s_type = sbi->fake_type;
	}

	sb->s_dev = sbi->lower_sb->s_dev;

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
	/* SLAB_HWCACHE_ALIGN: Aligns objects to the CPU's L1/L2 cache lines.
	 * SLAB_MEM_SPREAD: Avoid concentrating memory on a single node.
	 * This makes bulk inode allocations more fast.
	 */
	nomount_inode_cachep = kmem_cache_create("nomount_inode_cache",
				sizeof(struct nomount_inode_info), 0,
				SLAB_RECLAIM_ACCOUNT | SLAB_HWCACHE_ALIGN | SLAB_MEM_SPREAD, init_once);
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
