#!/system/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# One-shot status publication; no property mutation or speculative daemons.
MODDIR="${0%/*}"
[ ! -f "$MODDIR/disable" ] || exit 0
umask 077
if [ -f "$MODDIR/helperctl.sh" ]; then
  exec sh "$MODDIR/helperctl.sh" status --json
fi
printf '%s\n' 'KaoriosHelper: INCOMPLETE_RUNTIME (control interface missing)' >&2
exit 1
