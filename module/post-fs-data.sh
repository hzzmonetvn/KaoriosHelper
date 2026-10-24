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

# NOTICE: Global device-wide properties (verified boot, flash lock, crypto state)
# are NOT modified unconditionally during boot.
# Security property spoofing is scoped strictly to targeted client processes via
# the Zygisk PIF engine (in-memory Bionic libc interception), preserving the device's
# genuine security state and encryption for all other processes.

