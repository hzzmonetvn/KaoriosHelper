#!/system/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
# KaoriosHelper Manager Action Trigger (action.sh)

MODDIR="${0%/*}"

if [ -f "$MODDIR/helperctl.sh" ]; then
  exec sh "$MODDIR/helperctl.sh" status
else
  echo "KaoriosHelper: Active"
  exit 0
fi
