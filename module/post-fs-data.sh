#!/system/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Prepare private runtime storage without changing Android properties.
MODDIR="${0%/*}"
DATA_DIR="$MODDIR/data"
[ ! -L "$DATA_DIR" ] || exit 1
[ ! -L "$DATA_DIR/logs" ] || exit 1
[ ! -L "$DATA_DIR/runtime" ] || exit 1
umask 077
mkdir -p "$DATA_DIR/logs" || exit 1
chmod 0700 "$DATA_DIR" "$DATA_DIR/logs" || exit 1
# Evidence from a previous boot must never establish current runtime state.
rm -f "$DATA_DIR"/*.verified "$DATA_DIR/runtime/"*.ack.json "$DATA_DIR/runtime/"*.json
