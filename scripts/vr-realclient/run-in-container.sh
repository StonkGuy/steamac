#!/usr/bin/env bash
# Runs inside the vr-realclient container (host networking): Xvfb + Monado (simulated HMD) + WiVRn v26.9 client.
# Usage: run-in-container.sh <host> <port> <pin> [seconds]   (requires the WIVRN_TEST_URI patch, see wivrn-test-uri.patch)
set -u
HOST=$1; PORT=$2; PIN=$3; SECS=${4:-60}
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp/xdg}; mkdir -p $XDG_RUNTIME_DIR; chmod 700 $XDG_RUNTIME_DIR
export HOME=/tmp/home; mkdir -p $HOME
if [[ -n ${LOSS:-} ]]; then   # ingress UDP loss in percent; needs its own netns (podman --network pasta --cap-add NET_ADMIN)
  nft add table inet t && nft add chain inet t i '{ type filter hook input priority 0; }' &&
  nft add rule inet t i udp sport "$PORT" numgen random mod 100 '<' "$LOSS" drop && echo "ingress UDP loss $LOSS%"
fi
Xvfb :99 -screen 0 1280x720x24 & export DISPLAY=:99; sleep 1
export XRT_COMPOSITOR_FORCE_${COMPOSITOR:-XCB}=1 SIMULATED_ENABLE=true XRT_DEBUG_GUI=0
export P_OVERRIDE_ACTIVE_CONFIG=simulated VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.x86_64.json}
(sleep 100000 | monado-service > /tmp/monado.log 2>&1) &
sleep 4
export XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json
if [[ $HOST == *:* ]]; then H="[$HOST]"; else H=$HOST; fi
WIVRN_LOGLEVEL=${WIVRN_LOGLEVEL:-debug} WIVRN_TEST_URI="wivrn://:$PIN@$H:$PORT" timeout "$SECS" ${WRAP:-} /work/build-client/bin/wivrn
echo "client exit $?"; tail -30 /tmp/monado.log
