/*
 * NoMountFS: File operations
 * Handling read, write and open.
 */

#include "nomount.h"
#include "compat.h"
#include <linux/uio.h>

/* * nomount_open: called when a file is being opened.
 * We must open the lower file and store its reference.
 */
static int nomount_open(struct inode *inode, struct file *file)
{
	int err = 0;
	struct file *lower_file = NULL;
	struct path lower_paths[NOMOUNT_MAX_BRANCHES];
	int num_lower_paths;
	struct nomount_file_info *info;
	int i;

	/* Allocate private storage for lower file reference */
	info = kzalloc(sizeof(struct nomount_file_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
 
	info->ghost_emitted = false;
	hash_init(info->dirent_hashtable);
	INIT_LIST_HEAD(&info->dirents_list);
	mutex_init(&info->readdir_mutex);
	info->num_lower_files = 0;

	num_lower_paths = nomount_get_all_lower_paths(file->f_path.dentry, lower_paths);
	
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

	nomount_put_all_lower_paths(file->f_path.dentry, lower_paths, num_lower_paths);

	if (err && info->num_lower_files == 0) {
		kfree(info);
	} else if (err) {
		/* Failed to open all directories, close what we opened */
		for (i = 0; i < info->num_lower_files; i++) {
			fput(info->lower_files[i]);
		}
		kfree(info);
	} else {
		file->private_data = info;
		fsstack_copy_attr_all(inode, nomount_lower_inode(inode));
	}

	return err;
}

/* * nomount_release: close the lower file when the virtual one is closed.
 */
static int nomount_release(struct inode *inode, struct file *file)
{
	struct nomount_file_info *info = NOMOUNT_F(file);
	struct nomount_dirent *nd, *tmp;
	int i;

	if (info) {
		for (i = 0; i < info->num_lower_files; i++) {
			if (info->lower_files[i]) {
				fput(info->lower_files[i]);
			}
		}
		
		/* Clean up cached directory entries */
		list_for_each_entry_safe(nd, tmp, &info->dirents_list, list) {
			hash_del(&nd->hash);
			list_del(&nd->list);
			kfree(nd->name);
			kfree(nd);
		}
		
		kfree(info);
		file->private_data = NULL;
	}
	return 0;
}

/* * IOCTL: Critical for hardware-specific storage commands in Android.
 */
static long nomount_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct file *lower_file = nomount_lower_file(file);
    long err = -ENOTTY;

    if (lower_file->f_op && lower_file->f_op->unlocked_ioctl)
        err = lower_file->f_op->unlocked_ioctl(lower_file, cmd, arg);

    /* If ioctl changed size/attributes, sync them back */ 
    if (!err)
        fsstack_copy_attr_all(file_inode(file), file_inode(lower_file));

    return err;
}

#ifdef CONFIG_COMPAT
/* * compat_ioctl: Handles 32-bit ioctl calls on 64-bit kernels.
 * Essential for Android compatibility with older apps/libraries.
 */
static long nomount_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long err = -ENOTTY;
	struct file *lower_file = nomount_lower_file(file);

	/* Redirect the call to the lower filesystem's compat_ioctl */
	if (lower_file->f_op && lower_file->f_op->compat_ioctl)
		err = lower_file->f_op->compat_ioctl(lower_file, cmd, arg);

	return err;
}
#endif

static int nomount_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
    struct file *lower_file = nomount_lower_file(file);
    struct dentry *dentry = file->f_path.dentry;
    int err;

    err = __generic_file_fsync(file, start, end, datasync);
    if (err) return err;

    err = vfs_fsync_range(lower_file, start, end, datasync);

    if (!err)
        fsstack_copy_attr_all(d_inode(dentry), file_inode(lower_file));

    return err;
}

static int nomount_flush(struct file *file, fl_owner_t id)
{
    struct file *lower_file = nomount_lower_file(file);
    if (lower_file && lower_file->f_op && lower_file->f_op->flush)
        return lower_file->f_op->flush(lower_file, id);
    return 0;
}

static int nomount_fasync(int fd, struct file *file, int flag)
{
    struct file *lower_file = nomount_lower_file(file);
    if (lower_file->f_op && lower_file->f_op->fasync)
        return lower_file->f_op->fasync(fd, lower_file, flag);
    return 0;
}

#ifdef HAVE_LEGACY_IO
/* * nomount_read: The old read for Kernel < 3.16.
 */
static ssize_t nomount_read(struct file *file, char __user *buf,
			   size_t count, loff_t *ppos)
{
	ssize_t err;
	struct file *lower_file = nomount_lower_file(file);
	struct dentry *dentry = file->f_path.dentry;

	/* Forward read to the lower filesystem */
	err = vfs_read(lower_file, buf, count, ppos);

	/* Update virtual inode access time from lower inode */
	if (err >= 0)
		fsstack_copy_attr_atime(d_inode(dentry), file_inode(lower_file));

	return err;
}

