/*
 * NoMountFS: Memory mapping operations.
 * Enhanced with stacked VMA logic for Android stability.
 */

#include "nomount.h"
#include "compat.h"

/* * nomount_fault: Handles page faults by redirection.
 * Using a local VMA copy prevents race conditions in multi-threaded apps.
 * Note: nomount_lower_file returns the topmost branch file (lower_files[0]),
 * which is exactly what we want since files (non-directories) are only opened
 * from their topmost layer.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
static vm_fault_t nomount_fault(struct vm_fault *vmf)
#else
static int nomount_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
	/* For older kernels, vma is passed directly */
#else
	struct vm_area_struct *vma = vmf->vma;
#endif
	struct file *file = vma->vm_file;
	struct file *lower_file;
	const struct vm_operations_struct *lower_vm_ops;
	struct vm_area_struct lower_vma;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
	vm_fault_t ret;
#else
	int ret;
#endif

	/* Clone VMA to protect the original context */
	memcpy(&lower_vma, vma, sizeof(struct vm_area_struct));
	lower_vm_ops = NOMOUNT_F(file)->lower_vm_ops;
	lower_file = nomount_lower_file(file);

	BUG_ON(!lower_vm_ops);

	/* Switch file to the real underlying object */
	lower_vma.vm_file = lower_file;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
	/* In modern kernels, we must temporarily point vmf->vma to our clone */
	vmf->vma = &lower_vma;
	ret = lower_vm_ops->fault(vmf);
	vmf->vma = vma; /* Restore original context */
#else
	ret = lower_vm_ops->fault(&lower_vma, vmf);
#endif

	return ret;
}

/* * nomount_page_mkwrite: Called when a memory page is about to become writable.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
static vm_fault_t nomount_page_mkwrite(struct vm_fault *vmf)
#else
static int nomount_page_mkwrite(struct vm_area_struct *vma, struct vm_fault *vmf)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
	struct vm_area_struct *vma = vmf->vma;
#else
	struct vm_area_struct *vma = vmf->vma;
#endif
	struct file *file = vma->vm_file;
	struct file *lower_file;
	const struct vm_operations_struct *lower_vm_ops;
	struct vm_area_struct lower_vma;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
	vm_fault_t ret = VM_FAULT_NOPAGE;
#else
	int ret = 0;
#endif

	memcpy(&lower_vma, vma, sizeof(struct vm_area_struct));
	lower_vm_ops = NOMOUNT_F(file)->lower_vm_ops;
	lower_file = nomount_lower_file(file);

	if (!lower_vm_ops || !lower_vm_ops->page_mkwrite)
		return ret;

	lower_vma.vm_file = lower_file;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
	vmf->vma = &lower_vma;
	ret = lower_vm_ops->page_mkwrite(vmf);
	vmf->vma = vma;
#else
	ret = lower_vm_ops->page_mkwrite(&lower_vma, vmf);
#endif

	return ret;
}

static ssize_t nomount_direct_IO(struct kiocb *iocb, struct iov_iter *iter)
{
	/*
	 * This function should never be called directly.  We need it
	 * to exist, to get past a check in open_check_o_direct(),
	 * which is called from do_last().
	 */
	return -EINVAL;
}

/* Address space operations are required for certain VFS checks */
const struct address_space_operations nomount_aops = {
	.direct_IO = nomount_direct_IO,
};

const struct vm_operations_struct nomount_vm_ops = {
	.fault		= nomount_fault,
	.page_mkwrite	= nomount_page_mkwrite,
};

/* * nomount_mmap: Establishes the memory mapping.
 * Only the topmost shadowed file (lower_files[0]) is mapped into memory.
 */
int nomount_mmap(struct file *file, struct vm_area_struct *vma)
{
	int err = 0;
	struct file *lower_file = nomount_lower_file(file);

	if (!lower_file->f_op->mmap)
		return -ENODEV;

	/*1. Call the lower mmap ALWAYS to initialize the VMA correctly */
	vma->vm_file = lower_file;
	err = lower_file->f_op->mmap(lower_file, vma);
	vma->vm_file = file;

	if (err) 
		return err;

	/* Save original operations only the first time */
	if (!NOMOUNT_F(file)->lower_vm_ops)
		NOMOUNT_F(file)->lower_vm_ops = vma->vm_ops;

	/* 2. Setup our stacked operations */
	vma->vm_ops = &nomount_vm_ops;
	
	/* 3. Link address space operations */
	file->f_mapping->a_ops = &nomount_aops;

	return err;
}
