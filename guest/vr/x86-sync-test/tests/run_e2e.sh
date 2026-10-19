#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# End-to-end test without a headset: vr-fakehost (synthetic motion + tests/data/script.txt) serves the guest
# link on a Unix socket, vr-testapp loads the runtime like Proton does and checks poses, pacing, submits,
# actions and haptics; then the fake host's log is checked (every submit consistent with the frame it was
# rendered for, the haptic event arrived). Runs natively on x86-64 Linux and in the VM guest under FEX.
#   tests/run_e2e.sh BUILD_DIR [touch|index]
set -eu
out=${1:?build dir}
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'kill $fh 2>/dev/null || true; rm -rf "$tmp"' EXIT

for profile in ${2:-touch index}; do
  sock=$tmp/vr.sock log=$tmp/host-$profile.log
  "$out/vr-fakehost" --socket "$sock" --script "$here/data/script.txt" --log "$log" --profile "$profile" --once &
  fh=$!
  i=0
  while [ ! -S "$sock" ] && [ $i -lt 100 ]; do sleep 0.05; i=$((i + 1)); done
  STEAMAC_VR_SOCKET=$sock STEAMAC_VR_LOG=${STEAMAC_VR_LOG:-1} \
    "$out/vr-testapp" "$out/runtime" "$here/data" --profile "$profile" 2>"$tmp/app-$profile.err" || {
    cat "$tmp/app-$profile.err"
    echo "FAIL e2e ($profile): vr-testapp"
    exit 1
  }
  wait $fh
  # host-side checks
  grep -q '"ev":"hello"' "$log" || { echo "FAIL e2e ($profile): no hello"; exit 1; }
  grep -q '"ok":false' "$log" && { grep '"ok":false' "$log" | head -3; echo "FAIL e2e ($profile): bad submit"; exit 1; }
  grep -q '"ev":"haptic","hand":1,"duration":0.1000,"frequency":160.0,"amplitude":0.500' "$log" ||
    { echo "FAIL e2e ($profile): haptic not received"; exit 1; }
  grep -q '"ev":"haptic","hand":0' "$log" && { echo "FAIL e2e ($profile): haptic went to the left hand"; exit 1; }
  summary=$(grep '"ev":"summary"' "$log")
  echo "e2e ($profile): $summary"
  case $summary in *'"submit_errors":0'*) ;; *) echo "FAIL e2e ($profile)"; exit 1 ;; esac
  # the runtime logs every interface the app asked for (the record M2 needs from VRChat)
  grep -q 'requested interface IVRInput_' "$tmp/app-$profile.err" || { echo "FAIL e2e: interface log"; exit 1; }
done
echo "PASS e2e"
