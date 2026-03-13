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
	struct path lower_path;
	struct nomount_file_info *info;

	/* Allocate private storage for lower file reference */
	info = kzalloc(sizeof(struct nomount_file_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
 
	info->ghost_emitted = false;

	nomount_get_lower_path(file->f_path.dentry, &lower_path);
	lower_file = dentry_open(&lower_path, file->f_flags, current_cred());
	path_put(&lower_path);

	if (IS_ERR(lower_file)) {
		err = PTR_ERR(lower_file);
		kfree(info);
	} else {
		info->lower_file = lower_file;
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

	if (info) {
		if (info->lower_file) {
			fput(info->lower_file);
			info->lower_file = NULL;
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
};

/* * nomount_filldir: Intercepts entries from the lower filesystem.
 */
static int nomount_filldir(struct dir_context *ctx, const char *name, int namelen,
                           loff_t offset, u64 ino, unsigned int d_type)
{
    struct nomount_readdir_data *buf = container_of(ctx, struct nomount_readdir_data, ctx);
    struct nomount_sb_info *sbi = buf->sbi;
    struct nomount_file_info *nfi = buf->nfi;

    /* Check if this is the entry we are targeting for injection */
    if (sbi->has_inject && namelen == strlen(sbi->inject_name) &&
        strncmp(name, sbi->inject_name, namelen) == 0) {
        
        nfi->ghost_emitted = true;
        /* Replace inode with the one from the injected file */
        ino = d_inode(sbi->inject_path.dentry)->i_ino;
        d_type = DT_REG; 
    }

    /* Pass the entry back to the original VFS actor */
    return buf->orig_ctx->actor(buf->orig_ctx, name, namelen, offset, ino, d_type);
}

#if defined(HAVE_ITERATE_SHARED) || defined(HAVE_ITERATE)
int nomount_iterate(struct file *file, struct dir_context *ctx)
{
    struct file *lower_file = nomount_lower_file(file);
    struct nomount_sb_info *sbi = NOMOUNT_SB(file->f_inode->i_sb);
    struct nomount_file_info *nfi = NOMOUNT_F(file);
    int err;
    bool is_root = (file->f_path.dentry == file->f_inode->i_sb->s_root);

    /* If no injection is active or we are not in the root dir, pass-through */
    if (!is_root || !sbi->has_inject) {
#ifdef HAVE_ITERATE_SHARED
        err = iterate_dir(lower_file, ctx);
#else
        if (!lower_file->f_op->iterate) return -ENOTDIR;
        err = lower_file->f_op->iterate(lower_file, ctx);
#endif
        file->f_pos = lower_file->f_pos;
        return err;
    }

    /* Prepare the interception buffer */
    struct nomount_readdir_data buf = {
        .ctx.actor = nomount_filldir,
        .ctx.pos = ctx->pos,
        .orig_ctx = ctx,
        .sbi = sbi,
        .nfi = nfi
    };

    /* Execute the iteration on the lower filesystem using our actor */
#ifdef HAVE_ITERATE_SHARED
    err = iterate_dir(lower_file, &buf.ctx);
#else
    err = lower_file->f_op->iterate(lower_file, &buf.ctx);
#endif

    /* Sync positions back to the original context and file */
    ctx->pos = buf.ctx.pos;
    file->f_pos = lower_file->f_pos;

    if (err >= 0 && !nfi->ghost_emitted) {
        u64 fake_ino = d_inode(sbi->inject_path.dentry)->i_ino;
        
        if (dir_emit(ctx, sbi->inject_name, strlen(sbi->inject_name), fake_ino, DT_REG)) {
            nfi->ghost_emitted = true;
            ctx->pos++;
            file->f_pos = ctx->pos;
        }
    }
    return err;
}
#else
/* * Ultra-Legacy kernels ( < 3.11) use the old readdir.
 */
int nomount_readdir(struct file *file, void *dirent, filldir_t filldir)
{
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