static ssize_t nomount_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	ssize_t err;
	struct file *lower_file = nomount_lower_file(file);
	struct dentry *dentry = file->f_path.dentry;

	/* Forward write to the lower filesystem */
	err = vfs_write(lower_file, buf, count, ppos);

	/* Copy inode size and times so VFS doesn't detect inconsistencies */
	if (err >= 0) {
		fsstack_copy_inode_size(d_inode(dentry), file_inode(lower_file));
		fsstack_copy_attr_times(d_inode(dentry), file_inode(lower_file));
	}

	return err;
}
#else
/* * nomount_read_iter: modern read interface.
 */
static ssize_t nomount_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	ssize_t err;
	struct file *file = iocb->ki_filp;
	struct file *lower_file = nomount_lower_file(file);
	struct kiocb lower_iocb;

	if (!lower_file->f_op->read_iter)
		return -EINVAL;

	/* Clone the control block and switch to lower file context */
	memcpy(&lower_iocb, iocb, sizeof(struct kiocb));
	lower_iocb.ki_filp = lower_file;
	
	err = lower_file->f_op->read_iter(&lower_iocb, iter);
	iocb->ki_pos = lower_iocb.ki_pos;

	/* Update times if read succeeded */
	if (err >= 0 || err == -EIOCBQUEUED)
		fsstack_copy_attr_atime(d_inode(file->f_path.dentry), file_inode(lower_file));

	return err;
}

static ssize_t nomount_write_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	ssize_t err;
	struct file *file = iocb->ki_filp;
	struct file *lower_file = nomount_lower_file(file);
	struct kiocb lower_iocb;

	if (!lower_file->f_op->write_iter)
		return -EINVAL;

	memcpy(&lower_iocb, iocb, sizeof(struct kiocb));
	lower_iocb.ki_filp = lower_file;

	err = lower_file->f_op->write_iter(&lower_iocb, iter);
	iocb->ki_pos = lower_iocb.ki_pos;

	/* Sync size and times after write */
	if (err >= 0 || err == -EIOCBQUEUED) {
		fsstack_copy_inode_size(d_inode(file->f_path.dentry), file_inode(lower_file));
		fsstack_copy_attr_times(d_inode(file->f_path.dentry), file_inode(lower_file));
	}

	return err;
}
#endif

struct nomount_readdir_data {
    struct dir_context ctx;
    struct dir_context *orig_ctx;
    struct nomount_sb_info *sbi;
    struct nomount_file_info *nfi;
	int branch_idx;
};

/* * nomount_filldir_cache: Fills the in-memory cache of directories.
 */
static int nomount_filldir_cache(struct dir_context *ctx, const char *name, int namelen,
                           loff_t offset, u64 ino, unsigned int d_type)
{
    struct nomount_readdir_data *buf = container_of(ctx, struct nomount_readdir_data, ctx);
    struct nomount_sb_info *sbi = buf->sbi;
    struct nomount_file_info *nfi = buf->nfi;
	struct nomount_dirent *nd;
	u32 hash_val;

	/* Calculate hash for fast lookup */
	hash_val = full_name_hash(NULL, name, namelen);

	/* O(1) deduplication check across branches */
	hash_for_each_possible(nfi->dirent_hashtable, nd, hash, hash_val) {
		if (nd->len == namelen && strncmp(nd->name, name, namelen) == 0) {
			return 0; /* Already in cache, skip */
		}
	}

    /* Check if this is the entry we are targeting for injection */
    if (sbi->has_inject && namelen == strlen(sbi->inject_name) &&
        strncmp(name, sbi->inject_name, namelen) == 0) {
        
        nfi->ghost_emitted = true;
        /* Replace inode with the one from the injected file */
        ino = d_inode(sbi->inject_path.dentry)->i_ino;
        d_type = DT_REG; 
    }

	/* Add to deduplication hash table and ordered list */
	nd = kmalloc(sizeof(*nd), GFP_KERNEL);
	if (nd) {
		nd->name = kstrndup(name, namelen, GFP_KERNEL);
		nd->len = namelen;
		nd->ino = ino;
		nd->d_type = d_type;
		
		hash_add(nfi->dirent_hashtable, &nd->hash, hash_val);
		list_add_tail(&nd->list, &nfi->dirents_list);
	}

    return 0;
}

