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

# ── Bootloader & Verified Boot Properties (Attestation rootOfTrust hardening) ──
resetprop_if_diff() {
  local prop="$1"
  local val="$2"
  local cur
  cur=$(getprop "$prop" 2>/dev/null)
  if [ "$cur" != "$val" ]; then
    resetprop -n "$prop" "$val" 2>/dev/null || true
  fi
}

# Android standard verified boot
resetprop_if_diff ro.boot.verifiedbootstate green
resetprop_if_diff vendor.boot.verifiedbootstate green
resetprop_if_diff ro.boot.vbmeta.device_state locked
resetprop_if_diff vendor.boot.vbmeta.device_state locked

# OEM-specific bootloader flags
resetprop_if_diff ro.boot.warranty_bit 0
resetprop_if_diff ro.vendor.boot.warranty_bit 0
resetprop_if_diff ro.vendor.warranty_bit 0
resetprop_if_diff ro.warranty_bit 0
resetprop_if_diff ro.boot.realmebootstate green
resetprop_if_diff ro.is_ever_orange 0

# Crypto and flash states
resetprop_if_diff ro.crypto.state encrypted
resetprop_if_diff ro.boot.flash.locked 1
