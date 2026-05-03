/*
 * Mirage: File operations
 * Handling read, write and open.
 */

#include "mirage.h"
#include "compat.h"
#include <linux/uio.h>

struct kmem_cache *mirage_dirent_cachep;

int mirage_init_dirent_cache(void)
{
	mirage_dirent_cachep = kmem_cache_create("mirage_dirent_cache",
											 sizeof(struct nomount_dirent), 0,
											 SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD, NULL);
	return mirage_dirent_cachep ? 0 : -ENOMEM;
}

void mirage_destroy_dirent_cache(void)
{
	if (mirage_dirent_cachep)
		kmem_cache_destroy(mirage_dirent_cachep);
}

/* * mirage_vfs_open: called when a file is being opened.
 * We must open the lower file and store its reference.
 */
static int mirage_vfs_open(struct inode *inode, struct file *file)
{
	int err = 0;
	struct file *lower_file = NULL;
	struct path lower_paths[MIRAGE_MAX_BRANCHES];
	int num_lower_paths;
	struct mirage_file_info *info;
	int i;

	/* Allocate private storage for lower file reference */
	info = kmem_cache_alloc(mirage_dirent_cachep, GFP_KERNEL);
	if (unlikely(!info))
		return -ENOMEM;
 
	info->ghost_emitted = false;
	info->num_lower_files = 0;

	if (!S_ISDIR(inode->i_mode)) {
		get_lower_path(file->f_path.dentry, &lower_paths[0]);
		num_lower_paths = lower_paths[0].dentry ? 1 : 0;
	} else {
		num_lower_paths = get_all_lower_paths(file->f_path.dentry, lower_paths);
	}
	
	for (i = 0; i < num_lower_paths; i++) {
		lower_file = dentry_open(&lower_paths[i], file->f_flags, current_cred());
		if (IS_ERR(lower_file)) {
			err = PTR_ERR(lower_file);
			break; /* If one fails, stop and cleanup */
		}
		info->lower_files[info->num_lower_files++] = lower_file;
		
		/* If it's not a directory, we only open the topmost one */
		if (!S_ISDIR(inode->i_mode))
			break;
	}

	if (!S_ISDIR(inode->i_mode)) {
		if (lower_paths[0].dentry)
			put_lower_path(file->f_path.dentry, &lower_paths[0]);
	} else {
		put_all_lower_paths(file->f_path.dentry, lower_paths, num_lower_paths);
	}

	if (err && info->num_lower_files == 0) {
		kmem_cache_free(mirage_dirent_cachep, info);
		return err;
	} else if (err) {
		/* Failed partway through opening directories; close what we opened */
		for (i = 0; i < info->num_lower_files; i++) {
			fput(info->lower_files[i]);
		}
		kmem_cache_free(mirage_dirent_cachep, info);
		return err;
	} else if (info->num_lower_files == 0) {
		/* No files were opened — this is an error */
		kmem_cache_free(mirage_dirent_cachep, info);
		return -ENOENT;
	} else {
		file->private_data = info;
		fsstack_copy_attr_all(inode, mirage_vfs_lower_inode(inode));
	}
	return 0;
}

/* * mirage_vfs_release: close the lower file when the virtual one is closed.
 */
static int mirage_vfs_release(struct inode *inode, struct file *file)
{
	struct mirage_file_info *info = mirage_file(file);
	int i;

	if (info) {
		for (i = 0; i < info->num_lower_files; i++) {
			if (info->lower_files[i]) {
				fput(info->lower_files[i]);
			}
		}
		
		kmem_cache_free(nomount_dirent_cachep, info);
		file->private_data = NULL;
	}
	return 0;
}

/* * IOCTL: Critical for hardware-specific storage commands in Android.
 */
static long mirage_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct file *lower_file = mirage_vfs_lower_file(file);
	long err = -ENOTTY;

	if (lower_file->f_op && lower_file->f_op->unlocked_ioctl)
		err = lower_file->f_op->unlocked_ioctl(lower_file, cmd, arg);

	return err;
}

#ifdef CONFIG_COMPAT
/* * compat_ioctl: Handles 32-bit ioctl calls on 64-bit kernels.
 * Essential for Android compatibility with older apps/libraries.
 */
