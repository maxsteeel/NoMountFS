#!/system/bin/sh
# nomount-umount.sh — NoMountFS kernel umount list management
#
# This script manages the NoMountFS kernel-level umount list,
# allowing you to add or remove paths that should be automatically
# unmounted for new app processes.
#
# Usage:
#   nomount-umount.sh add <path>    - Add a path to the umount list
#   nomount-umount.sh del <path>    - Remove a path from the umount list
#   nomount-umount.sh list          - List all paths in the umount list
#   nomount-umount.sh clear         - Clear all paths from the umount list
#   nomount-umount.sh enable        - Enable kernel umount
#   nomount-umount.sh disable       - Disable kernel umount
#   nomount-umount.sh status        - Show current status

set -e

NOMOUNT_PROC="/proc/nomountfs"
NOMOUNT_UMOUNT_LIST="/data/adb/nomountfs_umount_list"
LOG_TAG="NoMountFS-umount"

log() {
    echo "[$LOG_TAG] $*"
    echo "[$LOG_TAG] $*" > /dev/kmsg 2>/dev/null || true
}

# Check if NoMountFS kernel module is loaded
check_module() {
    if ! grep -q "nomountfs" /proc/filesystems 2>/dev/null; then
        log "ERROR: NoMountFS module is not loaded"
        exit 1
    fi
}

# Add a path to the umount list
cmd_add() {
    local path="$1"
    if [ -z "$path" ]; then
        log "ERROR: path required"
        echo "Usage: $0 add <path>"
        exit 1
    fi

    # Normalize path
    path=$(realpath "$path" 2>/dev/null || echo "$path")

    # Check if already in list
    if [ -f "$NOMOUNT_PROC/umount_list" ] && grep -qxF "$path" "$NOMOUNT_PROC/umount_list" 2>/dev/null; then
        log "Path already in umount list: $path"
        return 0
    fi

    # Add to persistent list
    mkdir -p "$(dirname "$NOMOUNT_UMOUNT_LIST")"
    if ! grep -qxF "$path" "$NOMOUNT_UMOUNT_LIST" 2>/dev/null; then
        echo "$path" >> "$NOMOUNT_UMOUNT_LIST"
        log "Added to umount list: $path"
    fi
    # Also try to add via kernel interface if available
    if [ -f "$NOMOUNT_PROC/umount_add" ]; then
        echo "$path" > "$NOMOUNT_PROC/umount_add" 2>/dev/null || true
    fi
}

# Remove a path from the umount list
cmd_del() {
    local path="$1"
    if [ -z "$path" ]; then
        log "ERROR: path required"
        echo "Usage: $0 del <path>"
        exit 1
    fi

    # Normalize path
    path=$(realpath "$path" 2>/dev/null || echo "$path")

    # Remove from persistent list
    if [ -f "$NOMOUNT_UMOUNT_LIST" ]; then
        local temp_file=$(mktemp)
        grep -vxF "$path" "$NOMOUNT_UMOUNT_LIST" > "$temp_file" 2>/dev/null || true
        mv "$temp_file" "$NOMOUNT_UMOUNT_LIST"
    fi

    log "Removed from umount list: $path"

    # Also try to remove via kernel interface if available
    if [ -f "$NOMOUNT_PROC/umount_del" ]; then
        echo "$path" > "$NOMOUNT_PROC/umount_del" 2>/dev/null || true
    fi
}

# List all paths in the umount list
cmd_list() {
    # Also show from kernel if available
    if [ -f "$NOMOUNT_PROC/umount_list" ]; then
        echo "=== NoMountFS Kernel Umount List ==="
        cat "$NOMOUNT_PROC/umount_list"
    fi
}

# Clear all paths from the umount list
cmd_clear() {
    if [ -f "$NOMOUNT_UMOUNT_LIST" ]; then
        rm -f "$NOMOUNT_UMOUNT_LIST"
        log "Cleared umount list"
    fi

    # Also try to clear via kernel interface if available
    if [ -f "$NOMOUNT_PROC/umount_clear" ]; then
        echo "1" > "$NOMOUNT_PROC/umount_clear" 2>/dev/null || true
    fi
}

# Enable kernel umount
cmd_enable() {
    if [ -f "$NOMOUNT_PROC/umount_enabled" ]; then
        echo "1" > "$NOMOUNT_PROC/umount_enabled"
        log "Kernel umount enabled"
    else
        log "Kernel umount control not available (kernel interface not implemented)"
    fi
}

# Disable kernel umount
cmd_disable() {
    if [ -f "$NOMOUNT_PROC/umount_enabled" ]; then
        echo "0" > "$NOMOUNT_PROC/umount_enabled"
        log "Kernel umount disabled"
    else
        log "Kernel umount control not available (kernel interface not implemented)"
    fi
}

# Show current status
cmd_status() {
    echo "=== NoMountFS Status ==="

    # Check if module is loaded
    if grep -q "nomountfs" /proc/filesystems 2>/dev/null; then
        echo "Module: loaded"
    else
        echo "Module: not loaded"
    fi

    # Check umount list
    local count=0
    if [ -f "$NOMOUNT_UMOUNT_LIST" ]; then
        count=$(wc -l < "$NOMOUNT_UMOUNT_LIST" 2>/dev/null || echo "0")
    fi
    echo "Persistent umount entries: $count"

    # Check kernel umount status
    if [ -f "$NOMOUNT_PROC/umount_enabled" ]; then
        local enabled=$(cat "$NOMOUNT_PROC/umount_enabled" 2>/dev/null)
        if [ "$enabled" = "1" ]; then
            echo "Kernel umount: enabled"
        else
            echo "Kernel umount: disabled"
        fi
    else
        echo "Kernel umount: not available"
    fi

    # Show active mounts
    echo ""
    echo "=== Active NoMountFS Mounts ==="
    grep "nomountfs" /proc/mounts 2>/dev/null || echo "(none)"
}

# Restore umount list from persistent storage
restore_list() {
    if [ ! -f "$NOMOUNT_UMOUNT_LIST" ]; then
        return 0
    fi

    log "Restoring umount list from persistent storage..."
    while IFS= read -r path; do
        [ -z "$path" ] && continue
        # Add via kernel interface if available
        if [ -f "$NOMOUNT_PROC/umount_add" ]; then
            echo "$path" > "$NOMOUNT_PROC/umount_add" 2>/dev/null || true
        fi
    done < "$NOMOUNT_UMOUNT_LIST"
    log "Umount list restored"
}

# Main
case "${1:-}" in
    add)
        check_module
        cmd_add "$2"
        ;;
    del)
        check_module
        cmd_del "$2"
        ;;
    list)
        cmd_list
        ;;
    clear)
        cmd_clear
        ;;
    enable)
        cmd_enable
        ;;
    disable)
        cmd_disable
        ;;
    status)
        cmd_status
        ;;
    restore)
        restore_list
        ;;
    *)
        echo "NoMountFS Kernel Umount Manager"
        echo ""
        echo "Usage: $0 <command> [args]"
        echo ""
        echo "Commands:"
        echo "  add <path>     Add a path to the umount list"
        echo "  del <path>     Remove a path from the umount list"
        echo "  list           List all paths in the umount list"
        echo "  clear          Clear all paths from the umount list"
        echo "  enable         Enable kernel umount"
        echo "  disable        Disable kernel umount"
        echo "  status         Show current status"
        echo ""
        echo "Examples:"
        echo "  $0 add /system"
        echo "  $0 add /vendor"
        echo "  $0 list"
        echo "  $0 del /system"
        ;;
esac
