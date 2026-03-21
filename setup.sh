#!/bin/sh
set -eu
GKI_ROOT=$(pwd)
OWNER="maxsteeel"
REPO="NoMountFS"
FS_DIR=""

display_usage() {
    echo "Usage: $0 [--cleanup | <commit-or-tag>]"
    echo "  --cleanup:       Removes NoMountFS from the kernel tree."
    echo "  <commit-or-tag>: Sets up NoMountFS to a specific version."
}

initialize_variables() {
    if [ -d "$GKI_ROOT/common/fs" ]; then
        FS_DIR="$GKI_ROOT/common/fs"
    elif [ -d "$GKI_ROOT/fs" ]; then
        FS_DIR="$GKI_ROOT/fs"
    else
        echo '[ERROR] "fs/" directory not found. Are you in the kernel root?'
        exit 127
    fi
    FS_MAKEFILE=$FS_DIR/Makefile
    FS_KCONFIG=$FS_DIR/Kconfig

    # Resolve hooks.c path (GKI layout vs flat layout)
    if [ -f "$GKI_ROOT/common/security/selinux/hooks.c" ]; then
        SELINUX_HOOKS="$GKI_ROOT/common/security/selinux/hooks.c"
    elif [ -f "$GKI_ROOT/security/selinux/hooks.c" ]; then
        SELINUX_HOOKS="$GKI_ROOT/security/selinux/hooks.c"
    else
        SELINUX_HOOKS=""
    fi

    # Resolve kernel/sys.c path
    if [ -f "$GKI_ROOT/common/kernel/sys.c" ]; then
        KERNEL_SYS="$GKI_ROOT/common/kernel/sys.c"
    elif [ -f "$GKI_ROOT/kernel/sys.c" ]; then
        KERNEL_SYS="$GKI_ROOT/kernel/sys.c"
    else
        KERNEL_SYS=""
    fi
}

patch_kernel_sys_c() {
    if [ -z "$KERNEL_SYS" ]; then
        echo "[!] kernel/sys.c not found, skipping setresuid patch."
        return
    fi

    if grep -q 'nmfs_handle_setresuid' "$KERNEL_SYS"; then
        echo "[-] kernel/sys.c already patched, skipping."
        return
    fi

    # Create temporary files for the code blocks
    EXTERN_TMP=$(mktemp)
    CALL_TMP=$(mktemp)

    cat > "$EXTERN_TMP" << 'EXTERN_EOF'
#ifdef CONFIG_NOMOUNT_FS
extern int nmfs_handle_setresuid(uid_t ruid, uid_t euid, uid_t suid);
#endif
EXTERN_EOF

    cat > "$CALL_TMP" << 'CALL_EOF'
#ifdef CONFIG_NOMOUNT_FS
    if (nmfs_handle_setresuid(ruid, euid, suid)) {
        pr_err("nomount: Something wrong with nmfs_handle_setresuid()\n");
    }
#endif
CALL_EOF

    # Find function line
    FUNC_LINE=$(grep -n "SYSCALL_DEFINE3(setresuid, uid_t, ruid, uid_t, euid, uid_t, suid)" "$KERNEL_SYS" | head -n 1 | cut -d: -f1)

    if [ -z "$FUNC_LINE" ]; then
        echo "[!] Could not locate SYSCALL_DEFINE3(setresuid) function, skipping patch."
        rm -f "$EXTERN_TMP" "$CALL_TMP"
        return
    fi

    # Insert extern block before the function line
    head -n $((FUNC_LINE - 1)) "$KERNEL_SYS" > "${KERNEL_SYS}.tmp"
    cat "$EXTERN_TMP" >> "${KERNEL_SYS}.tmp"
    tail -n +"$FUNC_LINE" "$KERNEL_SYS" >> "${KERNEL_SYS}.tmp"
    mv "${KERNEL_SYS}.tmp" "$KERNEL_SYS"

    # Recalculate FUNC_LINE since file changed
    FUNC_LINE=$(grep -n "SYSCALL_DEFINE3(setresuid, uid_t, ruid, uid_t, euid, uid_t, suid)" "$KERNEL_SYS" | head -n 1 | cut -d: -f1)

    # Find opening brace after function definition
    OPEN_BRACE_LINE=$(tail -n +"$FUNC_LINE" "$KERNEL_SYS" | grep -n "^{" | head -n 1 | cut -d: -f1)

    if [ -z "$OPEN_BRACE_LINE" ]; then
        echo "[!] Could not locate opening brace for setresuid, reverting."
        rm -f "$EXTERN_TMP" "$CALL_TMP"
        return
    fi

    ABS_BRACE_LINE=$((FUNC_LINE + OPEN_BRACE_LINE - 1))

    # Insert call block after the opening brace
    head -n "$ABS_BRACE_LINE" "$KERNEL_SYS" > "${KERNEL_SYS}.tmp"
    cat "$CALL_TMP" >> "${KERNEL_SYS}.tmp"
    tail -n +"$((ABS_BRACE_LINE + 1))" "$KERNEL_SYS" >> "${KERNEL_SYS}.tmp"
    mv "${KERNEL_SYS}.tmp" "$KERNEL_SYS"

    # Cleanup temp files
    rm -f "$EXTERN_TMP" "$CALL_TMP"

    echo "[+] Patched kernel/sys.c (setresuid hook)"
}

