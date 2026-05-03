/*
 * Mirage: Main entry point and filesystem registration.
 */

#include "mirage.h"
#include "compat.h"

#include <linux/module.h>

struct cred *mirage_cred = NULL;

/* Function to handle the actual mounting process */
static struct dentry *mirage_mount(struct file_system_type *fs_type,
				  int flags, const char *dev_name,
				  void *raw_data)
{
	/*
     * mount_nodev is used because this is a virtual filesystem,
	 * it doesn't need a physical block device (like /dev/block/sda).
	 */

	/* Wrap both arguments so as not to lose mount options (-o) */
	struct mirage_mount_data mdata = {
		.dev_name = dev_name,
		.raw_data = raw_data
	};

	/* The structure is passed instead of the direct dev_name */
	return mount_nodev(fs_type, flags, &mdata, mirage_fill_super);
}

/* File system definition */
struct file_system_type mirage_fs_type = {
	.owner		= THIS_MODULE,
	.name		= NOMOUNT_NAME, /* This will be "mirage" */
	.mount		= mirage_mount,
	.kill_sb	= generic_shutdown_super,
	.fs_flags	= FS_RENAME_DOES_D_MOVE,
};

static int __init init_mirage_vfs(void)
{
	int err;

	pr_info("mirage: Registering virtual filesystem...\n");

	/* 1. Allocate credentials */
	mirage_cred = prepare_creds();
	if (!mirage_cred) {
		pr_err("mirage: prepare cred failed!\n");
		return -ENOMEM;
	}

#ifdef MIRAGE_KERNEL_UMOUNT
	/* 2. Initialize umount subsystem */
	mirage_kernel_umount_init();
#endif

	/* 3. Initialize inode cache */
	err = mirage_init_inode_cache();
	if (err)
		goto out_umount_exit;

	/* 4. Initialize dentry cache */
	err = mirage_init_dentry_cache();
	if (err)
		goto out_free_inode_cache;
		
	/* 5. Initialize dirent cache */
	err = mirage_init_dirent_cache();
	if (err)
		goto out_free_dentry_cache;

	/* 6. Register the filesystem */
	err = register_filesystem(&mirage_fs_type);
	if (err)
		goto out_free_dirent_cache;

#ifdef MIRAGE_KERNEL_UMOUNT
	/* 7. Initialize hooks */
	err = mirage_init_hooks();
	if (err)
		goto out_unregister_fs;
#endif

	pr_info("mirage: Successfully registered.\n");
	return 0;

out_unregister_fs:
	unregister_filesystem(&mirage_fs_type);
out_free_dirent_cache:
	mirage_destroy_dirent_cache();
out_free_dentry_cache:
	mirage_destroy_dentry_cache();
out_free_inode_cache:
	mirage_destroy_inode_cache();
out_umount_exit:
#ifdef MIRAGE_KERNEL_UMOUNT
	mirage_kernel_umount_exit();
#endif
	if (mirage_cred) {
		put_cred(mirage_cred);
		mirage_cred = NULL;
	}

	return err;
}

static void __exit exit_mirage_vfs(void)
{
	pr_info("mirage: Unregistering filesystem...\n");

#ifdef MIRAGE_KERNEL_UMOUNT
	/* Unregister tracepoint hooks first to stop new interceptions safely */
	mirage_exit_hooks();
#endif

	unregister_filesystem(&nomount_fs_type);
	
	/* Destroy caches only after VFS is unregistered and no one can use it */
	mirage_destroy_dirent_cache();
	mirage_destroy_dentry_cache();
	mirage_destroy_inode_cache();

#ifdef NOMOUNT_FS_KERNEL_UMOUNT
	mirage_kernel_umount_exit();
#endif

	if (mirage_cred) {
		put_cred(mirage_cred);
		mirage_cred = NULL;
	}
}

MODULE_AUTHOR("Erez Zadok (WrapFS), maxsteeel (Mirage)");
MODULE_DESCRIPTION("Mirage: A transparent stackable filesystem for Android");
MODULE_LICENSE("GPL");

module_init(init_mirage_vfs);
module_exit(exit_mirage_vfs);
