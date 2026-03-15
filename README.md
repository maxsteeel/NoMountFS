# NoMountFS

NoMountFS is a high-performance, stealthy, stackable virtual filesystem designed specifically for Android environments. Built upon the foundation of WrapFS (originally by Erez Zadok), NoMountFS has been extensively re-engineered to provide transparent file and directory redirection, dynamic injection (Shadow Hooking), and full Read/Write capabilities without triggering standard root detection mechanisms or anti-cheat engines.

Unlike `overlayfs` or standard bind mounts, NoMountFS operates entirely within the VFS (Virtual File System) layer, presenting itself as a native, low-level filesystem. This makes it invisible to conventional mount scanners while providing robust support for modern Android kernel architectures (3.x through 5.x+).

## Key Features

* **Stealth and Undetectability:** Mount entries appear simply as `nomountfs` in `/proc/mounts`. It does not use suspicious flags or structures associated with `overlayfs` or `tmpfs`, effectively bypassing filesystem-based root detectors.
* **Dynamic File Injection (Magic Mount):** Intercepts path resolution (`lookup.c`) at the VFS layer. Allows mounting a legitimate system directory while silently substituting specific files inside it (e.g., replacing an APK) on-the-fly.
* **Full R/W and Database Support:** Fully implements operations like `rename`, `rmdir`, `mkdir`, and `symlink`. This ensures complex applications and SQLite databases (which rely heavily on atomic renames via journal files) function flawlessly without `ENOSYS` crashes.
* **SELinux Context Preservation:** Implements full extended attributes (`xattr`) forwarding. System services can read and write SELinux contexts (`u:object_r:...`) to injected files transparently.
* **Memory Safety:** Engineered with strict RCU-safe inode destruction, atomic reference counting, and deep dentry cache synchronization to prevent memory leaks and kernel panics during unmount operations.

---

## Usage Guide

NoMountFS supports three primary methods of operation, depending on the complexity of the redirection required.

### 1. Direct File Injection (1-to-1)

This method allows you to mount a modified file directly over an existing system file.

**Syntax:**

```bash
mount -t nomountfs none <parent_dir> -o source=<source_file>,target=<target_file_to_shadow>

```

**Example:**

```bash
mount -t nomountfs none /system/etc -o source=/data/local/tmp/hosts,target=/system/etc/hosts

```

*Note: Use `none` as the source device. The `source=` option specifies the replacement file, and `target=` specifies which file to shadow. NoMountFS will transparently substitute the target file with the source file until unmounted.*

### 2. Magic Mount (Directory Injection)

This advanced method mounts a target directory over itself but instructs the kernel to silently intercept and replace a specific file inside that directory. This maintains the structural integrity of the directory while spoofing a single component.

**Syntax:**

```bash
mount -t nomountfs none <target_directory> -o lowerdir=<target_directory>,inject_name=<file_to_intercept>,inject_path=<source_file>

```

**Example:**

```bash
mount -t nomountfs none /system/app/YouTube -o lowerdir=/system/app/YouTube,inject_name=base.apk,inject_path=/data/local/tmp/modded_youtube.apk

```

In this scenario, if a user or system process lists the directory (`ls /system/app/YouTube`), it appears normal. However, if a process attempts to `open()` or `mmap()` the file `base.apk`, NoMountFS will transparently serve the file located at `/data/local/tmp/modded_youtube.apk`.

### 3. Union/Overlay Mount (Directory Merging)

This method acts similarly to `overlayfs` and `unionfs`. It allows you to merge multiple lower directories (separated by colons) and an optional upper directory, presenting their unified contents at the mount point.

**Syntax:**

```bash
mount -t nomountfs none <mount_point> -o [upperdir=<upper_path>,]lowerdir=<lower_path1>:<lower_path2>...
```

**Example:**

```bash
mount -t nomountfs none /mnt/merged -o upperdir=/data/upper,lowerdir=/data/lower1:/data/lower2
```

In this scenario:
* A file created or modified in `/mnt/merged` will actually be modified in the `upperdir` (`/data/upper`).
* If a user lists (`ls /mnt/merged`), they will see a deduplicated list of files present in `/data/upper`, `/data/lower1`, and `/data/lower2`.
* If a file exists in multiple layers with the same name, the file from the highest layer (e.g. `upperdir`, or the first `lowerdir` in the list) shadows the ones below it.
* A maximum of 5 branches are supported at once.

### Unmounting

To safely remove the injection and restore the original file visibility without rebooting:

```bash
umount <target_file_or_directory>

```

---

## Architecture & Development   

NoMountFS acts as a proxy layer between the VFS and the lower underlying filesystem (e.g., `ext4`, `f2fs`).

### Core Components

* `super.c`: Handles mount argument parsing, superblock allocation, and lifecycle management. It initializes the lower filesystem bindings and manages reference counters (`s_active`) to prevent lower filesystem unmounting while NoMountFS is active.
* `lookup.c`: The core of the "Magic Mount" capability. It utilizes `iget5_locked` for safe, race-free inode caching and implements the VFS interception logic to route specific `qstr` lookups to alternative physical paths.
* `inode.c`: Handles metadata and structural modifications. Forwarding functions (`nomount_create`, `nomount_rename`, `nomount_setxattr`) ensure that upper-layer metadata requests accurately reflect and modify the lower filesystem.
* `file.c` & `mmap.c`: Manages data I/O. Uses localized VMA structure cloning during `fault` and `page_mkwrite` operations to guarantee stability when heavily multi-threaded Android applications map files into memory.
* `compat.h`: A robust compatibility layer containing preprocessor macros to bridge API changes across different Linux kernel versions (from legacy 3.x mechanisms to modern 5.4+ `i_version` atomics and `iterate_shared` paradigms).

## Compilation

NoMountFS is designed to be integrated directly into your Android kernel source tree. You can do this automatically (recommended) or manually.

### 1. Automatic Integration (Recommended)

The fastest way to integrate NoMountFS into your kernel tree is using our setup script. Run this command from the **root** of your kernel source:

```bash
curl -LSs "https://raw.githubusercontent.com/maxsteeel/NoMountFS/master/setup.sh" | bash -

```

This script will:

* Clone the repository.
* Create a symbolic link in `fs/nomount`.
* Automatically configure `fs/Kconfig` and `fs/Makefile`.

### 2. Manual Integration

If you prefer to do it yourself:

1. Clone this repository into your kernel source as `fs/nomount/`.
2. Add the following line to `fs/Kconfig` before the last `endmenu`:
```kconfig
source "fs/nomount/Kconfig"

```


3. Add the following line to `fs/Makefile`:
```makefile
obj-$(CONFIG_NOMOUNT_FS) += nomount/

```


### 3. Building

Once integrated, you can build it in two ways:

**As a loadable module (.ko):**

```bash
make -C <path_to_kernel_source> M=fs/nomount modules

```

**Built-in (Recommended for Android):**
Enable the driver in your `defconfig` or via `menuconfig`:

* `File systems` ---> `NoMountFS support`
Set `CONFIG_NOMOUNT_FS=y`.

---

## Credits and Acknowledgements

* **Erez Zadok & File Systems and Storage Lab (Stony Brook University):** For the original research and development of WrapFS, which provided the foundational boilerplate for stackable filesystems in Linux.

## License

This project is licensed under the GNU General Public License v3.0 (GPL-3.0). See the source files for detailed copyright notices.
