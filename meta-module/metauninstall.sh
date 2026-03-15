#!/system/bin/sh
# metauninstall.sh — NoMountFS Metamodule uninstall hook  v2.3
#
# Called by KernelSU before a module's directory is removed.
# $1 = MODULE_ID of the module being uninstalled.
#
# Actions:
#   1. Remove the module's ID from mount_order.
#   2. For every nomountfs-mounted partition: unmount, rebuild the merged
#      tree without the removed module, re-mount.
#      If no remaining modules contribute files for a partition, it stays
#      unmounted (real partition files become visible again).

MODULE_ID="$1"
META_DIR="/data/adb/modules/meta-nomountfs"
ORDER_FILE="$META_DIR/mount_order"
STATE_DIR="/dev/nomountfs_state"
MERGE_BASE="/dev/nomountfs_merge"
MODULES_DIR="/data/adb/modules"
PARTITIONS="system vendor product system_ext odm oem"

log() {
    local msg="[meta-nomountfs] metauninstall: $*"
    echo "$msg" >> /dev/kmsg 2>/dev/null
    echo "$msg" >> "$STATE_DIR/mount.log" 2>/dev/null
}

is_nomountfs() {
    grep -q " $1 nomountfs " /proc/mounts 2>/dev/null
}

if [ -z "$MODULE_ID" ]; then
    log "ERROR: no MODULE_ID passed. Aborting."
    exit 1
fi

log "Uninstalling module: $MODULE_ID"

# ─── Step 1: remove from mount_order ─────────────────────────────────────────

if [ -f "$ORDER_FILE" ]; then
    TMP_ORDER="$STATE_DIR/mount_order_tmp_$$"
    grep -vx "$MODULE_ID" "$ORDER_FILE" > "$TMP_ORDER" 2>/dev/null
    mv -f "$TMP_ORDER" "$ORDER_FILE"
    log "Removed $MODULE_ID from mount_order."
fi

# ─── Step 2: rebuild module list (mirrors build_module_list in metamount.sh,
#             excluding MODULE_ID) ────────────────────────────────────────────

build_module_list_except() {
    local EXCLUDE="$1"
    MODULE_LIST=""

    # Collect all enabled modules alphabetically, excluding the removed one.
    ALPHA_LIST=""
    for MOD in "$MODULES_DIR"/*; do
        [ -d "$MOD" ] || continue
        local MODID="${MOD##*/}"
        [ "$MODID" = "meta-nomountfs" ] && continue
        [ "$MODID" = "$EXCLUDE" ]       && continue
        [ -f "$MOD/disable" ]           && continue
        [ -f "$MOD/skip_mount" ]        && continue
        ALPHA_LIST="$ALPHA_LIST $MODID"
    done
    ALPHA_LIST="$(echo "$ALPHA_LIST" | tr ' ' '\n' | grep -v '^$' | sort | tr '\n' ' ')"

    # Populate from mount_order first (excluding the removed module, already
    # stripped above but guard here too).
    LISTED=""
    if [ -f "$ORDER_FILE" ]; then
        while IFS= read -r LINE || [ -n "$LINE" ]; do
            LINE="$(echo "$LINE" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
            [ -z "$LINE" ] && continue
            case "$LINE" in '#'*) continue ;; esac
            [ "$LINE" = "$EXCLUDE" ] && continue

            MOD="$MODULES_DIR/$LINE"
            [ -d "$MOD" ] || continue
            [ -f "$MOD/disable" ]    && continue
            [ -f "$MOD/skip_mount" ] && continue

            MODULE_LIST="$MODULE_LIST $MOD"
            LISTED="$LISTED $LINE"
        done < "$ORDER_FILE"
    fi

    # Append unlisted modules alphabetically.
    for MODID in $ALPHA_LIST; do
        already=0
        for L in $LISTED; do
            [ "$L" = "$MODID" ] && { already=1; break; }
        done
        [ $already -eq 1 ] && continue
        MODULE_LIST="$MODULE_LIST $MODULES_DIR/$MODID"
    done
}



