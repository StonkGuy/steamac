#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# End-to-end test without a headset: vr-fakehost (synthetic motion + tests/data/script.txt) serves the guest
# link on a Unix socket, vr-testapp loads the runtime like Proton does and checks poses, pacing, submits,
# actions and haptics; then the fake host's log is checked (every submit consistent with the frame it was
# rendered for, the haptic event arrived). Runs natively on x86-64 Linux and in the VM guest under FEX.
# Each profile runs twice: the test app's own sequence, and --title (a Unity SteamVR plugin title's: manifest in
# the startup info, IVRInput_010, every action read for every input source each frame).
#   tests/run_e2e.sh BUILD_DIR [touch|index]
set -eu
out=${1:?build dir}
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'kill $fh 2>/dev/null || true; rm -rf "$tmp"' EXIT

for mode in "" --title; do
for profile in ${2:-touch index}; do
  sock=$tmp/vr.sock log=$tmp/host-$profile.log
  "$out/vr-fakehost" --socket "$sock" --script "$here/data/script.txt" --log "$log" --profile "$profile" --once &
  fh=$!
  i=0
  while [ ! -S "$sock" ] && [ $i -lt 100 ]; do sleep 0.05; i=$((i + 1)); done
  STEAMAC_VR_SOCKET=$sock STEAMAC_VR_LOG=${STEAMAC_VR_LOG:-1} \
    "$out/vr-testapp" "$out/runtime" "$here/data" --profile "$profile" $mode 2>"$tmp/app-$profile.err" || {
    cat "$tmp/app-$profile.err"
    echo "FAIL e2e ($profile $mode): vr-testapp"
    exit 1
  }
  wait $fh
  # host-side checks
  grep -q '"ev":"hello"' "$log" || { echo "FAIL e2e ($profile): no hello"; exit 1; }
  grep -q '"ok":false' "$log" && { grep '"ok":false' "$log" | head -3; echo "FAIL e2e ($profile): bad submit"; exit 1; }
  grep -q '"ev":"haptic","hand":1,"duration":0.1000,"frequency":160.0,"amplitude":0.500' "$log" ||
    { echo "FAIL e2e ($profile): haptic not received"; exit 1; }
  grep -q '"ev":"haptic","hand":0' "$log" && { echo "FAIL e2e ($profile): haptic went to the left hand"; exit 1; }
  # the submit carries the WMTVRSubmitInfo fields: left whole texture (w = 0) gamma flag, right flipped
  # half-width rect, wait value of the later eye
  grep -q '"rect0":\[0,0,0,0\],"flags":\[2,3\],"slice":\[0,0\],"wait":\[8192,2\]' "$log" ||
    { grep '"ev":"submit"' "$log" | head -2; echo "FAIL e2e ($profile): submit fields"; exit 1; }
  summary=$(grep '"ev":"summary"' "$log")
  echo "e2e ($profile): $summary"
  case $summary in *'"submit_errors":0'*) ;; *) echo "FAIL e2e ($profile)"; exit 1 ;; esac
  # the runtime logs every interface the app asked for (the record M2 needs from VRChat)
  grep -q 'requested interface IVRInput_' "$tmp/app-$profile.err" || { echo "FAIL e2e: interface log"; exit 1; }
  # --title: the manifest came from the startup info, with the binding file of the headset's controllers
  if [ -n "$mode" ]; then
    case $profile in index) ct=knuckles ;; *) ct=oculus_touch ;; esac
    grep -q "action manifest .*/vrchat/actions.json: 20 actions, 25 bindings for $ct" "$tmp/app-$profile.err" ||
      { grep -i manifest "$tmp/app-$profile.err"; echo "FAIL e2e ($profile $mode): startup manifest"; exit 1; }
  fi
done
done
# the headset leaves mid-game and comes back: the HMD stays (standby), the game keeps its frames
sock=$tmp/vr-standby.sock log=$tmp/host-standby.log
"$out/vr-fakehost" --socket "$sock" --script "$here/data/script-standby.txt" --log "$log" --once &
fh=$!
i=0
while [ ! -S "$sock" ] && [ $i -lt 100 ]; do sleep 0.05; i=$((i + 1)); done
STEAMAC_VR_SOCKET=$sock "$out/vr-testapp" "$out/runtime" "$here/data" --standby 2>"$tmp/app-standby.err" || {
  cat "$tmp/app-standby.err"; echo "FAIL e2e (standby)"; exit 1; }
wait $fh
# the frames carry another view than the hello (game started on the standby view, then the headset connected):
# every submit must carry the projection and IPD the game was given, or the headset reprojects it distorted
sock=$tmp/vr-live.sock log=$tmp/host-live.log
"$out/vr-fakehost" --socket "$sock" --script "$here/data/script.txt" --log "$log" --once --live-view &
fh=$!
i=0
while [ ! -S "$sock" ] && [ $i -lt 100 ]; do sleep 0.05; i=$((i + 1)); done
STEAMAC_VR_SOCKET=$sock "$out/vr-testapp" "$out/runtime" "$here/data" 2>"$tmp/app-live.err" || {
  cat "$tmp/app-live.err"; echo "FAIL e2e (live view)"; exit 1; }
wait $fh
grep -q '"ok":false' "$log" && { grep '"ok":false' "$log" | head -3; echo "FAIL e2e (live view): bad submit"; exit 1; }
echo "e2e (live view): $(grep '"ev":"summary"' "$log")"
echo "PASS e2e"
