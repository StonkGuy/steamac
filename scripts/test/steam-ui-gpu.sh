#!/bin/bash
# SPDX-License-Identifier: MIT
# guest/layer/usr/lib/steamac/steam-ui-gpu (steamac.steam_ui_gpu= on the kernel command line) and its unit wiring:
#   default  no option: the env file sets STEAM_EXTRA_ARGS=-cef-disable-gpu
#   user     the user's STEAM_EXTRA_ARGS (extra_args.conf drop-in) are kept, -cef-disable-gpu appended once
#   gpu      steamac.steam_ui_gpu=1: no env file (a stale one from an earlier start is removed)
#   other    steamac.steam_ui_gpu=0 behaves like the default
#   unit     steam.service runs the script before Steam and reads the env file
# usage: scripts/test/steam-ui-gpu.sh
set -u
R=$(cd "$(dirname "$0")/../.." && pwd)/guest/layer
S=$R/usr/lib/steamac/steam-ui-gpu
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fails=0; fail() { fails=$((fails + 1)); echo "FAIL [$1]: $2"; }
E=$T/run/steamac/steam-extra-args.env

run() { # cmdline [STEAM_EXTRA_ARGS]
    printf '%s\n' "$1" > "$T/cmdline"
    STEAM_EXTRA_ARGS=${2:-} STEAMAC_CMDLINE=$T/cmdline STEAMAC_STEAM_ENV=$E bash "$S" > "$T/out" 2>&1
}

run "console=hvc0 rootwait" || fail default "exit status $?"
[ "$(cat "$E" 2>/dev/null)" = "STEAM_EXTRA_ARGS=-cef-disable-gpu" ] || fail default "env file '$(cat "$E" 2>/dev/null)'"

run "console=hvc0" "-inhibitbootstrap -nobootstrapperupdate" || fail user "exit status $?"
[ "$(cat "$E" 2>/dev/null)" = "STEAM_EXTRA_ARGS=-inhibitbootstrap -nobootstrapperupdate -cef-disable-gpu" ] \
    || fail user "env file '$(cat "$E" 2>/dev/null)'"
run "console=hvc0" "-cef-disable-gpu -silent" || fail user "exit status $?"
[ "$(cat "$E" 2>/dev/null)" = "STEAM_EXTRA_ARGS=-cef-disable-gpu -silent" ] || fail user "argument doubled: '$(cat "$E")'"

run "console=hvc0 steamac.steam_ui_gpu=1 rootwait" || fail gpu "exit status $?"
[ -e "$E" ] && fail gpu "env file left although steamac.steam_ui_gpu=1"

run "steamac.steam_ui_gpu=0" || fail other "exit status $?"
grep -qx 'STEAM_EXTRA_ARGS=-cef-disable-gpu' "$E" 2>/dev/null || fail other "env file not written"

C=$R/usr/lib/systemd/user/steam.service.d/50-steamac.conf
grep -qx 'ExecStartPre=-/usr/lib/steamac/steam-ui-gpu' "$C" || fail unit "ExecStartPre missing"
grep -qx 'EnvironmentFile=-%t/steamac/steam-extra-args.env' "$C" || fail unit "EnvironmentFile missing"
[ -x "$S" ] && bash -n "$S" || fail unit "script not executable or invalid"

[ $fails -eq 0 ] && echo "steam-ui-gpu: all PASS" || { echo "steam-ui-gpu: $fails FAIL"; exit 1; }