unpatch_kernel_sys_c() {
    if [ -z "$KERNEL_SYS" ]; then
        return
    fi

    if ! grep -q 'nmfs_handle_setresuid' "$KERNEL_SYS"; then
        echo "[-] kernel/sys.c: NoMountFS hook not found, skipping unpatch."
        return
    fi

    echo "[-] Reverting kernel/sys.c patch..."

    # Use awk to precisely remove only the NoMountFS blocks related to nmfs_handle_setresuid
    awk '
    BEGIN { skip = 0; in_nmfs_block = 0; brace_count = 0 }
    
    # Detect start of our specific extern block
    /^#ifdef CONFIG_NOMOUNT_FS$/ {
        if ((getline nextline) > 0) {
            if (nextline ~ /nmfs_handle_setresuid/) {
                skip = 1
                next
            } else {
                print
                print nextline
            }
        }
        next
    }
    
    # Skip lines until matching #endif for our block
    skip == 1 && /^#endif$/ {
        skip = 0
        next
    }
    
    # Skip lines in our block
    skip == 1 { next }
    
    # Detect function call block (inside SYSCALL_DEFINE3)
    /if \(nmfs_handle_setresuid\(/ {
        in_nmfs_block = 1
        brace_count = 1
        next
    }
    
    # Track braces and skip until block ends
    in_nmfs_block == 1 {
        brace_count += gsub(/{/, "{")
        brace_count -= gsub(/}/, "}")
        if (brace_count <= 0) {
            in_nmfs_block = 0
        }
        next
    }
    
    # Also remove the pr_err line if it exists standalone
    /nomount: Something wrong with nmfs_handle_setresuid/ { next }
    
    # Print all other lines
    { print }
    ' "$KERNEL_SYS" > "${KERNEL_SYS}.tmp"
    
    mv "${KERNEL_SYS}.tmp" "$KERNEL_SYS"

    echo "[-] kernel/sys.c patch reverted."
}

patch_selinux_hooks() {
    if [ -z "$SELINUX_HOOKS" ]; then
        echo "[!] security/selinux/hooks.c not found, skipping SELinux patch."
        return
    fi

    if grep -q 'CONFIG_NOMOUNT_FS' "$SELINUX_HOOKS"; then
        echo "[-] SELinux hooks.c already patched, skipping."
        return
    fi

    if ! grep -q 'If this is a user namespace mount' "$SELINUX_HOOKS"; then
        echo "[!] Anchor not found in hooks.c, skipping SELinux patch."
        return
    fi

    ANCHOR_LINE=$(grep -n 'If this is a user namespace mount' "$SELINUX_HOOKS" | cut -d: -f1)
    INSERT_BEFORE=$((ANCHOR_LINE - 1))

    sed -i "${INSERT_BEFORE}i\\
#ifdef CONFIG_NOMOUNT_FS\\
\t/* NoMountFS inherits labels directly from lower inodes via\\
\t * security_inode_notifysecctx — use NATIVE behavior so\\
\t * SBLABEL_MNT is set and notifysecctx can set LABEL_INITIALIZED.\\
\t */\\
\tif (!strcmp(sb->s_type->name, \"nomountfs\"))\\
\t\tsbsec->behavior = SECURITY_FS_USE_NATIVE;\\
#endif\\
" "$SELINUX_HOOKS"

    echo "[+] Patched security/selinux/hooks.c"
}

unpatch_selinux_hooks() {
    if [ -z "$SELINUX_HOOKS" ]; then
        return
    fi

    if ! grep -q 'CONFIG_NOMOUNT_FS' "$SELINUX_HOOKS"; then
        echo "[-] SELinux hooks.c: NoMountFS patch not found, skipping unpatch."
        return
    fi

    echo "[-] Reverting SELinux hooks.c patch..."

    # Use awk to precisely remove only NoMountFS blocks
    awk '
    BEGIN { skip = 0 }
    
    /^#ifdef CONFIG_NOMOUNT_FS$/ {
        skip = 1
        next
    }
    
    skip == 1 && /^#endif$/ {
        skip = 0
        next
    }
    
    skip == 1 { next }
    
    { print }
    ' "$SELINUX_HOOKS" > "${SELINUX_HOOKS}.tmp"
    
    mv "${SELINUX_HOOKS}.tmp" "$SELINUX_HOOKS"

    echo "[-] SELinux hooks.c patch reverted."
}

patch_selinux_hooks_copy_sid() {
    if [ -z "$SELINUX_HOOKS" ]; then
        echo "[!] security/selinux/hooks.c not found, skipping copy_sid patch."
        return
    fi

    if grep -q 'selinux_sb_copy_sid_from' "$SELINUX_HOOKS"; then
        echo "[-] SELinux hooks.c copy_sid already patched, skipping."
        return
    fi

    if ! grep -q 'Allow filesystems with binary mount data' "$SELINUX_HOOKS"; then
        echo "[!] copy_sid anchor not found in hooks.c, skipping."
        return
    fi

    ANCHOR_LINE=$(grep -n 'Allow filesystems with binary mount data' "$SELINUX_HOOKS" | cut -d: -f1)
    INSERT_BEFORE=$((ANCHOR_LINE - 1))

    sed -i "${INSERT_BEFORE}i\\
#ifdef CONFIG_NOMOUNT_FS\\
void selinux_sb_copy_sid_from(struct super_block *dst, struct super_block *src)\\
{\\
\tstruct superblock_security_struct *dst_sbsec = dst->s_security;\\
\tstruct superblock_security_struct *src_sbsec = src->s_security;\\
\tdst_sbsec->sid = src_sbsec->sid;\\
\tdst_sbsec->def_sid = src_sbsec->def_sid;\\
\tdst_sbsec->flags = src_sbsec->flags & SBLABEL_MNT;\\
}\\
EXPORT_SYMBOL(selinux_sb_copy_sid_from);\\
#endif\\
" "$SELINUX_HOOKS"

    echo "[+] Patched security/selinux/hooks.c (copy_sid)"
}

unpatch_selinux_hooks_copy_sid() {
    if [ -z "$SELINUX_HOOKS" ]; then
        return
    fi

    if ! grep -q 'selinux_sb_copy_sid_from' "$SELINUX_HOOKS"; then
        echo "[-] SELinux hooks.c: copy_sid patch not found, skipping unpatch."
        return
    fi

    echo "[-] Reverting SELinux hooks.c copy_sid patch..."

    # Use awk to remove only the selinux_sb_copy_sid_from block
    awk '
    BEGIN { skip = 0 }
    
    /^#ifdef CONFIG_NOMOUNT_FS$/ {
        if ((getline nextline) > 0) {
            if (nextline ~ /selinux_sb_copy_sid_from/) {
                skip = 1
                next
            } else {
                print
                print nextline
            }
        }
        next
    }
    
    skip == 1 && /^#endif$/ {
        skip = 0
        next
    }
    
    skip == 1 { next }
    
    { print }
    ' "$SELINUX_HOOKS" > "${SELINUX_HOOKS}.tmp"
    
    mv "${SELINUX_HOOKS}.tmp" "$SELINUX_HOOKS"

    echo "[-] SELinux hooks.c copy_sid patch reverted."
}

