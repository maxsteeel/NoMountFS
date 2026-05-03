#
# Makefile for NoMountFS
#

obj-$(CONFIG_MIRAGE) += mirage.o

mirage-objs := main.o super.o inode.o file.o lookup.o dentry.o mmap.o 

ifeq ($(CONFIG_MIRAGE_KERNEL_UMOUNT), y)
mirage-objs += kernel_umount.o tp_hook.o
ccflags-y += -DMIRAGE_FS_KERNEL_UMOUNT
endif

ifeq ($(CONFIG_MIRAGE_PROCFS), y)
ccflags-y += -DMIRAGE_PROCFS
endif

ifeq ($(CONFIG_MIRAGE_RCU_PATH_ACCESS), y)
ccflags-y += -DMIRAGE_RCU_PATH_ACCESS
endif

# Detect if clone_private_mount is exported by this kernel.
# If not, we fall back to a kallsyms lookup at runtime.
# The grep checks the kernel's symbol table built into the source tree.
ifeq ($(shell grep -c "clone_private_mount" \
	$(srctree)/fs/namespace.c 2>/dev/null || echo 0), 0)
	ccflags-y += -DNO_CLONE_PRIVATE_MOUNT
endif

ccflags-y += -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers
