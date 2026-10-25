#!/system/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
# KaoriosHelper Early Boot Setup (post-fs-data)

MODDIR="${0%/*}"
DATA_DIR="$MODDIR/data"

# Ensure data directory exists and has restricted permissions
mkdir -p "$DATA_DIR/logs" 2>/dev/null
chmod 0700 "$DATA_DIR" 2>/dev/null
chmod 0700 "$DATA_DIR/logs" 2>/dev/null

# Remove stale PID and lock files from prior reboots
rm -f "$DATA_DIR"/*.pid "$DATA_DIR"/*.lock "$DATA_DIR"/control.sock 2>/dev/null

# ── Resetprop Resolution ─────────────────────────────────────────────────────
RESETPROP="resetprop"
if ! command -v resetprop >/dev/null 2>&1; then
  if [ -x "/data/adb/ksu/bin/resetprop" ]; then
    RESETPROP="/data/adb/ksu/bin/resetprop"
  elif [ -x "/data/adb/ap/bin/resetprop" ]; then
    RESETPROP="/data/adb/ap/bin/resetprop"
  elif [ -x "/data/adb/magisk/magisk" ]; then
    RESETPROP="/data/adb/magisk/magisk resetprop"
  fi
fi

resetprop_if_diff() {
  local prop="$1"
  local val="$2"
  [ -z "$prop" ] || [ -z "$val" ] && return
  local cur
  cur="$($RESETPROP "$prop" 2>/dev/null)"
  if [ "$cur" != "$val" ]; then
    $RESETPROP -n "$prop" "$val" 2>/dev/null || $RESETPROP "$prop" "$val" 2>/dev/null || true
  fi
}

resetprop_delete() {
  local prop="$1"
  [ -z "$prop" ] && return
  $RESETPROP --delete "$prop" 2>/dev/null || true
}

# ── 1. Bootloader & Verified Boot State (AlwaysStrong / PIF parity) ──────────
resetprop_if_diff "ro.boot.verifiedbootstate" "green"
resetprop_if_diff "ro.boot.flash.locked" "1"
resetprop_if_diff "ro.boot.vbmeta.device_state" "locked"
resetprop_if_diff "ro.boot.veritymode" "enforcing"
resetprop_if_diff "sys.oem_unlock_allowed" "0"

# ── 2. OEM Tamper Indicators & Unlock Artifacts ──────────────────────────────
# Samsung Knox warranty bit
resetprop_if_diff "ro.boot.warranty_bit" "0"
resetprop_if_diff "ro.warranty_bit" "0"
# Realme / OPPO boot state
resetprop_if_diff "ro.boot.realmebootstate" "green"
# OnePlus / Oplus orange state
resetprop_if_diff "ro.is_ever_orange" "0"
resetprop_if_diff "ro.boot.vbmeta.invalidate_on_error" "yes"

# ── 3. Build Security & Release Type Spoofing ────────────────────────────────
resetprop_if_diff "ro.debuggable" "0"
resetprop_if_diff "ro.secure" "1"
resetprop_if_diff "ro.adb.secure" "1"

resetprop_if_diff "ro.build.type" "user"
resetprop_if_diff "ro.build.tags" "release-keys"
resetprop_if_diff "ro.bootimage.build.type" "user"
resetprop_if_diff "ro.bootimage.build.tags" "release-keys"
resetprop_if_diff "ro.system.build.type" "user"
resetprop_if_diff "ro.system.build.tags" "release-keys"
resetprop_if_diff "ro.system_ext.build.type" "user"
resetprop_if_diff "ro.system_ext.build.tags" "release-keys"
resetprop_if_diff "ro.product.build.type" "user"
resetprop_if_diff "ro.product.build.tags" "release-keys"
resetprop_if_diff "ro.vendor.build.type" "user"
resetprop_if_diff "ro.vendor.build.tags" "release-keys"
resetprop_if_diff "ro.odm.build.type" "user"
resetprop_if_diff "ro.odm.build.tags" "release-keys"

# ── 4. Purge Boot Verification Failure Flags ─────────────────────────────────
resetprop_delete "ro.boot.verifiedbooterror"
resetprop_delete "ro.boot.verifyerrorpart"
resetprop_delete "ro.boot.verifyerrortype"

# ── 5. User Config Overrides (pif.json / custom.prop) ────────────────────────
if [ -s "$DATA_DIR/pif.json" ]; then
  SEC_PATCH="$(grep -o '"SECURITY_PATCH"[[:space:]]*:[[:space:]]*"[^"]*"' "$DATA_DIR/pif.json" 2>/dev/null | head -n1 | cut -d'"' -f4)"
  if [ -n "$SEC_PATCH" ]; then
    resetprop_if_diff "ro.build.version.security_patch" "$SEC_PATCH"
    resetprop_if_diff "ro.vendor.build.security_patch" "$SEC_PATCH"
  fi
  FIRST_API="$(grep -o '"FIRST_API_LEVEL"[[:space:]]*:[[:space:]]*[0-9]*' "$DATA_DIR/pif.json" 2>/dev/null | head -n1 | tr -cd '0-9')"
  [ -z "$FIRST_API" ] && FIRST_API="$(grep -o '"DEVICE_INITIAL_SDK_INT"[[:space:]]*:[[:space:]]*[0-9]*' "$DATA_DIR/pif.json" 2>/dev/null | head -n1 | tr -cd '0-9')"
  if [ -n "$FIRST_API" ]; then
    resetprop_if_diff "ro.product.first_api_level" "$FIRST_API"
  fi
fi

if [ -s "$DATA_DIR/custom.prop" ]; then
  while IFS='=' read -r key val || [ -n "$key" ]; do
    case "$key" in
      \#*|"") continue ;;
      *)
        key="$(echo "$key" | tr -d ' \t\r\n')"
        val="$(echo "$val" | tr -d '\r\n')"
        [ -n "$key" ] && resetprop_if_diff "$key" "$val"
        ;;
    esac
  done < "$DATA_DIR/custom.prop"
fi

