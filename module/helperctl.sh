#!/system/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
# KaoriosHelper Unified Control CLI (helperctl)

set -u

MODDIR="${0%/*}"
[ -f "$MODDIR/module.prop" ] || MODDIR="/data/adb/modules/kaorioshelper"
DATA_DIR="$MODDIR/data"

# If native compiled C++ helperctl binary is present, delegate directly to it
ARCH="$(uname -m 2>/dev/null || echo '')"
if [ "$ARCH" = "x86_64" ] && [ -x "$MODDIR/bin/x86_64/helperctl" ]; then
  exec "$MODDIR/bin/x86_64/helperctl" "$@"
elif [ "$ARCH" = "aarch64" ] && [ -x "$MODDIR/bin/arm64-v8a/helperctl" ]; then
  exec "$MODDIR/bin/arm64-v8a/helperctl" "$@"
elif [ -x "$MODDIR/helperctl" ]; then
  exec "$MODDIR/helperctl" "$@"
fi

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
    local dex_exists="false"
    if [ -s "$DATA_DIR/classes.dex" ] || [ -s "$MODDIR/classes.dex" ]; then
      dex_exists="true"
    fi
    cat << EOF
{
  "schemaVersion": 1,
  "module": {
    "id": "kaorioshelper",
    "version": "$(sed -n 's/^version=//p' "$MODDIR/module.prop" 2>/dev/null || echo 'v1.0.0')",
    "installed": true,
    "active": $([ -f "$MODDIR/disable" ] && echo "false" || echo "true")
  },
  "engines": {
    "hma": {
      "installed": true,
      "enabled": $([ ! -f "$DATA_DIR/hma.disabled" ] && echo "true" || echo "false"),
      "configValid": $([ -s "$DATA_DIR/hma.json" ] && echo "true" || echo "false"),
      "runtimeAvailable": $dex_exists,
      "hookInitialized": $([ "$dex_exists" = "false" ] && echo '"INCOMPLETE_RUNTIME"' || echo '"NOT_VERIFIED"'),
      "appliedGeneration": $(cat "$DATA_DIR/generation.txt" 2>/dev/null || echo "0"),
      "pendingReload": $([ -f "$DATA_DIR/hma.reload_pending" ] && echo "true" || echo "false"),
      "deviceVerified": false,
      "lastError": $([ "$dex_exists" = "false" ] && echo '"classes.dex missing: upstream HMA DEX payload not bundled"' || echo '""'),
      "status": $([ "$dex_exists" = "false" ] && echo '"INCOMPLETE_RUNTIME"' || echo '"NOT_VERIFIED"'),
      "details": "system_server package protection"
    },
    "pif": {
      "installed": true,
      "enabled": $([ ! -f "$DATA_DIR/pif.disabled" ] && echo "true" || echo "false"),
      "configValid": $([ -s "$DATA_DIR/pif.json" ] && echo "true" || echo "false"),
      "runtimeAvailable": true,
      "hookInitialized": $([ -f "$DATA_DIR/runtime/pif.ack" ] && echo '"INITIALIZED"' || echo '"NOT_VERIFIED"'),
      "appliedGeneration": $(cat "$DATA_DIR/generation.txt" 2>/dev/null || echo "0"),
      "pendingReload": $([ -f "$DATA_DIR/pif.reload_pending" ] && echo "true" || echo "false"),
      "deviceVerified": $([ -f "$DATA_DIR/runtime/pif.ack" ] && echo "true" || echo "false"),
      "lastError": $([ -f "$DATA_DIR/runtime/pif.ack" ] && echo '""' || echo '"Runtime hook acknowledgement pending target process specialization"'),
      "status": $([ -f "$DATA_DIR/runtime/pif.ack" ] && echo '"ACTIVE"' || echo '"NOT_VERIFIED"'),
      "details": "Play Integrity property interceptor"
    },
    "tee": {
      "installed": true,
      "enabled": $([ -f "$DATA_DIR/tee.enabled" ] && echo "true" || echo "false"),
      "configValid": $([ -s "$DATA_DIR/keybox.xml" ] && echo "true" || echo "false"),
      "runtimeAvailable": false,
      "hookInitialized": "INCOMPLETE_RUNTIME",
      "appliedGeneration": $(cat "$DATA_DIR/generation.txt" 2>/dev/null || echo "0"),
      "pendingReload": $([ -f "$DATA_DIR/tee.reload_pending" ] && echo "true" || echo "false"),
      "deviceVerified": false,
      "lastError": "Keystore2 system daemon interception runtime unavailable",
      "status": "INCOMPLETE_RUNTIME",
      "details": "Keybox XML validated, Keystore2 daemon ptrace/AIDL is not bundled"
    },
    "copg": {
      "installed": true,
      "enabled": $([ -f "$DATA_DIR/copg.enabled" ] && echo "true" || echo "false"),
      "configValid": $([ -s "$DATA_DIR/spoof.json" ] || [ -s "$DATA_DIR/COPG.json" ] && echo "true" || echo "false"),
      "runtimeAvailable": true,
      "hookInitialized": $([ -f "$DATA_DIR/runtime/copg.ack" ] && echo '"INITIALIZED"' || echo '"NOT_VERIFIED"'),
      "appliedGeneration": $(cat "$DATA_DIR/generation.txt" 2>/dev/null || echo "0"),
      "pendingReload": $([ -f "$DATA_DIR/copg.reload_pending" ] && echo "true" || echo "false"),
      "deviceVerified": $([ -f "$DATA_DIR/runtime/copg.ack" ] && echo "true" || echo "false"),
      "lastError": $([ -f "$DATA_DIR/runtime/copg.ack" ] && echo '""' || echo '"Runtime hook acknowledgement pending target app specialization"'),
      "status": $([ -f "$DATA_DIR/runtime/copg.ack" ] && echo '"ACTIVE"' || echo '"NOT_VERIFIED"'),
      "details": "Game profile virtualization"
    }
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
  echo "Subsystem Engines (Verified Runtime Assessment):"

  # HMA
  if [ -f "$DATA_DIR/hma.disabled" ]; then
    echo "  • HMA Engine:  DISABLED"
  elif [ ! -s "$DATA_DIR/classes.dex" ] && [ ! -s "$MODDIR/classes.dex" ]; then
    echo "  • HMA Engine:  INCOMPLETE_RUNTIME (classes.dex missing)"
  elif [ -f "$DATA_DIR/runtime/hma.ack" ]; then
    echo "  • HMA Engine:  ACTIVE (Verified)"
  else
    echo "  • HMA Engine:  NOT_VERIFIED (Pending system_server launch)"
  fi

  # PIF
  if [ -f "$DATA_DIR/pif.disabled" ]; then
    echo "  • PIF Engine:  DISABLED"
  elif [ ! -s "$DATA_DIR/pif.json" ]; then
    echo "  • PIF Engine:  NOT_CONFIGURED (pif.json missing)"
  elif [ -f "$DATA_DIR/runtime/pif.ack" ]; then
    echo "  • PIF Engine:  ACTIVE (Verified)"
  else
    echo "  • PIF Engine:  NOT_VERIFIED (Pending GMS specialization)"
  fi

  # TEE
  if [ ! -f "$DATA_DIR/tee.enabled" ]; then
    echo "  • TEE Engine:  DISABLED (Default)"
  elif [ ! -s "$DATA_DIR/keybox.xml" ]; then
    echo "  • TEE Engine:  NOT_CONFIGURED (keybox.xml missing)"
  else
    echo "  • TEE Engine:  INCOMPLETE_RUNTIME (Keystore2 daemon interception not bundled)"
  fi

  # COPG
  if [ ! -f "$DATA_DIR/copg.enabled" ]; then
    echo "  • COPG Engine: DISABLED (Default)"
  elif [ ! -s "$DATA_DIR/spoof.json" ] && [ ! -s "$DATA_DIR/COPG.json" ]; then
    echo "  • COPG Engine: NOT_CONFIGURED (spoof.json missing)"
  elif [ -f "$DATA_DIR/runtime/copg.ack" ]; then
    echo "  • COPG Engine: ACTIVE (Verified)"
  else
    echo "  • COPG Engine: NOT_VERIFIED (Pending target app launch)"
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
