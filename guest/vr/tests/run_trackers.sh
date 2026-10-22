#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# End-to-end test of the tracked-device table (include/steamac_vr_trackers.h) without a headset: vr-fakehost serves
# the headset, the game side is vr-trackerapp (loads the runtime like Proton). Sources:
#   1. svr-tracker-feed, 3 devices, stops cleanly (slots released -> Deactivated at once)
#   2. svr-tracker-feed, 8 devices, killed with SIGKILL (slots left behind -> Deactivated when stale, 2 s)
#   3. svr-driverhost hosting tests/sample_driver (an unmodified-style SteamVR tracker driver)
#   4. a background app without a host link reads the headset and the feed's trackers through the mirror
#   tests/run_trackers.sh BUILD_DIR
set -eu
out=${1:?build dir}
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
export STEAMAC_VR_TRACKERS=$tmp/trackers
pids=
trap 'for p in $pids; do kill $p 2>/dev/null || true; done; rm -rf "$tmp"' EXIT

start_host() { # $1 name
  sock=$tmp/$1.sock
  "$out/vr-fakehost" --socket "$sock" --log "$tmp/$1-host.log" --once 2>"$tmp/$1-host.err" &
  fh=$!
  pids="$pids $fh"
  i=0
  while [ ! -S "$sock" ] && [ $i -lt 100 ]; do sleep 0.05; i=$((i + 1)); done
}
fail() { echo "FAIL trackers: $*"; for f in "$tmp"/*.err; do echo "== $f"; tail -20 "$f"; done; exit 1; }

# 1. clean stop
start_host feed
"$out/svr-tracker-feed" --count 3 --seconds 3 2>"$tmp/feed1.err" &
pids="$pids $!"
STEAMAC_VR_SOCKET=$sock STEAMAC_VR_LOG=1 "$out/vr-trackerapp" "$out/runtime" feed 3 3 2>"$tmp/app1.err" || fail "feed (clean stop)"
grep -q 'tracker 3 connected: serial steamac-feed-waist class 3 role TrackerRole_Waist' "$tmp/app1.err" || fail "no tracker log line"
wait $fh

# 2. crashed source: 8 devices, SIGKILL after 2.5 s
start_host kill
"$out/svr-tracker-feed" --count 8 --prefix crash 2>"$tmp/feed2.err" &
feed=$!
pids="$pids $feed"
( sleep 2.5; kill -9 $feed ) &
STEAMAC_VR_SOCKET=$sock "$out/vr-trackerapp" "$out/runtime" feed 8 3 2>"$tmp/app2.err" || fail "feed (killed source)"
wait $fh
"$out/svr-tracker-feed" --clear

# 3. a SteamVR driver through svr-driverhost (needs the scene app's mirror for the head: start the app first)
start_host driver
STEAMAC_VR_SOCKET=$sock "$out/vr-trackerapp" "$out/runtime" driver 3 2>"$tmp/app3.err" &
app=$!
sleep 0.5
"$out/svr-driverhost" --seconds 3 "$out/svrtest" 2>"$tmp/driverhost.err" || fail "driverhost exit"
wait $app || fail "driver"
grep -q '\[svrtest\] resources ok' "$tmp/driverhost.err" || fail "driver resources"
grep -q 'svrtest-hmd (class 1) refused' "$tmp/driverhost.err" || fail "HMD not refused"
grep -q 'svrtest-unlabelled published: index 4 type (default) model svrtest tracker role TrackerRole_Chest' \
  "$tmp/driverhost.err" || fail "role from the trackers setting"
wait $fh

# 4. companion app without a host link
start_host secondary
STEAMAC_VR_SOCKET=$sock "$out/vr-trackerapp" "$out/runtime" feed 3 4 2>"$tmp/app4s.err" &
app=$!
"$out/svr-tracker-feed" --count 3 --seconds 3.5 2>"$tmp/feed4.err" &
pids="$pids $!"
sleep 1
env -u STEAMAC_VR_SOCKET STEAMAC_VR_LOG=1 "$out/vr-trackerapp" "$out/runtime" secondary 2>"$tmp/app4.err" || fail "secondary"
grep -q 'init without a host link' "$tmp/app4.err" || fail "secondary log"
wait $app || fail "secondary's scene app"
wait $fh
echo "PASS trackers"