#if defined(HAVE_ITERATE_SHARED) || defined(HAVE_ITERATE)
int nomount_iterate(struct file *file, struct dir_context *ctx)
{
    struct nomount_sb_info *sbi = NOMOUNT_SB(file->f_inode->i_sb);
    struct nomount_file_info *nfi = NOMOUNT_F(file);
    int err = 0;
    bool is_root = (file->f_path.dentry == file->f_inode->i_sb->s_root);
	int i;
	int emit_idx = 0;
	struct nomount_dirent *nd, *tmp;

	mutex_lock(&nfi->readdir_mutex);

	/* Populate cache if starting from the beginning */
	if (ctx->pos == 0) {
		list_for_each_entry_safe(nd, tmp, &nfi->dirents_list, list) {
			hash_del(&nd->hash);
			list_del(&nd->list);
			kfree(nd->name);
			kfree(nd);
		}
		hash_init(nfi->dirent_hashtable);
		nfi->ghost_emitted = false;

		/* Read all lower branches into memory to deduplicate safely */
		for (i = 0; i < nfi->num_lower_files; i++) {
			struct file *lower_file = nfi->lower_files[i];

			lower_file->f_pos = 0; /* Always scan full directory from start */

			struct nomount_readdir_data buf = {
				.ctx.actor = nomount_filldir_cache,
				.ctx.pos = 0,
				.orig_ctx = ctx,
				.sbi = sbi,
				.nfi = nfi,
				.branch_idx = i
			};

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
		if (err >= 0 && is_root && sbi->has_inject && !nfi->ghost_emitted) {
			u64 fake_ino = d_inode(sbi->inject_path.dentry)->i_ino;
			
			nd = kmalloc(sizeof(*nd), GFP_KERNEL);
			if (nd) {
				u32 ghost_hash = full_name_hash(NULL, sbi->inject_name, strlen(sbi->inject_name));
				
				nd->name = kstrdup(sbi->inject_name, GFP_KERNEL);
				nd->len = strlen(sbi->inject_name);
				nd->ino = fake_ino;
				nd->d_type = DT_REG;
				
				hash_add(nfi->dirent_hashtable, &nd->hash, ghost_hash);
				list_add_tail(&nd->list, &nfi->dirents_list);
			}
			nfi->ghost_emitted = true;
		}
	}

	/* Emit entries sequentially from cache using simple integer pos */
	list_for_each_entry(nd, &nfi->dirents_list, list) {
		if (emit_idx >= ctx->pos) {
			if (!dir_emit(ctx, nd->name, nd->len, nd->ino, nd->d_type)) {
				break; /* User buffer is full, pause here */
			}
			ctx->pos++;
		}
		emit_idx++;
	}

	file->f_pos = ctx->pos;

	mutex_unlock(&nfi->readdir_mutex);
    return err;
}
#else
/* * Ultra-Legacy kernels ( < 3.11) use the old readdir.
 */
int nomount_readdir(struct file *file, void *dirent, filldir_t filldir)
{
	/* Too complex to cleanly support unioning on legacy without custom state tracking.
     * We fallback to single layer for ancient kernels */
    struct file *lower_file = nomount_lower_file(file);
    struct nomount_sb_info *sbi = NOMOUNT_SB(file->f_inode->i_sb);
    struct nomount_file_info *nfi = NOMOUNT_F(file);
    int err;

    if (!lower_file->f_op->readdir)
        return -ENOTDIR;

    /* For legacy, we do a simple pass-through first */
    err = lower_file->f_op->readdir(lower_file, dirent, filldir);
    file->f_pos = lower_file->f_pos;

    /* If it's the root and we haven't emitted the ghost, do it now */
    if (err >= 0 && (file->f_path.dentry == file->f_inode->i_sb->s_root) &&
        sbi->has_inject && !nfi->ghost_emitted) {
        
        u64 fake_ino = d_inode(sbi->inject_path.dentry)->i_ino;
        
        /* Call the legacy filldir callback manually */
        if (filldir(dirent, sbi->inject_name, strlen(sbi->inject_name), 
                    file->f_pos, fake_ino, DT_REG) >= 0) {
            nfi->ghost_emitted = true;
            file->f_pos++;
        }
    }
    return err;
}
#endif

/* File operations for regular files */
const struct file_operations nomount_main_fops = {
    .open           = nomount_open,
    .release        = nomount_release,
    .llseek         = generic_file_llseek,
    .mmap           = nomount_mmap,
    .unlocked_ioctl = nomount_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = nomount_compat_ioctl,
#endif
    .fsync          = nomount_fsync,
    .flush          = nomount_flush,
    .fasync         = nomount_fasync,
#ifdef HAVE_LEGACY_IO
    .read           = nomount_read,
    .write          = nomount_write,
#else
    .read_iter      = nomount_read_iter,
    .write_iter     = nomount_write_iter,
#endif
};

const struct file_operations nomount_dir_fops = {
    .open           = nomount_open,
    .release        = nomount_release,
    .llseek         = generic_file_llseek,
    .unlocked_ioctl = nomount_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = nomount_compat_ioctl,
#endif
    .fsync          = nomount_fsync,
    .flush          = nomount_flush,
    .fasync         = nomount_fasync,
#ifdef HAVE_ITERATE_SHARED
    .iterate_shared = nomount_iterate,
#elif defined(HAVE_ITERATE)
    .iterate        = nomount_iterate,
#else
	.readdir	= nomount_readdir,
#endif
};
