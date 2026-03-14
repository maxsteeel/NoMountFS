#
# Makefile for NoMountFS
#

obj-$(CONFIG_NOMOUNT_FS) += nomountfs.o

nomountfs-objs := main.o super.o inode.o file.o lookup.o dentry.o mmap.o

# Detect if clone_private_mount is exported by this kernel.
# If not, we fall back to a kallsyms lookup at runtime.
# The grep checks the kernel's symbol table built into the source tree.
ifeq ($(shell grep -c "clone_private_mount" \
	$(srctree)/fs/namespace.c 2>/dev/null || echo 0), 0)
	ccflags-y += -DNOMOUNT_NO_CLONE_PRIVATE_MOUNT
endif

