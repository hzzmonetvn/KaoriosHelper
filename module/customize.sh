#!/system/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
# KaoriosHelper Module Installation Script

# Sourced by Magisk/KernelSU/KernelSU-Next/APatch installers: never enable `set -u` or `set -e`
# so caller-managed optional variables like `$REPLACE` or `$REMOVE` never abort installation.

print_banner() {
  ui_print "╔═════════════════════════════════════════════╗"
  ui_print "║              KaoriosHelper                  ║"
  ui_print "║      Unified Root & Zygisk Framework        ║"
  ui_print "║          HzzMonet / Kaorios Project         ║"
  ui_print "╚═════════════════════════════════════════════╝"
}

print_banner

# ── 1. Root Environment Check ────────────────────────────────────────────────
ui_print "- Checking root environment..."
ROOT_SOLUTION=""
if [ -n "${APATCH:-}" ] || command -v apd >/dev/null 2>&1 || [ -f /data/adb/apd ]; then
  ROOT_SOLUTION="APatch"
elif [ -n "${KSU:-}" ] || command -v ksud >/dev/null 2>&1 || [ -f /data/adb/ksud ]; then
  ROOT_SOLUTION="KernelSU"
elif [ -n "${MAGISK_VER:-}" ] || command -v magisk >/dev/null 2>&1 || [ -f /data/adb/magisk/magisk ]; then
  ROOT_SOLUTION="Magisk"
else
  ROOT_SOLUTION="Generic Root"
fi
ui_print "  Detected Root Manager: $ROOT_SOLUTION"

# ── 2. Android Version Check ─────────────────────────────────────────────────
API_LEVEL=$(getprop ro.build.version.sdk 2>/dev/null)
case "$API_LEVEL" in
  ''|*[!0-9]*) API_LEVEL=0 ;;
esac
ui_print "  Android SDK API Level: $API_LEVEL"

if [ "$API_LEVEL" -lt 31 ]; then
  ui_print "! Warning: KaoriosHelper targets Android 12+ (API 31-35+)."
  ui_print "! Early Android versions (<12) may have limited TEE/PMS support."
fi

# ── 3. Architecture Validation ───────────────────────────────────────────────
ARCH=$(getprop ro.product.cpu.abi)
ABILIST=$(getprop ro.product.cpu.abilist)
ui_print "  Primary Device ABI: $ARCH ($ABILIST)"

case "$ARCH" in
  x86*|i686*|amd64*)
    abort "! Error: x86 and x86_64 architectures are not supported. KaoriosHelper is built exclusively for ARM devices (arm64-v8a, armeabi-v7a)."
    ;;
esac

# ── 4. Zygisk Environment Detection ──────────────────────────────────────────
ui_print "- Checking Zygisk environment..."
ZYGISK_AVAILABLE=false

if [ -d "/data/adb/modules/zygisksu" ] && [ ! -f "/data/adb/modules/zygisksu/disable" ]; then
  ui_print "  Found: Zygisk Next (Active)"
  ZYGISK_AVAILABLE=true
fi
if [ -d "/data/adb/modules/rezygisk" ] && [ ! -f "/data/adb/modules/rezygisk/disable" ]; then
  ui_print "  Found: ReZygisk (Active)"
  ZYGISK_AVAILABLE=true
fi
if [ "$ROOT_SOLUTION" = "Magisk" ]; then
  MAGISK_ZYGISK=$(magisk --sqlite "SELECT value FROM settings WHERE key='zygisk';" 2>/dev/null || true)
  if [ "$MAGISK_ZYGISK" = "value=1" ]; then
    ui_print "  Found: Magisk Built-in Zygisk (Active)"
    ZYGISK_AVAILABLE=true
  fi
fi

if ! $ZYGISK_AVAILABLE; then
  ui_print "! Note: No active Zygisk runtime detected."
  ui_print "  Ensure Zygisk is enabled (or install Zygisk Next/ReZygisk) after reboot."
fi

