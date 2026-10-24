#!/system/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
# KaoriosHelper Uninstallation Script (uninstall.sh)

MODDIR="${0%/*}"
DATA_DIR="$MODDIR/data"

# Stop running daemons
for pidfile in "$DATA_DIR"/*.pid; do
  if [ -f "$pidfile" ]; then
    pid=$(cat "$pidfile" 2>/dev/null)
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      kill -TERM "$pid" 2>/dev/null || true
    fi
    rm -f "$pidfile"
  fi
done

# Back up user configuration to preserve sensitive keys across uninstalls
if [ -d "$DATA_DIR" ]; then
  mkdir -p /data/adb/kaorioshelper_backup
  cp -a "$DATA_DIR/." /data/adb/kaorioshelper_backup/ 2>/dev/null || true
fi

exit 0