static long mirage_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long err = -ENOTTY;
	struct file *lower_file = mirage_vfs_lower_file(file);

	/* Redirect the call to the lower filesystem's compat_ioctl */
	if (lower_file->f_op && lower_file->f_op->compat_ioctl)
		err = lower_file->f_op->compat_ioctl(lower_file, cmd, arg);

	return err;
}
#endif

static int mirage_vfs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct file *lower_file = mirage_vfs_lower_file(file);
	int err;

	err = __generic_file_fsync(file, start, end, datasync);
	if (err) return err;

	return vfs_fsync_range(lower_file, start, end, datasync);
}

static int mirage_vfs_flush(struct file *file, fl_owner_t id)
{
	struct file *lower_file = mirage_vfs_lower_file(file);
	if (lower_file && lower_file->f_op && lower_file->f_op->flush)
		return lower_file->f_op->flush(lower_file, id);
	return 0;
}

static int mirage_vfs_fasync(int fd, struct file *file, int flag)
{
	struct file *lower_file = mirage_vfs_lower_file(file);
	if (lower_file->f_op && lower_file->f_op->fasync)
		return lower_file->f_op->fasync(fd, lower_file, flag);
	return 0;
}

#ifdef HAVE_LEGACY_IO
/* * mirage_vfs_read: The old read for Kernel < 3.16.
 */
static ssize_t mirage_vfs_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	/* * Forward read to the lower filesystem.
	 * We intentionally omit fsstack_copy_attr_atime.
	 * Updating metadata on every read chunk thrashes the CPU cache.
	 * getattr() will fetch the correct times directly from disk when requested.
	 */
	return vfs_read(mirage_vfs_lower_file(file), buf, count, ppos);
}

static ssize_t mirage_vfs_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	/* Forward write to the lower filesystem */
	return vfs_write(mirage_vfs_lower_file(file), buf, count, ppos);
}
#else
/* * mirage_vfs_read_iter: modern read interface.
 */
static ssize_t mirage_vfs_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	ssize_t err;
	struct file *file = iocb->ki_filp;
	struct file *lower_file = mirage_lower_file(file);
	struct file *saved_filp;

	if (unlikely(!lower_file->f_op->read_iter))
		return -EINVAL;

	/* Temporarily switch the control block's file context */
	saved_filp = iocb->ki_filp;
	iocb->ki_filp = lower_file;
	
	err = lower_file->f_op->read_iter(iocb, iter);
	
	/* Restore the original file context */
	iocb->ki_filp = saved_filp;

	/* Metadata sync removed */
	return err;
}

static ssize_t mirage_vfs_write_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	ssize_t err;
	struct file *file = iocb->ki_filp;
	struct file *lower_file = mirage_vfs_lower_file(file);
	struct file *saved_filp;

	if (unlikely(!lower_file->f_op->write_iter))
		return -EINVAL;

	/* Temporarily switch the control block's file context */
	saved_filp = iocb->ki_filp;
	iocb->ki_filp = lower_file;

	err = lower_file->f_op->write_iter(iocb, iter);
	
	/* Restore the original file context */
	iocb->ki_filp = saved_filp;

	/* * The Linux VFS naturally handles the i_size update of the upper 
	 * virtual inode during write calls. Manual fsstack_copy is redundant.
	 */
	return err;
}
#endif

struct mirage_readdir_data {
    struct dir_context ctx;
    struct mirage_sb_info *sbi;
    struct mirage_file_info *mfi;
	struct mirage_inode_info *mii;
};

/* * mirage_vfs_filldir_cache: Fills the in-memory cache of directories.
 */
