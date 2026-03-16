#!/system/bin/sh
# metamount.sh — NoMountFS Metamodule mount script  v2.3
#
# Features:
#   1. Merges all enabled modules into one unified source tree per partition.
#   2. Issues exactly ONE nomountfs mount per partition.
#   3. Layer stacking via $MODDIR/mount_order:
#        - One module ID per line, top = lowest priority, bottom = highest.
#        - Bottom entry mounts last → wins on file collision.
#        - Modules not listed in mount_order are appended after listed ones,
#          sorted alphabetically (so they layer above listed modules by default,
#          unless you explicitly place them in mount_order).
#        - If mount_order is absent, all modules are processed alphabetically
#          with the last letter winning on collision.
#
# Mount model:
#   mount -t nomountfs none <DEST>
#         -o upperdir=<MERGED_TREE>,lowerdir=<DEST>

MODDIR="${0%/*}"
MODULES_DIR="/data/adb/modules"
PARTITIONS="system vendor product system_ext odm oem"
ORDER_FILE="$MODDIR/mount_order"

STATE_DIR="/dev/nomountfs_state"
MERGE_BASE="/dev/nomountfs_merge"
mkdir -p "$STATE_DIR" "$MERGE_BASE"

# ─── helpers ──────────────────────────────────────────────────────────────────

log() {
    local msg="[meta-nomountfs] $*"
    echo "$msg" >> /dev/kmsg 2>/dev/null
    echo "$msg" >> "$STATE_DIR/mount.log" 2>/dev/null
}

wait_for_fs() {
    local retries=10
    while [ $retries -gt 0 ]; do
        grep -q "nomountfs" /proc/filesystems 2>/dev/null && return 0
        retries=$((retries - 1))
        sleep 1
    done
    return 1
}

record_mount() {
    echo "$1" >> "$STATE_DIR/active_mounts"
}

is_mounted() {
    grep -q " $1 nomountfs " /proc/mounts 2>/dev/null
}

# ─── ordered module list ──────────────────────────────────────────────────────
# Builds MODULE_LIST in mount order: lowest priority first, highest last.
#
# Order:
#   1. Modules listed in mount_order (top of file = first = lowest priority).
#   2. Enabled modules NOT in mount_order, sorted alphabetically, appended
#      after the listed ones (so they sit above listed modules unless moved).
#
# Only enabled, non-meta modules are included.

build_module_list() {
    MODULE_LIST=""

    # --- Pass 1: collect all enabled modules into a flat alphabetical list.
    ALPHA_LIST=""
    for MOD in "$MODULES_DIR"/*; do
        [ -d "$MOD" ] || continue
        MODID="${MOD##*/}"
        [ "$MODID" = "meta-nomountfs" ] && continue
        if [ -f "$MOD/disable" ] || [ -f "$MOD/skip_mount" ]; then
            continue
        fi
        ALPHA_LIST="$ALPHA_LIST $MODID"
    done
    # ALPHA_LIST is already in filesystem order (typically alphabetical on ext4).
    # Sort it explicitly to guarantee alphabetical regardless of fs.
    ALPHA_LIST="$(echo "$ALPHA_LIST" | tr ' ' '\n' | grep -v '^$' | sort | tr '\n' ' ')"

    # --- Pass 2: if mount_order exists, start MODULE_LIST with its entries
    #             (in file order, top→bottom), skipping any that are disabled
    #             or don't exist.
    LISTED=""
    if [ -f "$ORDER_FILE" ]; then
        while IFS= read -r LINE || [ -n "$LINE" ]; do
            # Strip leading/trailing whitespace and ignore blank/comment lines.
            LINE="$(echo "$LINE" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
            [ -z "$LINE" ] && continue
            case "$LINE" in '#'*) continue ;; esac

            MOD="$MODULES_DIR/$LINE"
            [ -d "$MOD" ] || continue
            [ -f "$MOD/disable" ] && continue
            [ -f "$MOD/skip_mount" ] && continue

            MODULE_LIST="$MODULE_LIST $MOD"
            LISTED="$LISTED $LINE"
        done < "$ORDER_FILE"
    fi

    # --- Pass 3: append alphabetically-sorted modules that were NOT listed
    #             in mount_order.
    for MODID in $ALPHA_LIST; do
        # Check if already in LISTED.
        already=0
        for L in $LISTED; do
            [ "$L" = "$MODID" ] && { already=1; break; }
        done
        [ $already -eq 1 ] && continue
        MODULE_LIST="$MODULE_LIST $MODULES_DIR/$MODID"
    done
}

# ─── SELinux relabelling ──────────────────────────────────────────────────────
# Strategy (per inode):
#   1. Read the context of the matching real file on the live partition
#      (the path the merged file will shadow).  If it exists and has a
#      readable context, use that — it's authoritative.
#   2. If the real file doesn't exist yet (new file added by a module) or
#      getfilecon fails, fall back to a path-based default map.
#
# This means a module replacing /system/framework/framework.jar gets exactly
# the same label the original jar has, with zero hardcoding needed.

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

