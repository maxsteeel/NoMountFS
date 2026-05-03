/*
 * Mirage: Memory mapping operations.
 */

#include "mirage.h"
#include "compat.h"

/* Address space operations are required for certain VFS checks (like O_DIRECT) */
static ssize_t mirage_direct_IO(struct kiocb *iocb, struct iov_iter *iter)
{
	/* We do not support direct IO natively, force fallback to buffered IO */
	return -EINVAL;
}

/* Address space operations are required for certain VFS checks */
const struct address_space_operations nomount_aops = {
	.direct_IO = mirage_direct_IO,
};

/*
 * mirage_vfs_mmap: Delegate memory mapping directly to the physical disk.
 *
 * Instead of intercepting page faults (which requires dangerous VMA 
 * cloning/memcpy and breaks on modern kernels), we completely hand over 
 * the VMA ownership to the physical underlying file. 
 */
int mirage_vfs_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct file *lower_file = mirage_lower_file(file);

	if (!lower_file->f_op->mmap)
		return -ENODEV;

	/* Drop the reference to our virtual nomountfs file in this VMA. */
	if (vma->vm_file)
		fput(vma->vm_file);
		
	/* * Replace it with the physical file and increase its reference count.
	 * From this millisecond forward, the Kernel Memory Manager will bypass
	 * Mirage entirely and talk directly to the real storage hardware.
	 */
	vma->vm_file = get_file(lower_file);

	/* Call the lower filesystem's mmap natively */
	return lower_file->f_op->mmap(lower_file, vma);
}