static int mirage_vfs_filldir_cache(struct dir_context *ctx, const char *name, int namelen,
                           loff_t offset, u64 ino, unsigned int d_type)
{
    struct mirage_readdir_data *buf = container_of(ctx, struct nomount_readdir_data, ctx);
    struct mirage_sb_info *sbi = buf->sbi;
    struct mirage_inode_info *mii = buf->mii;
	struct mirage_dirent *md;
	u32 hash_val;

	/* Calculate hash for fast lookup */
	hash_val = full_name_hash(NULL, name, namelen);

	/* O(1) deduplication check across branches */
	hash_for_each_possible(mii->dirent_hashtable, md, hash, hash_val) {
		if (md->len == namelen && memcmp(md->name, name, namelen) == 0) {
			return 0; /* Already in cache, skip */
		}
	}

    /* Check if this is the entry we are targeting for injection */
    if (unlikely(sbi->has_inject && hash_val == sbi->inject_name_hash && 
	namelen == sbi->inject_name_len && memcmp(name, sbi->inject_name, namelen) == 0)) {
        
        if (buf->mfi) {
            buf->mfi->ghost_emitted = true;
        }
        /* Replace inode with the one from the injected file */
        ino = d_inode(sbi->inject_path.dentry)->i_ino;
        d_type = DT_REG; 
    }

	/* Allocation from the kmem_cache for improved performance */
	md = kmem_cache_alloc(mirage_dirent_cachep, GFP_KERNEL);
    if (unlikely(!md))
        return -ENOMEM;

    memcpy(md->name, name, namelen);
    md->name[namelen] = '\0';

	md->len = namelen;
	md->ino = ino;
	md->d_type = d_type;
		
	hash_add(mii->dirent_hashtable, &md->hash, hash_val);
	list_add_tail(&md->list, &mii->dirents_list);

    return 0;
}

#if defined(HAVE_ITERATE_SHARED) || defined(HAVE_ITERATE)
int mirage_vfs_iterate(struct file *file, struct dir_context *ctx)
{
    struct mirage_sb_info *sbi = mirage_sb(file->f_inode->i_sb);
    struct mirage_file_info *mfi = mirage_file(file);
	struct mirage_inode_info *mii = mirage_inode(file->f_inode);
    int i, emit_idx = 0, err = 0;
    bool is_root = (file->f_path.dentry == file->f_inode->i_sb->s_root);
	struct mirage_dirent *md, *tmp;
	u64 current_version = 0;

        if (mfi->num_lower_files == 1 && (!is_root || !sbi->has_inject)) {
		struct file *lower_file = mfi->lower_files[0];

		/* Align physical file position with virtual context */
		lower_file->f_pos = ctx->pos;
#ifdef HAVE_ITERATE_SHARED
		err = iterate_dir(lower_file, ctx);
#else
		if (!lower_file->f_op->iterate)
			return -ENOTDIR;
		err = lower_file->f_op->iterate(lower_file, ctx);
#endif
		/* Sync virtual position back */
		file->f_pos = ctx->pos;
		return err;
	}

	mutex_lock(&mii->readdir_mutex);

        if (ctx->pos == 0) {
	        /* Check invalidation based on lower files versions/mtimes */
	        for (i = 0; i < nfi->num_lower_files; i++) {
		        struct inode *lower_inode = file_inode(mfi->lower_files[i]);
		        current_version += mirage_get_mtime(lower_inode);
		        current_version += mirage_query_iversion(lower_inode);
	        }

	        if (mii->cache_populated && mii->cache_version != current_version) {
		        mii->cache_populated = false; /* Invalidate */
	        }
	}

	/* * Populate cache if starting from the beginning.
     * If ctx->pos == 0 but the ghost has already been issued or the list is not empty,
     * means that the user restarted reading from the same opened directory.
     * We don't read the disk again, we use the RAM.
     */
	if (ctx->pos == 0 && !mii->cache_populated) {
		list_for_each_entry_safe(md, tmp, &mii->dirents_list, list) {
			hash_del(&md->hash);
			list_del(&md->list);
			kfree(md);
		}
		hash_init(mii->dirent_hashtable);
		mfi->ghost_emitted = false;

		/* Read all lower branches into memory to deduplicate safely */
		for (i = 0; i < mfi->num_lower_files; i++) {
			struct mirage_readdir_data buf;
			struct file *lower_file = mfi->lower_files[i];

			lower_file->f_pos = 0; /* Always scan full directory from start */
			buf.ctx.actor = mirage_vfs_filldir_cache;
			buf.ctx.pos = 0;
			buf.sbi = sbi;
			buf.nfi = mfi;
			buf.nii = mii;

#ifdef HAVE_ITERATE_SHARED
			err = iterate_dir(lower_file, &buf.ctx);
#else
			if (!lower_file->f_op->iterate) {
				err = -ENOTDIR;
				break;
			}
			err = lower_file->f_op->iterate(lower_file, &buf.ctx);
#endif
			if (err < 0) break;
		}

		/* Inject ghost file if it was missing in the cache */
		if (err >= 0 && is_root && sbi->has_inject && !mfi->ghost_emitted) {
			u64 fake_ino = d_inode(sbi->inject_path.dentry)->i_ino;
			
			md = kmalloc(sizeof(struct mirage_dirent) + sbi->inject_name_len + 1, GFP_KERNEL);
			if (!md) {
				err = -ENOMEM;
			} else {
                memcpy(md->name, sbi->inject_name, sbi->inject_name_len);
                md->name[sbi->inject_name_len] = '\0';
				
				md->len = sbi->inject_name_len;
				md->ino = fake_ino;
				md->d_type = DT_REG;
					
				hash_add(mii->dirent_hashtable, &md->hash, sbi->inject_name_hash);
				list_add_tail(&md->list, &mii->dirents_list);
				mfi->ghost_emitted = true;
			}
		}

		if (err >= 0) {
			mii->cache_populated = true;
			mii->cache_version = current_version;
		}
	}

	if (unlikely(err < 0))
		goto out_unlock;

	/* Emit entries sequentially from cache using simple integer pos */
	list_for_each_entry(md, &mii->dirents_list, list) {
		if (emit_idx >= ctx->pos) {
			if (!dir_emit(ctx, md->name, md->len, md->ino, md->d_type)) {
				break; /* User buffer is full, pause here */
			}
			ctx->pos++;
		}
		emit_idx++;
	}

	file->f_pos = ctx->pos;

out_unlock:
	mutex_unlock(&mii->readdir_mutex);
    return err;
}
#else
/* * Ultra-Legacy kernels ( < 3.11) use the old readdir.
 */
