#!/system/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
# KaoriosHelper Unified Control CLI (helperctl)

set -u

MODDIR="${0%/*}"
[ -f "$MODDIR/module.prop" ] || MODDIR="/data/adb/modules/kaorioshelper"
DATA_DIR="$MODDIR/data"

JSON_MODE=false
for arg in "$@"; do
  if [ "$arg" = "--json" ]; then
    JSON_MODE=true
  fi
done

print_json_status() {
  if [ -f "$DATA_DIR/status.json" ]; then
    cat "$DATA_DIR/status.json"
  else
    cat << EOF
{
  "module": {
    "id": "kaorioshelper",
    "version": "$(sed -n 's/^version=//p' "$MODDIR/module.prop" 2>/dev/null || echo 'v1.0.0')",
    "installed": true
  },
  "engines": {
    "hma": {"status": "APPLIED", "enabled": true},
    "pif": {"status": "APPLIED", "enabled": true},
    "tee": {"status": "NOT_CONFIGURED", "enabled": false},
    "copg": {"status": "NOT_CONFIGURED", "enabled": false}
  }
}
EOF
  fi
}

cmd_status() {
  if $JSON_MODE; then
    print_json_status
    return 0
  fi

  echo "==============================================="
  echo "         KaoriosHelper Runtime Status          "
  echo "==============================================="
  echo "Module Version: $(sed -n 's/^version=//p' "$MODDIR/module.prop" 2>/dev/null || echo 'v1.0.0')"

  if [ -f "$MODDIR/disable" ]; then
    echo "Module State: DISABLED"
  else
    echo "Module State: ACTIVE"
  fi

  echo ""
  echo "Subsystem Engines:"

  # HMA
  if [ -f "$DATA_DIR/hma.disabled" ]; then
    echo "  • HMA Engine:  DISABLED"
  else
    echo "  • HMA Engine:  ACTIVE (system_server)"
  fi

  # PIF
  if [ -f "$DATA_DIR/pif.disabled" ]; then
    echo "  • PIF Engine:  DISABLED"
  else
    echo "  • PIF Engine:  ACTIVE (GMS/DroidGuard)"
  fi

  # TEE
  if [ -f "$DATA_DIR/tee.enabled" ]; then
    if [ -s "$DATA_DIR/keybox.xml" ]; then
      echo "  • TEE Engine:  ACTIVE (Keybox Loaded)"
    else
      echo "  • TEE Engine:  ENABLED (Keybox Missing: $DATA_DIR/keybox.xml)"
    fi
  else
    echo "  • TEE Engine:  DISABLED (Default)"
  fi

  # COPG
  if [ -f "$DATA_DIR/copg.enabled" ]; then
    echo "  • COPG Engine: ACTIVE"
  else
    echo "  • COPG Engine: DISABLED (Default)"
  fi

  echo ""
  echo "Storage Contract Directory: $DATA_DIR"
  echo "==============================================="
}

cmd_version() {
  local ver
  ver=$(sed -n 's/^version=//p' "$MODDIR/module.prop" 2>/dev/null || echo 'v1.0.0')
  if $JSON_MODE; then
    echo "{\"version\": \"$ver\", \"moduleId\": \"kaorioshelper\"}"
  else
    echo "KaoriosHelper $ver"
  fi
}

cmd_enable() {
  local eng="$1"
  case "$eng" in
    tee|copg)
      touch "$DATA_DIR/$eng.enabled"
      touch "$DATA_DIR/$eng.reload_pending"
      echo "Engine '$eng' enabled."
      ;;
    hma|pif)
      rm -f "$DATA_DIR/$eng.disabled"
      touch "$DATA_DIR/$eng.reload_pending"
      echo "Engine '$eng' enabled."
      ;;
    *)
      echo "Unknown engine: $eng (valid: hma, pif, tee, copg)"
      return 1
      ;;
  esac
}

cmd_disable() {
  local eng="$1"
  case "$eng" in
    tee|copg)
      rm -f "$DATA_DIR/$eng.enabled"
      touch "$DATA_DIR/$eng.reload_pending"
      echo "Engine '$eng' disabled."
      ;;
    hma|pif)
      touch "$DATA_DIR/$eng.disabled"
      touch "$DATA_DIR/$eng.reload_pending"
      echo "Engine '$eng' disabled."
      ;;
    *)
      echo "Unknown engine: $eng (valid: hma, pif, tee, copg)"
      return 1
      ;;
  esac
}

cmd_reload() {
  local eng="${1:-all}"
  echo "Reload requested for: $eng"
  if [ "$eng" = "all" ]; then
    rm -f "$DATA_DIR"/*.reload_pending
  else
    rm -f "$DATA_DIR/$eng.reload_pending"
  fi
  echo "Reload acknowledged."
}

cmd_diagnostics() {
  echo "KaoriosHelper Diagnostic Information:"
  echo "ABI: $(getprop ro.product.cpu.abi)"
  echo "SDK: $(getprop ro.build.version.sdk)"
  echo "Verified Boot: $(getprop ro.boot.verifiedbootstate)"
  echo "VBMeta State:  $(getprop ro.boot.vbmeta.device_state)"
  echo ""
  echo "Active Configuration Files:"
  ls -la "$DATA_DIR"/*.json "$DATA_DIR"/*.xml 2>/dev/null || true
}

# ── Main CLI Dispatcher ──────────────────────────────────────────────────────
ACTION="${1:-status}"
shift 2>/dev/null || true

case "$ACTION" in
  status)
    cmd_status
    ;;
  version)
    cmd_version
    ;;
  enable)
    [ $# -ge 1 ] || { echo "Usage: helperctl enable <hma|pif|tee|copg>"; exit 1; }
    cmd_enable "$1"
    ;;
  disable)
    [ $# -ge 1 ] || { echo "Usage: helperctl disable <hma|pif|tee|copg>"; exit 1; }
    cmd_disable "$1"
    ;;
  reload)
    cmd_reload "${1:-all}"
    ;;
  diagnostics)
    cmd_diagnostics
    ;;
  *)
    echo "Usage: helperctl {status|version|enable|disable|reload|diagnostics} [--json]"
    exit 1
    ;;
esac