# ── 5. Conflict Scanner ──────────────────────────────────────────────────────
ui_print "- Scanning for standalone conflicting modules..."
CONFLICT_COUNT=0
for mod in hma_oss_zygisk playintegrityfix playintegrityfork tricky_store teesim COPG; do
  if [ -d "/data/adb/modules/$mod" ] && [ ! -f "/data/adb/modules/$mod/disable" ]; then
    ui_print "  ! Warning: Active competing module found: $mod"
    ui_print "    To avoid hook collisions, consider disabling $mod after installation."
    CONFLICT_COUNT=$((CONFLICT_COUNT + 1))
  fi
done

if [ "$CONFLICT_COUNT" -gt 0 ]; then
  ui_print "  $CONFLICT_COUNT potential conflicting module(s) noted."
else
  ui_print "  No competing modules detected."
fi

# ── 6. Configuration Preservation Across Upgrades ────────────────────────────
CONFIG_DIR="${KAORIOS_CONFIG_DIR:-/data/adb/modules/kaorioshelper/data}"
LEGACY_DIR="${KAORIOS_LEGACY_DIR:-/data/adb/kaorios_helper}"
BACKUP_DIR="${KAORIOS_BACKUP_DIR:-/data/adb/kaorioshelper_upgrade_backup}"

rm -rf "$BACKUP_DIR"
mkdir -p "$BACKUP_DIR"

if [ -d "$CONFIG_DIR" ]; then
  ui_print "- Backing up existing user configuration..."
  cp -a "$CONFIG_DIR/." "$BACKUP_DIR/" 2>/dev/null || true
elif [ -d "$LEGACY_DIR" ]; then
  ui_print "- Found legacy configuration at $LEGACY_DIR; preparing migration..."
  mkdir -p "$BACKUP_DIR"
  [ -f "$LEGACY_DIR/tee/keybox.xml" ] && cp -a "$LEGACY_DIR/tee/keybox.xml" "$BACKUP_DIR/keybox.xml" 2>/dev/null || true
  [ -f "$LEGACY_DIR/copg/COPG.json" ] && cp -a "$LEGACY_DIR/copg/COPG.json" "$BACKUP_DIR/spoof.json" 2>/dev/null || true
fi

# ── 7. Module Extraction & Architecture Cleanup ──────────────────────────────
ui_print "- Setting up module binaries..."

# Extract module files into $MODPATH
mkdir -p "$MODPATH/data"
mkdir -p "$MODPATH/data/logs"

# Restore preserved configuration
if [ -d "$BACKUP_DIR" ] && [ "$(ls -A "$BACKUP_DIR" 2>/dev/null)" ]; then
  ui_print "- Restoring existing user configuration..."
  cp -a "$BACKUP_DIR/." "$MODPATH/data/" 2>/dev/null || true
fi
rm -rf "$BACKUP_DIR" 2>/dev/null || true

# Check existing user configuration; do not create fake PIF profiles
if [ ! -f "$MODPATH/data/pif.json" ]; then
  ui_print "! Note: PIF engine is unconfigured (pif.json absent)."
  ui_print "  Supply a valid custom profile via 'helperctl config set pif <file.json>'."
fi

[ -f "$MODPATH/data/spoof.json" ] || {
  cat > "$MODPATH/data/spoof.json" << 'EOF'
{
  "version": 1,
  "enabled": false,
  "cpu_spoof": {
    "blacklist": [],
    "cpu_only_packages": []
  },
  "profiles": {}
}
EOF
  [ -f "$MODPATH/data/copg.stored_gen" ] || printf '1\n' > "$MODPATH/data/copg.stored_gen"
}

[ -f "$MODPATH/data/hma.json" ] || {
  cat > "$MODPATH/data/hma.json" << 'EOF'
{
  "configVersion": 1,
  "detailLog": false,
  "webViewProtection": false,
  "templates": {},
  "settingsTemplates": {},
  "ignoredPackagesForPresets": [],
  "installSourceSpoof": [],
  "scope": {}
}
EOF
  [ -f "$MODPATH/data/hma.stored_gen" ] || printf '1\n' > "$MODPATH/data/hma.stored_gen"
}