int mirage_vfs_readdir(struct file *file, void *dirent, filldir_t filldir)
{
	/* Too complex to cleanly support unioning on legacy without custom state tracking.
     * We fallback to single layer for ancient kernels */
    struct file *lower_file = mirage_vfs_lower_file(file);
    struct nomount_sb_info *sbi = mirage_sb(file->f_inode->i_sb);
    struct nomount_file_info *mfi = mirage_file(file);
    int err;

    if (!lower_file->f_op->readdir)
        return -ENOTDIR;

    /* For legacy, we do a simple pass-through first */
    err = lower_file->f_op->readdir(lower_file, dirent, filldir);
    file->f_pos = lower_file->f_pos;

    /* If it's the root and we haven't emitted the ghost, do it now */
    if (err >= 0 && (file->f_path.dentry == file->f_inode->i_sb->s_root) &&
        sbi->has_inject && !mfi->ghost_emitted) {
        
        u64 fake_ino = d_inode(sbi->inject_path.dentry)->i_ino;
        
        /* Call the legacy filldir callback manually */
        if (filldir(dirent, sbi->inject_name, sbi->inject_name_len, 
                    file->f_pos, fake_ino, DT_REG) >= 0) {
            mfi->ghost_emitted = true;
            file->f_pos++;
        }
    }
    return err;
}
#endif

/* File operations for regular files */
const struct file_operations mirage_main_fops = {
    .open           = mirage_vfs_open,
    .release        = mirage_vfs_release,
    .llseek         = generic_file_llseek,
    .mmap           = mirage_vfs_mmap,
    .unlocked_ioctl = mirage_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = mirage_compat_ioctl,
#endif
    .fsync          = mirage_vfs_fsync,
    .flush          = mirage_vfs_flush,
    .fasync         = mirage_vfs_fasync,
#ifdef HAVE_LEGACY_IO
    .read           = mirage_vfs_read,
    .write          = mirage_vfs_write,
#else
    .read_iter      = mirage_vfs_read_iter,
    .write_iter     = mirage_vfs_write_iter,
#endif
};

const struct file_operations mirage_dir_fops = {
    .open           = mirage_vfs_open,
    .release        = mirage_vfs_release,
    .llseek         = generic_file_llseek,
    .unlocked_ioctl = mirage_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = mirage_compat_ioctl,
#endif
    .fsync          = mirage_vfs_fsync,
    .flush          = mirage_vfs_flush,
    .fasync         = mirage_vfs_fasync,
#ifdef HAVE_ITERATE_SHARED
    .iterate_shared = mirage_vfs_iterate,
#elif defined(HAVE_ITERATE)
    .iterate        = mirage_vfs_iterate,
#else
	.readdir	    = mirage_vfs_readdir,
#endif
};

