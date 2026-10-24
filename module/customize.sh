#!/system/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
# KaoriosHelper Module Installation Script

set -u

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
if command -v apd >/dev/null 2>&1 || [ -f /data/adb/apd ]; then
  ROOT_SOLUTION="APatch"
elif command -v ksud >/dev/null 2>&1 || [ -f /data/adb/ksud ]; then
  ROOT_SOLUTION="KernelSU"
elif command -v magisk >/dev/null 2>&1 || [ -f /data/adb/magisk/magisk ]; then
  ROOT_SOLUTION="Magisk"
else
  ROOT_SOLUTION="Generic Root"
fi
ui_print "  Detected Root Manager: $ROOT_SOLUTION"

# ── 2. Android Version Check ─────────────────────────────────────────────────
API_LEVEL=$(getprop ro.build.version.sdk)
[ -n "$API_LEVEL" ] || API_LEVEL=0
ui_print "  Android SDK API Level: $API_LEVEL"

if [ "$API_LEVEL" -lt 31 ]; then
  ui_print "! Warning: KaoriosHelper targets Android 12+ (API 31-35+)."
  ui_print "! Early Android versions (<12) may have limited TEE/PMS support."
fi

# ── 3. Architecture Validation ───────────────────────────────────────────────
ARCH=$(getprop ro.product.cpu.abi)
ABILIST=$(getprop ro.product.cpu.abilist)
ui_print "  Primary Device ABI: $ARCH ($ABILIST)"

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
CONFIG_DIR="/data/adb/modules/kaorioshelper/data"
LEGACY_DIR="/data/adb/kaorios_helper"
BACKUP_DIR="/data/adb/kaorioshelper_upgrade_backup"

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
  rm -rf "$BACKUP_DIR"
fi

# Ensure canonical configuration templates exist if not already configured
[ -f "$MODPATH/data/pif.json" ] || cat > "$MODPATH/data/pif.json" << 'EOF'
{
  "MANUFACTURER": "Google",
  "MODEL": "Pixel 7 Pro",
  "FINGERPRINT": "google/cheetah_beta/cheetah:CANARY/ZP11.260918.007/16484274:user/release-keys",
  "BRAND": "google",
  "PRODUCT": "cheetah_beta",
  "DEVICE": "cheetah",
  "RELEASE": "14",
  "ID": "ZP11.260918.007",
  "INCREMENTAL": "16484274",
  "TYPE": "user",
  "TAGS": "release-keys",
  "SECURITY_PATCH": "2026-10-05",
  "DEVICE_INITIAL_SDK_INT": "33",
  "spoofBuild": 1,
  "spoofProps": 1,
  "spoofProvider": 0,
  "spoofSignature": 0
}
EOF

[ -f "$MODPATH/data/spoof.json" ] || cat > "$MODPATH/data/spoof.json" << 'EOF'
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

[ -f "$MODPATH/data/hma.json" ] || cat > "$MODPATH/data/hma.json" << 'EOF'
{
  "configVersion": 2,
  "detailLog": false,
  "errorOnlyLog": false,
  "maxLogSize": 512,
  "forceMountData": true,
  "disableActivityLaunchProtection": false,
  "altAppDataIsolation": false,
  "altVoldAppDataIsolation": false,
  "skipSystemAppDataIsolation": true,
  "webViewProtection": true,
  "ignoreBrandProtections": false,
  "templates": {},
  "settingsTemplates": {},
  "disabledHooks": [],
  "scope": {}
}
EOF

# ABI binary selection
if [ -d "$MODPATH/zygisk" ]; then
  if echo "$ABILIST" | grep -qv "x86_64"; then
    rm -f "$MODPATH/zygisk/x86_64.so" 2>/dev/null
  fi
  if echo "$ABILIST" | grep -qvE "armeabi|armv7|arm"; then
    rm -f "$MODPATH/zygisk/armeabi-v7a.so" 2>/dev/null
  fi
fi

# Multi-ABI helperctl installation
if [ "$ARCH" = "x64" ] || [ "$ARCH" = "x86_64" ] || echo "$ARCH" | grep -q "x86_64"; then
  if [ -f "$MODPATH/bin/x86_64/helperctl" ]; then
    cp -f "$MODPATH/bin/x86_64/helperctl" "$MODPATH/helperctl"
    ui_print "  Installed native helperctl (x86_64)"
  fi
elif [ "$ARCH" = "arm64" ] || [ "$ARCH" = "arm64-v8a" ] || echo "$ARCH" | grep -q "arm64"; then
  if [ -f "$MODPATH/bin/arm64-v8a/helperctl" ]; then
    cp -f "$MODPATH/bin/arm64-v8a/helperctl" "$MODPATH/helperctl"
    ui_print "  Installed native helperctl (arm64-v8a)"
  fi
fi
rm -rf "$MODPATH/bin" 2>/dev/null || true
chmod 0755 "$MODPATH/helperctl" 2>/dev/null || true

# ── 8. Permissions & Contexts ────────────────────────────────────────────────
ui_print "- Enforcing permissions and security policies..."
chmod 0755 "$MODPATH/customize.sh" 2>/dev/null || true
chmod 0755 "$MODPATH/post-fs-data.sh" 2>/dev/null || true
chmod 0755 "$MODPATH/service.sh" 2>/dev/null || true
chmod 0755 "$MODPATH/uninstall.sh" 2>/dev/null || true
chmod 0755 "$MODPATH/action.sh" 2>/dev/null || true
chmod 0755 "$MODPATH/helperctl.sh" 2>/dev/null || true

chmod 0700 "$MODPATH/data" 2>/dev/null || true
chmod 0700 "$MODPATH/data/logs" 2>/dev/null || true
chmod 0600 "$MODPATH/data/"*.json 2>/dev/null || true
[ -f "$MODPATH/data/keybox.xml" ] && chmod 0600 "$MODPATH/data/keybox.xml" 2>/dev/null || true

# SELinux contexts
chcon -R u:object_r:system_file:s0 "$MODPATH" 2>/dev/null || true

ui_print "╔═════════════════════════════════════════════╗"
ui_print "║     KaoriosHelper Installed Successfully    ║"
ui_print "║      Reboot your device to activate         ║"
ui_print "╚═════════════════════════════════════════════╝"
