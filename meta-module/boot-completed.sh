#!/system/bin/sh
# boot-completed.sh — NoMountFS post-boot mount registration
#
# This script runs after boot is complete and registers all nomountfs
# mount points with the kernel umount manager.
#
# Usage: Called by service.sh after boot_completed
#
# Flow:
#   1. metamount.sh writes mount points to /dev/nomountfs_state/pending_mounts
#   2. This script reads that file and calls umounter.sh add for each
#   3. Deletes the pending file when done

STATE_DIR="/dev/nomountfs_state"
PENDING_FILE="$STATE_DIR/pending_mounts"
META_DIR="${0%/*}"

log() {
    local msg="[nomountfs-boot] $*"
    echo "$msg" >> /dev/kmsg 2>/dev/null
    echo "$msg" >> "$STATE_DIR/boot.log" 2>/dev/null
}

log "Starting post-boot mount registration..."

if [ ! -f "$PENDING_FILE" ]; then
    log "No pending mounts to register (file not found)"
    exit 0
fi

# Count mounts
MOUNT_COUNT=$(wc -l < "$PENDING_FILE" 2>/dev/null || echo "0")
log "Registering $MOUNT_COUNT mount point(s)..."

# Process each mount point
while IFS= read -r MOUNT_POINT || [ -n "$MOUNT_POINT" ]; do
    # Skip empty lines
    [ -z "$MOUNT_POINT" ] && continue
    
    log "Registering: $MOUNT_POINT"
    umount -l $MOUNT_POINT
    mount -t nomountfs none $MOUNT_POINT -o upperdir="/dev/nomountfs_merge${MOUNT_POINT}",lowerdir=$MOUNT_POINT
    "$META_DIR/umounter.sh" add "$MOUNT_POINT" 2>>"$STATE_DIR/boot.log"
done < "$PENDING_FILE"

# Clean up
rm -f "$PENDING_FILE"
log "Post-boot mount registration complete"
