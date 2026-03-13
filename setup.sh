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
