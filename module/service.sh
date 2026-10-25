#!/system/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
# KaoriosHelper Late Boot Service (service.sh)

MODDIR="${0%/*}"
DATA_DIR="$MODDIR/data"
LOG_FILE="$DATA_DIR/logs/service.log"

log() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG_FILE" 2>/dev/null
}

log "KaoriosHelper service.sh initializing..."

# Wait for boot completion before starting persistent daemons
until [ "$(getprop sys.boot_completed)" = "1" ]; do
  sleep 2
done

log "Boot completed. Initializing RuntimeManager..."

# ── Late-boot Property Re-enforcement (AlwaysStrong / PIF parity) ────────────
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

resetprop_late_if_diff() {
  local prop="$1"
  local val="$2"
  [ -z "$prop" ] || [ -z "$val" ] && return
  local cur
  cur="$($RESETPROP "$prop" 2>/dev/null)"
  if [ "$cur" != "$val" ]; then
    $RESETPROP -n "$prop" "$val" 2>/dev/null || $RESETPROP "$prop" "$val" 2>/dev/null || true
  fi
}

resetprop_late_if_diff "ro.boot.verifiedbootstate" "green"
resetprop_late_if_diff "ro.boot.flash.locked" "1"
resetprop_late_if_diff "ro.boot.vbmeta.device_state" "locked"
resetprop_late_if_diff "ro.boot.veritymode" "enforcing"
resetprop_late_if_diff "sys.oem_unlock_allowed" "0"

# ── Status Initialization ───────────────────────────────────────────────────
if [ -x "$MODDIR/helperctl" ]; then
  "$MODDIR/helperctl" reload all >/dev/null 2>&1 || true
  "$MODDIR/helperctl" status >/dev/null 2>&1 || true
