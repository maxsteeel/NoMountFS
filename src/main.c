/*
 * NoMountFS: Main entry point and filesystem registration.
 */

#include "nomount.h"
#include "compat.h"

#include <linux/module.h>

/* Function to handle the actual mounting process */
static struct dentry *nomount_mount(struct file_system_type *fs_type,
				  int flags, const char *dev_name,
				  void *raw_data)
{
	/*
     * mount_nodev is used because this is a virtual filesystem,
	 * it doesn't need a physical block device (like /dev/block/sda).
	 */

	/*  Wrap both arguments so as not to lose mount options (-o) */
	struct nomount_mount_data mdata = {
		.dev_name = dev_name,
		.raw_data = raw_data
	};

	/* The structure is passed instead of the direct dev_name */
	return mount_nodev(fs_type, flags, &mdata, nomount_fill_super);
}

/* File system definition */
static struct file_system_type nomount_fs_type = {
	.owner		= THIS_MODULE,
	.name		= NOMOUNT_NAME, /* This will be "nomountfs" */
	.mount		= nomount_mount,
	.kill_sb	= generic_shutdown_super,
	.fs_flags	= FS_RENAME_DOES_D_MOVE,
};

static int __init init_nomount_fs(void)
{
	int err;

	pr_info("NoMountFS: Registering filesystem...\n");

	/* Initialize the memory cache for our inodes */
	err = nomount_init_inode_cache();
	if (err)
		goto out;

	/* Initialize the memory cache for our dentries */
	err = nomount_init_dentry_cache();
	if (err)
		goto out_free_inode_cache;

	/* Register the filesystem in the VFS layer */
	err = register_filesystem(&nomount_fs_type);
	if (err)
		goto out_free_dentry_cache;

	pr_info("NoMountFS: Successfully registered.\n");
	return 0;

out_free_dentry_cache:
	nomount_destroy_dentry_cache();
out_free_inode_cache:
	nomount_destroy_inode_cache();
out:
	return err;
}

static void __exit exit_nomount_fs(void)
{
	pr_info("NoMountFS: Unregistering filesystem...\n");
	unregister_filesystem(&nomount_fs_type);
	nomount_destroy_dentry_cache();
	nomount_destroy_inode_cache();
}

MODULE_AUTHOR("Erez Zadok (WrapFS), maxsteeel (NoMountFS)");
MODULE_DESCRIPTION("NoMountFS: A transparent stackable filesystem for Android");
MODULE_LICENSE("GPL");

module_init(init_nomount_fs);
module_exit(exit_nomount_fs);
