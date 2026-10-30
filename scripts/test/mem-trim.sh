#!/bin/bash
# SPDX-License-Identifier: MIT
# guest/layer/usr/lib/steamac/mem-trim (steamac.mem_trim= on the kernel command line) and its units:
#   default  no option: writes exactly "1" (clean page cache only) to drop_caches
#   off      steamac.mem_trim=0: drop_caches is not touched
#   other    another value (steamac.mem_trim=1) behaves like the default
#   units    the timer is enabled (timers.target.wants), fires once per boot after boot settles,
#            and starts the oneshot service that runs the script
#   games    "mem-trim games": nothing while a game scope exists or when none ever did; one trim (drop_caches 1,
#            compact_memory 1) on the first poll after the last game is gone, none on the next; off with
#            steamac.mem_trim=0; its service is enabled and runs "games"
# usage: scripts/test/mem-trim.sh
set -u
R=$(cd "$(dirname "$0")/../.." && pwd)/guest/layer
S=$R/usr/lib/steamac/mem-trim
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fails=0; fail() { fails=$((fails + 1)); echo "FAIL [$1]: $2"; }
printf 'MemTotal: 8108884 kB\nMemFree: 1000 kB\n' > "$T/meminfo"

run() { # cmdline
    printf '%s\n' "$1" > "$T/cmdline"; rm -f "$T/drop"
    STEAMAC_CMDLINE=$T/cmdline STEAMAC_DROP_CACHES=$T/drop STEAMAC_MEMINFO=$T/meminfo bash "$S" > "$T/out" 2>&1
}

run "console=hvc0 rootwait" || fail default "exit status $?"
[ "$(cat "$T/drop" 2>/dev/null)" = 1 ] || fail default "drop_caches got '$(cat "$T/drop" 2>/dev/null)', want 1"

run "console=hvc0 steamac.mem_trim=0 rootwait" || fail off "exit status $?"
[ -e "$T/drop" ] && fail off "drop_caches written although steamac.mem_trim=0"

run "steamac.mem_trim=1" || fail other "exit status $?"
[ "$(cat "$T/drop" 2>/dev/null)" = 1 ] || fail other "drop_caches not written"

U=$R/usr/lib/systemd/system
[ "$(readlink "$U/timers.target.wants/steamac-mem-trim.timer")" = ../steamac-mem-trim.timer ] || fail units "timer not enabled"
grep -qx 'OnBootSec=150s' "$U/steamac-mem-trim.timer" || fail units "timer is not a one-shot after boot"
grep -q '^OnUnitActiveSec\|^OnCalendar' "$U/steamac-mem-trim.timer" && fail units "timer repeats (must run once per boot)"
grep -qx 'Type=oneshot' "$U/steamac-mem-trim.service" && grep -qx 'ExecStart=/usr/lib/steamac/mem-trim' "$U/steamac-mem-trim.service" \
    || fail units "service does not run the script once"
[ -x "$S" ] && bash -n "$S" || fail units "script not executable or invalid"

A=$T/apps; mkdir -p "$A"
games() { # cmdline: one poll of "mem-trim games"
    printf '%s\n' "$1" > "$T/cmdline"; rm -f "$T/drop" "$T/compact"
    STEAMAC_CMDLINE=$T/cmdline STEAMAC_DROP_CACHES=$T/drop STEAMAC_COMPACT=$T/compact STEAMAC_MEMINFO=$T/meminfo \
        STEAMAC_APPS=$A STEAMAC_TRIM_STATE=$T/state STEAMAC_TRIM_POLLS=1 bash "$S" games > "$T/out" 2>&1
}
trimmed() { [ "$(cat "$T/drop" 2>/dev/null)" = 1 ] && [ "$(cat "$T/compact" 2>/dev/null)" = 1 ]; }
untouched() { [ ! -e "$T/drop" ] && [ ! -e "$T/compact" ]; }
rm -f "$T/state"
games "rootwait" || fail games "exit status $?"
untouched || fail games "trimmed although no game ever ran"
mkdir "$A/app-steam-app438100-2792.scope"
games "rootwait" && untouched || fail games "trimmed while a game runs"
games "rootwait" && untouched || fail games "trimmed while a game runs (second poll)"
rmdir "$A/app-steam-app438100-2792.scope"; mkdir "$A/app-steam-session.scope"
games "rootwait" && trimmed || fail games "no trim after the game ended (drop '$(cat "$T/drop" 2>/dev/null)' compact '$(cat "$T/compact" 2>/dev/null)')"
grep -q 'game ended' "$T/out" || fail games "no log line for the trim"
games "rootwait" && untouched || fail games "trimmed again with no game started since"
mkdir "$A/app-steam-app438100-3001.scope"; games "rootwait"; rmdir "$A/app-steam-app438100-3001.scope"
games "console=hvc0 steamac.mem_trim=0" && untouched || fail games "trimmed although steamac.mem_trim=0"
[ "$(readlink "$U/multi-user.target.wants/steamac-mem-trim-games.service")" = ../steamac-mem-trim-games.service ] \
    || fail games "games service not enabled"
grep -qx 'ExecStart=/usr/lib/steamac/mem-trim games' "$U/steamac-mem-trim-games.service" || fail games "service does not run games"

[ $fails -eq 0 ] && echo "mem-trim: all PASS" || { echo "mem-trim: $fails FAIL"; exit 1; }
