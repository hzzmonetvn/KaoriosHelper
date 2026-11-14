#!/system/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Standard root-manager Action entry point delegates to native control.
MODDIR="${0%/*}"
if [ -f "$MODDIR/helperctl.sh" ]; then
  sh "$MODDIR/helperctl.sh" config sync all || exit $?
  exec sh "$MODDIR/helperctl.sh" status
fi
printf '%s\n' 'KaoriosHelper: INCOMPLETE_RUNTIME (control interface missing)' >&2
exit 1
