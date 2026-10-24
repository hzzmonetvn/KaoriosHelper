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

# Re-assert lock state properties after late vendor init runs
if [ -f "$MODDIR/post-fs-data.sh" ]; then
  sh "$MODDIR/post-fs-data.sh" 2>/dev/null || true
fi

# ── Status Initialization ───────────────────────────────────────────────────
if [ -x "$MODDIR/helperctl" ]; then
  "$MODDIR/helperctl" reload all >/dev/null 2>&1 || true
else
  cat > "$DATA_DIR/status.json" << EOF
{
  "moduleVersion": "$(sed -n 's/^version=//p' "$MODDIR/module.prop" 2>/dev/null || echo "v1.0.0")",
  "bootCompleted": true,
  "serviceRunning": true,
  "engines": {
    "hma": {
      "enabled": $([ ! -f "$DATA_DIR/hma.disabled" ] && echo "true" || echo "false"),
      "status": $([ ! -f "$DATA_DIR/hma.disabled" ] && echo '"CONFIGURED_NOT_HOOKED"' || echo '"DISABLED"')
    },
    "pif": {
      "enabled": $([ ! -f "$DATA_DIR/pif.disabled" ] && echo "true" || echo "false"),
      "status": $([ ! -f "$DATA_DIR/pif.disabled" ] && echo '"PARTIALLY_APPLIED"' || echo '"DISABLED"')
    },
    "tee": {
      "enabled": $([ -f "$DATA_DIR/tee.enabled" ] && echo "true" || echo "false"),
      "status": $([ -f "$DATA_DIR/tee.enabled" ] && [ -s "$DATA_DIR/keybox.xml" ] && echo '"STUB_NO_KEYMINT_AIDL"' || echo '"NOT_CONFIGURED"')
    },
    "copg": {
      "enabled": $([ -f "$DATA_DIR/copg.enabled" ] && echo "true" || echo "false"),
      "status": $([ -f "$DATA_DIR/copg.enabled" ] && echo '"TARGETS_CONFIGURED_STUB_HOOK"' || echo '"NOT_CONFIGURED"')
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