default_context_for_path() {
    local P="$1"
    case "$P" in
        /system/lib64/*|/system/lib/*)           echo "u:object_r:system_lib_file:s0" ;;
        /system/app/*|/system/priv-app/*)        echo "u:object_r:system_app_file:s0" ;;
        /system/bin/*|/system/xbin/*)            echo "u:object_r:system_file:s0"     ;;
        /system/framework/*)                     echo "u:object_r:system_file:s0"     ;;
        /system/etc/*)                           echo "u:object_r:system_file:s0"     ;;
        /system/*)                               echo "u:object_r:system_file:s0"     ;;
        /vendor/lib64/*|/vendor/lib/*)           echo "u:object_r:vendor_lib_file:s0" ;;
        /vendor/app/*|/vendor/priv-app/*)        echo "u:object_r:vendor_app_file:s0" ;;
        /vendor/bin/*)                           echo "u:object_r:vendor_file:s0"     ;;
        /vendor/*)                               echo "u:object_r:vendor_file:s0"     ;;
        /product/*|/system_ext/*|/odm/*|/oem/*) echo "u:object_r:system_file:s0"     ;;
        *)                                       echo "u:object_r:system_file:s0"     ;;
    esac
}

get_context() {
    local DEST="$1"
    local CTX
    CTX="$(getfilecon "$DEST" 2>/dev/null | awk '{print $NF}')"
    case "$CTX" in
        u:object_r:*:s0*) echo "$CTX"; return ;;
    esac
    CTX="$(ls -Z "$DEST" 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i~/^u:object_r:/) {print $i; exit}}')"
    case "$CTX" in
        u:object_r:*:s0*) echo "$CTX"; return ;;
    esac
    default_context_for_path "$DEST"
}

relabel_merged() {
    local MERGED="$1"
    local DEST_ROOT="$2"
    find "$MERGED" -type d 2>/dev/null | while IFS= read -r DIR; do
        chcon "$(get_context "${DEST_ROOT}${DIR#"$MERGED"}")" "$DIR" 2>/dev/null
    done
    find "$MERGED" \( -type f -o -type l \) 2>/dev/null | while IFS= read -r FILE; do
        chcon "$(get_context "${DEST_ROOT}${FILE#"$MERGED"}")" "$FILE" 2>/dev/null
    done
}

rebuild_merge() {
    local PART="$1"
    local DEST="$2"
    local MERGED="$MERGE_BASE/$PART"
    rm -rf "$MERGED"

    for MOD in $MODULE_LIST; do
        local SRC="$MOD/$PART"
        [ -d "$SRC" ] || continue

        local MODID="${MOD##*/}"
        log "  re-layer $MODID/$PART"

        find "$SRC" \( -type f -o -type l \) 2>/dev/null | while IFS= read -r FILE; do
            local REL="${FILE#"$SRC"}"
            local DST="${MERGED}${REL}"
            mkdir -p "${DST%/*}" 2>/dev/null
            if [ -L "$FILE" ]; then
                ln -sf "$(readlink "$FILE")" "$DST" 2>/dev/null
            else
                cp -f "$FILE" "$DST" 2>/dev/null
            fi
        done
    done

    if [ ! -d "$MERGED" ] || [ -z "$(ls -A "$MERGED" 2>/dev/null)" ]; then
        return
    fi

    relabel_merged "$MERGED" "$DEST"
    log "  relabelled $MERGED"
    echo "$MERGED"
}

# ─── Step 3: unmount → rebuild → remount each affected partition ──────────────

build_module_list_except "$MODULE_ID"
log "Remaining module order:$MODULE_LIST"

for PART in $PARTITIONS; do
    case "$PART" in
        system)     TARGET="/system"     ;;
        vendor)     TARGET="/vendor"     ;;
        product)    TARGET="/product"    ;;
        system_ext) TARGET="/system_ext" ;;
        odm)        TARGET="/odm"        ;;
        oem)        TARGET="/oem"        ;;
        *)          continue             ;;
    esac

    is_nomountfs "$TARGET" || continue
    log "Unmounting $TARGET for rebuild..."

    if ! umount "$TARGET" 2>/dev/null; then
        umount -l "$TARGET" 2>/dev/null \
            && log "  lazy unmount: $TARGET" \
            || { log "  FAIL to unmount $TARGET — skipping"; continue; }
    fi

    MERGED="$(rebuild_merge "$PART" "$TARGET")"

    if [ -z "$MERGED" ]; then
        log "  No remaining modules for $PART — leaving unmounted."
        continue
    fi

    mount -t nomountfs none "$TARGET" \
        -o "upperdir=${MERGED},lowerdir=${TARGET}" \
        2>>"$STATE_DIR/mount.log"
    if [ $? -eq 0 ]; then
        log "  Re-mounted $PART -> $TARGET"
    else
        log "  FAIL to re-mount $PART -> $TARGET"
    fi
done

# ─── Step 4: sync active_mounts state file ────────────────────────────────────

if [ -f "$STATE_DIR/active_mounts" ]; then
    TMP="$STATE_DIR/active_mounts_tmp_$$"
    while IFS= read -r MNT; do
        is_nomountfs "$MNT" && echo "$MNT"
    done < "$STATE_DIR/active_mounts" > "$TMP"
    mv -f "$TMP" "$STATE_DIR/active_mounts"
fi

log "Done."