perform_cleanup() {
    echo "[+] Cleaning up NoMountFS..."
    [ -L "$FS_DIR/nomount" ] && rm "$FS_DIR/nomount" && echo "[-] Symlink removed."
    if [ -f "$FS_MAKEFILE" ]; then
        sed -i '/nomount/d' "$FS_MAKEFILE" && echo "[-] Makefile reverted."
    fi
    if [ -f "$FS_KCONFIG" ]; then
        sed -i '/nomount\/Kconfig/d' "$FS_KCONFIG" && echo "[-] Kconfig reverted."
    fi

    unpatch_selinux_hooks
    unpatch_selinux_hooks_copy_sid
    unpatch_kernel_sys_c

    if [ -d "$GKI_ROOT/$REPO" ]; then
        echo "[?] Do you want to delete the repository directory $REPO? (y/n)"
        read -r answer
        if [ "$answer" = "y" ]; then
            rm -rf "$GKI_ROOT/$REPO" && echo "[-] Repository deleted."
        fi
    fi
    echo "[+] Done."
}

setup_nomountfs() {
    echo "[+] Setting up NoMountFS..."
    if [ ! -d "$GKI_ROOT/$REPO" ]; then
        git clone "https://github.com/$OWNER/$REPO" || { echo "[!] Clone failed"; exit 1; }
    fi
    cd "$GKI_ROOT/$REPO"
    git fetch --all
    if [ -z "${1-}" ]; then
        TAG=$(git describe --tags --abbrev=0 2>/dev/null || echo "")
        if [ -n "$TAG" ]; then
            git checkout "$TAG" && echo "[-] Checked out tag $TAG"
        else
            git checkout master 2>/dev/null || echo "[!] Using current branch"
        fi
    else
        git checkout "$1" && echo "[-] Checked out $1."
    fi
    cd "$FS_DIR"
    ln -sf "$(realpath --relative-to="$FS_DIR" "$GKI_ROOT/$REPO/src")" "nomount"
    echo "[+] Symlink created: fs/nomount -> $REPO/src"
    if ! grep -q "nomount" "$FS_MAKEFILE"; then
        printf "obj-\$(CONFIG_NOMOUNT_FS) += nomount/\n" >> "$FS_MAKEFILE"
        echo "[+] Modified fs/Makefile"
    fi
    if ! grep -q "nomount/Kconfig" "$FS_KCONFIG"; then
        sed -i '$i source "fs/nomount/Kconfig"' "$FS_KCONFIG"
        echo "[+] Modified fs/Kconfig"
    fi

    patch_selinux_hooks
    patch_selinux_hooks_copy_sid
    patch_kernel_sys_c

    echo '[+] NoMountFS is ready to be compiled!'
    echo '[+] Run: make menuconfig and look for NoMountFS under File Systems'
}

if [ "$#" -eq 0 ]; then
    initialize_variables
    setup_nomountfs
elif [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    display_usage
elif [ "$1" = "--cleanup" ]; then
    initialize_variables
    perform_cleanup
else
    initialize_variables
    setup_nomountfs "$@"
fi
