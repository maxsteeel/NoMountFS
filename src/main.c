/*
 * NoMountFS: Main entry point and filesystem registration.
 */

#include "nomount.h"
#include "compat.h"

#include <linux/module.h>

struct cred *nmfs_cred = NULL;

/* Function to handle the actual mounting process */
static struct dentry *nomount_mount(struct file_system_type *fs_type,
				  int flags, const char *dev_name,
				  void *raw_data)
{
	/*
     * mount_nodev is used because this is a virtual filesystem,
	 * it doesn't need a physical block device (like /dev/block/sda).
	 */

	/* Wrap both arguments so as not to lose mount options (-o) */
	struct nomount_mount_data mdata = {
		.dev_name = dev_name,
		.raw_data = raw_data
	};

	/* The structure is passed instead of the direct dev_name */
	return mount_nodev(fs_type, flags, &mdata, nomount_fill_super);
}

/* File system definition */
struct file_system_type nomount_fs_type = {
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

	/* 1. Allocate credentials */
	nmfs_cred = prepare_creds();
	if (!nmfs_cred) {
		pr_err("nomount: prepare cred failed!\n");
		return -ENOMEM;
	}

#ifdef NOMOUNT_FS_KERNEL_UMOUNT
	/* 2. Initialize umount subsystem */
	nomount_kernel_umount_init();
#endif

	/* 3. Initialize inode cache */
	err = nomount_init_inode_cache();
	if (err)
		goto out_umount_exit;

	/* 4. Initialize dentry cache */
	err = nomount_init_dentry_cache();
	if (err)
		goto out_free_inode_cache;
		
	/* 5. Initialize dirent cache */
	err = nomount_init_dirent_cache();
	if (err)
		goto out_free_dentry_cache;

	/* 6. Register the filesystem */
	err = register_filesystem(&nomount_fs_type);
	if (err)
		goto out_free_dirent_cache;

#ifdef NOMOUNT_FS_KERNEL_UMOUNT
	/* 7. Initialize hooks */
	err = nomount_init_hooks();
	if (err)
		goto out_unregister_fs;
#endif

	pr_info("NoMountFS: Successfully registered.\n");
	return 0;

out_unregister_fs:
	unregister_filesystem(&nomount_fs_type);
out_free_dirent_cache:
	nomount_destroy_dirent_cache();
out_free_dentry_cache:
	nomount_destroy_dentry_cache();
out_free_inode_cache:
	nomount_destroy_inode_cache();
out_umount_exit:
#ifdef NOMOUNT_FS_KERNEL_UMOUNT
	nomount_kernel_umount_exit();
#endif
	if (nmfs_cred) {
		put_cred(nmfs_cred);
		nmfs_cred = NULL;
	}

	return err;
}

static void __exit exit_nomount_fs(void)
{
	pr_info("NoMountFS: Unregistering filesystem...\n");

#ifdef NOMOUNT_FS_KERNEL_UMOUNT
	/* Unregister tracepoint hooks first to stop new interceptions safely */
	nomount_exit_hooks();
#endif

	unregister_filesystem(&nomount_fs_type);
	
	/* Destroy caches only after VFS is unregistered and no one can use it */
	nomount_destroy_dirent_cache();
	nomount_destroy_dentry_cache();
	nomount_destroy_inode_cache();

#ifdef NOMOUNT_FS_KERNEL_UMOUNT
	nomount_kernel_umount_exit();
#endif

	if (nmfs_cred) {
		put_cred(nmfs_cred);
		nmfs_cred = NULL;
	}
}

MODULE_AUTHOR("Erez Zadok (WrapFS), maxsteeel (NoMountFS)");
MODULE_DESCRIPTION("NoMountFS: A transparent stackable filesystem for Android");
MODULE_LICENSE("GPL");

module_init(init_nomount_fs);
module_exit(exit_nomount_fs);
