#!/system/bin/sh
# metainstall.sh — NoMountFS Metamodule installation hook  v2.1
# Sourced during regular module installation (NOT the metamodule itself).
# Inherits all variables and functions from KernelSU's built-in install.sh.
# $MODPATH is the path of the module being installed (set by KernelSU).

META_DIR="/data/adb/modules/meta-nomountfs"
ORDER_FILE="$META_DIR/mount_order"

# Derive the module ID being installed from its path.
INSTALL_ID="${MODPATH##*/}"

ui_print "- [meta-nomountfs] Validating NoMountFS environment..."

# Check 1: nomountfs filesystem registered with the kernel VFS.
if grep -q "nomountfs" /proc/filesystems 2>/dev/null; then
    ui_print "  ✓ nomountfs found in /proc/filesystems"
else
    ui_print "  ! nomountfs NOT in /proc/filesystems"
    ui_print "    Ensure the NoMountFS .ko is loaded before next reboot."
fi

# Check 2: Warn if another union/overlay mount metamodule appears active.
for OTHER_META in /data/adb/modules/*/metamount.sh; do
    [ -f "$OTHER_META" ] || continue
    OTHER_DIR="${OTHER_META%/metamount.sh}"
    OTHER_ID="${OTHER_DIR##*/}"
    [ "$OTHER_ID" = "meta-nomountfs" ] && continue
    if [ ! -f "$OTHER_DIR/disable" ]; then
        ui_print "  ! WARNING: Another mount metamodule is active: $OTHER_ID"
        ui_print "    This may conflict. Disable one of them."
    fi
done

# Check 3: Register module in mount_order only if it has at least one
# recognised partition folder (system/ vendor/ product/ system_ext/ odm/ oem/).
# Modules without any of these have nothing to mount and are not listed.
if [ -n "$INSTALL_ID" ] && [ -d "$META_DIR" ]; then
    HAS_PART=0
    for PART in system vendor product system_ext odm oem; do
        if [ -d "$MODPATH/$PART" ]; then
            HAS_PART=1
            break
        fi
    done

    if [ $HAS_PART -eq 0 ]; then
        ui_print "  = $INSTALL_ID has no partition folders — skipping mount_order"
    elif ! grep -qx "$INSTALL_ID" "$ORDER_FILE" 2>/dev/null; then
        echo "$INSTALL_ID" >> "$ORDER_FILE"
        ui_print "  + Registered $INSTALL_ID in mount_order (position: bottom)"
    else
        ui_print "  = $INSTALL_ID already in mount_order — order unchanged"
    fi
else
    ui_print "  ! Could not register in mount_order: META_DIR=$META_DIR INSTALL_ID=$INSTALL_ID"
fi

ui_print "- [meta-nomountfs] Running standard module installation..."
install_module
ui_print "- [meta-nomountfs] Module installed successfully."