else
  cat > "$DATA_DIR/status.json" << EOF
{
  "schemaVersion": 1,
  "moduleVersion": "$(sed -n 's/^version=//p' "$MODDIR/module.prop" 2>/dev/null || echo "v1.0.0")",
  "moduleState": "$([ -f "$MODDIR/disable" ] && echo "DISABLED" || echo "ACTIVE")",
  "bootCompleted": true,
  "serviceRunning": true,
  "engines": {
    "hma": {
      "installed": true,
      "enabled": $([ ! -f "$DATA_DIR/hma.disabled" ] && echo "true" || echo "false"),
      "configValid": $([ -s "$DATA_DIR/hma.json" ] && echo "true" || echo "false"),
      "runtimeAvailable": $([ -s "$DATA_DIR/classes.dex" ] || [ -s "$MODDIR/classes.dex" ] && echo "true" || echo "false"),
      "hookInitialized": $([ ! -s "$DATA_DIR/classes.dex" ] && [ ! -s "$MODDIR/classes.dex" ] && echo '"INCOMPLETE_RUNTIME"' || echo '"NOT_VERIFIED"'),
      "appliedGeneration": $(cat "$DATA_DIR/generation.txt" 2>/dev/null || echo "0"),
      "pendingReload": $([ -f "$DATA_DIR/hma.reload_pending" ] && echo "true" || echo "false"),
      "runtimeObserved": $([ -f "$DATA_DIR/runtime/hma.ack" ] || [ -f "$DATA_DIR/runtime/hma.ack.json" ] && echo "true" || echo "false"),
      "deviceVerified": $([ -f "$DATA_DIR/hma.verified" ] && echo "true" || echo "false"),
      "lastError": $([ ! -s "$DATA_DIR/classes.dex" ] && [ ! -s "$MODDIR/classes.dex" ] && echo '"classes.dex missing: upstream HMA DEX payload not bundled"' || echo '""'),
      "status": $([ ! -s "$DATA_DIR/classes.dex" ] && [ ! -s "$MODDIR/classes.dex" ] && echo '"INCOMPLETE_RUNTIME"' || echo '"NOT_VERIFIED"'),
      "details": "system_server in-process protection"
    },
    "pif": {
      "installed": true,
      "enabled": $([ ! -f "$DATA_DIR/pif.disabled" ] && echo "true" || echo "false"),
      "configValid": $([ -s "$DATA_DIR/pif.json" ] && echo "true" || echo "false"),
      "runtimeAvailable": true,
      "hookInitialized": $([ -f "$DATA_DIR/runtime/pif.ack" ] || [ -f "$DATA_DIR/runtime/pif.ack.json" ] && echo '"INITIALIZED"' || echo '"NOT_VERIFIED"'),
      "appliedGeneration": $(cat "$DATA_DIR/generation.txt" 2>/dev/null || echo "0"),
      "pendingReload": $([ -f "$DATA_DIR/pif.reload_pending" ] && echo "true" || echo "false"),
      "runtimeObserved": $([ -f "$DATA_DIR/runtime/pif.ack" ] || [ -f "$DATA_DIR/runtime/pif.ack.json" ] && echo "true" || echo "false"),
      "deviceVerified": $([ -f "$DATA_DIR/pif.verified" ] && echo "true" || echo "false"),
      "lastError": $([ -f "$DATA_DIR/runtime/pif.ack" ] || [ -f "$DATA_DIR/runtime/pif.ack.json" ] && echo '""' || echo '"Runtime hook acknowledgement pending target process specialization"'),
      "status": $([ -f "$DATA_DIR/runtime/pif.ack" ] || [ -f "$DATA_DIR/runtime/pif.ack.json" ] && echo '"ACTIVE"' || echo '"NOT_VERIFIED"'),
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
      "runtimeObserved": false,
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
      "hookInitialized": $([ -f "$DATA_DIR/runtime/copg.ack" ] || [ -f "$DATA_DIR/runtime/copg.ack.json" ] && echo '"INITIALIZED"' || echo '"NOT_VERIFIED"'),
      "appliedGeneration": $(cat "$DATA_DIR/generation.txt" 2>/dev/null || echo "0"),
      "pendingReload": $([ -f "$DATA_DIR/copg.reload_pending" ] && echo "true" || echo "false"),
      "runtimeObserved": $([ -f "$DATA_DIR/runtime/copg.ack" ] || [ -f "$DATA_DIR/runtime/copg.ack.json" ] && echo "true" || echo "false"),
      "deviceVerified": $([ -f "$DATA_DIR/copg.verified" ] && echo "true" || echo "false"),
      "lastError": $([ -f "$DATA_DIR/runtime/copg.ack" ] || [ -f "$DATA_DIR/runtime/copg.ack.json" ] && echo '""' || echo '"Runtime hook acknowledgement pending target app specialization"'),
      "status": $([ -f "$DATA_DIR/runtime/copg.ack" ] || [ -f "$DATA_DIR/runtime/copg.ack.json" ] && echo '"ACTIVE"' || echo '"NOT_VERIFIED"'),
      "details": "Game profile virtualization"
    }
  },
  "lastUpdate": "$(date '+%Y-%m-%d %H:%M:%S')"
}
EOF
fi

# ── Start TEE Engine if enabled ─────────────────────────────────────────────
if [ -f "$DATA_DIR/tee.enabled" ]; then
  if [ -x "$MODDIR/tee/supervisor" ]; then
    log "Starting TEE supervisor daemon..."
    "$MODDIR/tee/supervisor" "$MODDIR/tee/daemon" "$MODDIR/tee" >/dev/null 2>&1 &
    echo "$!" > "$DATA_DIR/tee-supervisor.pid"
  else
    log "TEE enabled but native supervisor binary ($MODDIR/tee/supervisor) not bundled."
  fi
fi

# ── Start COPG Controller if enabled ────────────────────────────────────────
if [ -f "$DATA_DIR/copg.enabled" ]; then
  if [ -x "$MODDIR/copg/controller" ]; then
    log "Starting COPG unified controller..."
    "$MODDIR/copg/controller" >/dev/null 2>&1 &
    echo "$!" > "$DATA_DIR/copg-controller.pid"
  else
    log "COPG enabled but native controller binary ($MODDIR/copg/controller) not bundled."
  fi
fi

log "KaoriosHelper background initialization complete."