# ABI binary selection (preserve both 64-bit and 32-bit ARM libraries on mixed-ABI devices)
if [ -d "$MODPATH/zygisk" ]; then
  ALL_ABIS="$ARCH $ABILIST"
  if ! echo "$ALL_ABIS" | grep -qE "arm64|aarch64"; then
    rm -f "$MODPATH/zygisk/arm64-v8a.so" 2>/dev/null || true
  fi
  if ! echo "$ALL_ABIS" | grep -qE "armeabi|armv7|arm"; then
    rm -f "$MODPATH/zygisk/armeabi-v7a.so" 2>/dev/null || true
  fi
  # Purge any legacy x86 / x86_64 artifacts
  rm -f "$MODPATH/zygisk/x86_64.so" "$MODPATH/zygisk/x86.so" 2>/dev/null || true
fi

# Multi-ABI helperctl installation (arm64-v8a, armeabi-v7a)
HELPERCTL_INSTALLED=false
for abi_cand in "$ARCH" $(echo "$ABILIST" | tr ',' ' '); do
  case "$abi_cand" in
    arm64*|aarch64*)
      if [ -f "$MODPATH/bin/arm64-v8a/helperctl" ]; then
        cp -f "$MODPATH/bin/arm64-v8a/helperctl" "$MODPATH/helperctl"
        ui_print "  Installed native helperctl (arm64-v8a)"
        HELPERCTL_INSTALLED=true
        break
      fi
      ;;
    armeabi*|armv7*|arm*)
      if [ -f "$MODPATH/bin/armeabi-v7a/helperctl" ]; then
        cp -f "$MODPATH/bin/armeabi-v7a/helperctl" "$MODPATH/helperctl"
        ui_print "  Installed native helperctl (armeabi-v7a)"
        HELPERCTL_INSTALLED=true
        break
      fi
      ;;
  esac
done

if ! $HELPERCTL_INSTALLED; then
  # Fallback to any available helperctl binary
  for fallback_bin in "$MODPATH/bin/"*"/helperctl"; do
    if [ -f "$fallback_bin" ]; then
      cp -f "$fallback_bin" "$MODPATH/helperctl"
      ui_print "  Installed fallback helperctl ($fallback_bin)"
      HELPERCTL_INSTALLED=true
      break
    fi
  done
fi
rm -rf "$MODPATH/bin" 2>/dev/null || true

# ── 8. Permissions & Contexts ────────────────────────────────────────────────
ui_print "- Enforcing permissions and security policies..."
apply_perm() {
  [ -e "$1" ] || return 0
  if command -v set_perm >/dev/null 2>&1; then
    set_perm "$1" 0 0 "$2" 2>/dev/null || chmod "$2" "$1" 2>/dev/null || true
  else
    chmod "$2" "$1" 2>/dev/null || true
  fi
}

apply_perm "$MODPATH/helperctl" 0755
for sh_file in "$MODPATH/customize.sh" "$MODPATH/post-fs-data.sh" "$MODPATH/service.sh" \
               "$MODPATH/uninstall.sh" "$MODPATH/action.sh" "$MODPATH/helperctl.sh"; do
  apply_perm "$sh_file" 0755
done
for so_file in "$MODPATH/zygisk/"*.so; do
  apply_perm "$so_file" 0644
done
apply_perm "$MODPATH/classes.dex" 0644

apply_perm "$MODPATH/data" 0700
apply_perm "$MODPATH/data/logs" 0700
for json_file in "$MODPATH/data/"*.json; do
  apply_perm "$json_file" 0600
done
apply_perm "$MODPATH/data/keybox.xml" 0600

# SELinux contexts
chcon -R u:object_r:system_file:s0 "$MODPATH" 2>/dev/null || true

ui_print "╔═════════════════════════════════════════════╗"
ui_print "║     KaoriosHelper Installed Successfully    ║"
ui_print "║      Reboot your device to activate         ║"
ui_print "╚═════════════════════════════════════════════╝"
