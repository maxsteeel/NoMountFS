# customize.sh — NoMountFS Metamodule installer  (sourced by KernelSU)
#
# Runs in BusyBox ash with Standalone Mode enabled.
# KSU handles file extraction and permissions automatically before sourcing
# this script — do NOT call install_module here.
#
# Available: KSU, KSU_VER, KSU_VER_CODE, KSU_KERNEL_VER_CODE, BOOTMODE,
#            MODPATH, TMPDIR, ZIPFILE, ARCH, IS64BIT, API
# Functions: ui_print, abort, set_perm, set_perm_recursive

MODULES_DIR="/data/adb/modules"
ORDER_FILE="$MODPATH/mount_order"
PARTITIONS="system vendor product system_ext odm oem"

# ─── environment checks ───────────────────────────────────────────────────────

ui_print "- [meta-nomountfs] Installing NoMountFS Metamodule..."
ui_print "  KernelSU $KSU_VER  |  API $API  |  ARCH $ARCH"

if grep -q "nomountfs" /proc/filesystems 2>/dev/null; then
    ui_print "  ✓ nomountfs found in /proc/filesystems"
else
    ui_print "  ! nomountfs NOT in /proc/filesystems"
    ui_print "    Load the NoMountFS .ko before rebooting, or mounts will fail."
fi

# Warn if another mount metamodule is already active.
for OTHER_META in "$MODULES_DIR"/*/metamount.sh; do
    [ -f "$OTHER_META" ] || continue
    OTHER_DIR="${OTHER_META%/metamount.sh}"
    OTHER_ID="${OTHER_DIR##*/}"
    [ "$OTHER_ID" = "meta-nomountfs" ] && continue
    [ -f "$OTHER_DIR/disable" ] && continue
    ui_print "  ! WARNING: Another mount metamodule is active: $OTHER_ID"
    ui_print "    Disable it to avoid conflicting mounts."
done

# ─── build initial mount_order ────────────────────────────────────────────────
#
# Only include modules that actually have at least one recognised partition
# folder (system/, vendor/, product/, system_ext/, odm/, oem/).
# Modules with none of those folders have nothing to mount and are excluded.
# Eligible modules are sorted alphabetically: top = lowest priority,
# bottom = highest (last one mounted wins on file collision).

ui_print "- [meta-nomountfs] Building initial mount_order..."

RAW_LIST=""
for MOD in "$MODULES_DIR"/*; do
    [ -d "$MOD" ] || continue
    MODID="${MOD##*/}"

    [ "$MODID" = "meta-nomountfs" ] && continue
    [ -f "$MOD/disable" ]           && continue
    [ -f "$MOD/skip_mount" ]        && continue

    # Only include if the module has at least one partition subtree.
    HAS_PART=0
    for PART in $PARTITIONS; do
        if [ -d "$MOD/$PART" ]; then
            HAS_PART=1
            break
        fi
    done
    [ $HAS_PART -eq 0 ] && continue

    RAW_LIST="$RAW_LIST
$MODID"
done

SORTED="$(printf '%s\n' $RAW_LIST | grep -v '^$' | sort)"

{
    printf '# mount_order — managed by meta-nomountfs\n'
    printf '# One module ID per line. Top = lowest priority, bottom = highest.\n'
    printf '# Only modules with partition folders (system/vendor/etc.) are listed.\n'
    printf '# Edit manually to reorder. metainstall.sh appends new installs here.\n'
    [ -n "$SORTED" ] && printf '%s\n' $SORTED
} > "$ORDER_FILE"

if [ -z "$SORTED" ]; then
    ui_print "  (no mountable modules found — mount_order written empty)"
else
    COUNT="$(printf '%s\n' $SORTED | grep -c .)"
    ui_print "  ✓ mount_order written with $COUNT module(s):"
    for ID in $SORTED; do
        ui_print "      $ID"
    done
fi
chmod 777 umounter.sh
ui_print "- [meta-nomountfs] Installation complete."
ui_print "  Reboot to activate NoMountFS mounts."
