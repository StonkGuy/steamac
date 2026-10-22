#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# The Windows driver host under Wine, the way a body estimator runs (make check-wine; needs wine64 and the
# driverhost-win build): svr-driverhost.exe --then companion.exe loads the svrtest driver (it serves a named pipe),
# starts the companion beside it in the same wineserver, the companion sends moving waist positions over the pipe,
# and the waist's pose in the tracked-device table follows them. The host exits with the companion.
#   tests/run_trackers_wine.sh BUILD_DIR
set -eu
out=$(cd "${1:?build dir}" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export STEAMAC_VR_TRACKERS=$tmp/trackers WINEDEBUG=-all WINEPREFIX=${WINEPREFIX:-$tmp/prefix}
fail() { echo "FAIL trackers-wine: $*"; cat "$tmp/host.log"; exit 1; }
wine64 wineboot -i >/dev/null 2>&1 || true
wine64 "$out/win/svr-driverhost.exe" --table "$tmp/trackers" --then "$out/win/svrtest/companion.exe" >"$tmp/host.log" 2>&1 &
host=$!
xs=""
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
  sleep 0.5
  x=$("$out/svr-tracker-feed" --table "$tmp/trackers" --list 2>/dev/null | sed -n 's/.*svrtest-waist .* pos \([-0-9.]*\) .*/\1/p')
  xs="$xs ${x:-none}"
done
wait $host || fail "host exit"
grep -q "pipe: companion connected" "$tmp/host.log" || fail "the companion never reached the driver's pipe"
grep -q "companion: sent 30 positions" "$tmp/host.log" || fail "companion did not finish"
n=$(echo "$xs" | tr ' ' '\n' | grep -v '^$' | grep -v none | sort -u | wc -l)
[ "$n" -ge 4 ] || fail "waist did not move with the companion's positions:$xs"
echo "trackers-wine: waist x over time:$xs"
echo "PASS trackers-wine"