# get_context <real_dest_path>
# Tries getfilecon first (reads xattr directly, no mount traversal needed).
# Falls back to `ls -Z` parsing, then to the default map.
get_context() {
    local DEST="$1"
    local CTX

    # getfilecon prints: <path> <context>
    CTX="$(getfilecon "$DEST" 2>/dev/null | awk '{print $NF}')"
    # Validate: must look like a full context (u:object_r:...:s0)
    case "$CTX" in
        u:object_r:*:s0*) echo "$CTX"; return ;;
    esac

    # Fallback: ls -Z on Android prints context in field 4 or 5 depending on
    # whether the file exists and SELinux is enforcing.
    CTX="$(ls -Z "$DEST" 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i~/^u:object_r:/) {print $i; exit}}')"
    case "$CTX" in
        u:object_r:*:s0*) echo "$CTX"; return ;;
    esac

    # Last resort: path-based default map.
    default_context_for_path "$DEST"
}

# relabel_merged: walk every inode in the merged tree, look up the context of
# the corresponding real file on the destination partition, apply it.
# $1 = merged tree root  (e.g. /dev/nomountfs_merge/system)
# $2 = real partition root (e.g. /system)

relabel_merged() {
    local MERGED="$1"
    local DEST_ROOT="$2"

    # Directories first so child lookups are allowed immediately after.
    find "$MERGED" -type d 2>/dev/null | while IFS= read -r DIR; do
        local DEST_PATH="${DEST_ROOT}${DIR#"$MERGED"}"
        chcon "$(get_context "$DEST_PATH")" "$DIR" 2>/dev/null
    done

    # Files and symlinks.
    find "$MERGED" \( -type f -o -type l \) 2>/dev/null | while IFS= read -r FILE; do
        local DEST_PATH="${DEST_ROOT}${FILE#"$MERGED"}"
        chcon "$(get_context "$DEST_PATH")" "$FILE" 2>/dev/null
    done
}

# ─── merge tree builder ───────────────────────────────────────────────────────
# Iterates MODULE_LIST (lowest priority first) and hard-links/copies every
# file from each module's <part>/ subtree into a single merged staging dir.
# Later iterations overwrite earlier ones → last (highest priority) wins.
# After merging, relabels every inode so SELinux sees the correct context.

merge_partition() {
    local PART="$1"
    local DEST="$2"
    local MERGED="$MERGE_BASE/$PART"
    rm -rf "$MERGED"

    for MOD in $MODULE_LIST; do
        local SRC="$MOD/$PART"
        [ -d "$SRC" ] || continue

        local MODID="${MOD##*/}"
        log "  layer $MODID/$PART"

        find "$SRC" \( -type f -o -type l \) 2>/dev/null | while IFS= read -r FILE; do
            local REL="${FILE#"$SRC"}"
            local DST="${MERGED}${REL}"
            mkdir -p "${DST%/*}" 2>/dev/null

            if [ -L "$FILE" ]; then
                ln -sf "$(readlink "$FILE")" "$DST" 2>/dev/null
            else
                # Always copy — never hard-link.
                # Hard-links share the inode with the source file on the module's
                # ext4 partition, so chcon on the merge-tree copy would modify
                # the original inode's xattr, triggering an SELinux relabelfrom
                # denial when init owns the source file (u:object_r:system_file).
                cp -f "$FILE" "$DST" 2>/dev/null
            fi
        done
    done

    if [ ! -d "$MERGED" ] || [ -z "$(ls -A "$MERGED" 2>/dev/null)" ]; then
        return
    fi

    # Apply correct SELinux labels to every inode in the merged tree.
    relabel_merged "$MERGED" "$DEST"
    log "  relabelled $MERGED"

    echo "$MERGED"
}

# ─── single partition mount ───────────────────────────────────────────────────

mount_partition() {
    local PART="$1"
    local DEST="$2"

    [ -d "$DEST" ] || { log "skip (no dest dir): $DEST"; return 0; }
    is_mounted "$DEST" && { log "skip (already mounted): $DEST"; return 0; }

    local MERGED
    MERGED="$(merge_partition "$PART" "$DEST")"

    if [ -z "$MERGED" ]; then
        log "skip (no module files for partition): $PART"
        return 0
    fi

    mount -t nomountfs KSU "$DEST" \
        -o "upperdir=${MERGED},lowerdir=${DEST}" \
        2>>"$STATE_DIR/mount.log"
    local rc=$?
    if [ $rc -eq 0 ]; then
        if [ -f /data/adb/ksu/bin/ksud ]; then
            /data/adb/ksu/bin/ksud kernel umount del "$DEST"  >/dev/null 2>&1
            /data/adb/ksu/bin/ksud kernel umount add "$DEST"  >/dev/null 2>&1
            /data/adb/ksud kernel notify-module-mounted >/dev/null 2>&1
        fi
        log "OK: $PART -> $DEST"
        record_mount "$DEST"
    else
        log "FAIL (rc=$rc): $PART -> $DEST"
    fi
    return $rc
}

# ─── main ─────────────────────────────────────────────────────────────────────

if ! wait_for_fs; then
    log "FATAL: nomountfs not found in /proc/filesystems after 10s. Aborting."
    exit 1
fi

log "Starting merged mount pass (v2.1)..."

if [ -f "$ORDER_FILE" ]; then
    log "Using mount_order: $ORDER_FILE"
else
    log "No mount_order found — using alphabetical order."
fi

build_module_list
log "Module load order:$MODULE_LIST"

ok=0; fail=0

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

    if mount_partition "$PART" "$TARGET"; then
        ok=$((ok + 1))
    else
        fail=$((fail + 1))
    fi
done

log "Mount pass complete: $ok mounted, $fail failed."
