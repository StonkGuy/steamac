#!/bin/bash
# d3dr end-to-end test on the Mac (no VM): the test programs run in a macOS Wine twice, once directly on the
# user's D3DMetal (the oracle) and once through d3dr (client d3d11.dll -> TCP -> d3dr-server.exe -> D3DMetal).
#   d3dr_triangle  must PASS both ways (same pixels)
#   d3d11bench     (optional, BENCH=1) prints fps both ways
# Needs the user's own CrossOver-derived Wine with D3DMetal, nothing of Apple's is part of steamac:
#   D3DR_WINE      Wine engine dir with bin/wine (e.g. a Sikarugir engine, wswine.bundle)
#   D3DR_D3DMETAL  D3DMetal renderer dir with wine/ and external/ (e.g. Sikarugir's Template .../renderer/d3dmetal),
#                  used in place (never copy D3DMetal's files: its layout relies on the symlinks)
#   WINEPREFIX     a scratch prefix (created if missing)
# Usage: tests/loopback.sh [build dir]   (exit 0 = pass, 77 = skipped: no D3DMetal)
set -u
B=$(cd "${1:-$(dirname "$0")/../build}" && pwd)
: "${D3DR_WINE:?set D3DR_WINE}" "${WINEPREFIX:?set WINEPREFIX}"
if [[ ! -e ${D3DR_D3DMETAL:-}/external/libd3dshared.dylib ]]; then echo "loopback: SKIP (no D3DMetal at D3DR_D3DMETAL)"; exit 77; fi
WINE=$D3DR_WINE/bin/wine
export WINEDLLPATH_PREPEND=$D3DR_D3DMETAL/wine CX_APPLEGPTK_LIBD3DSHARED_PATH=$D3DR_D3DMETAL/external/libd3dshared.dylib
export ROSETTA_ADVERTISE_AVX=1 WINEDEBUG=${WINEDEBUG:--all}
PORT=${D3DR_PORT:-47811}
W=$(mktemp -d "${TMPDIR:-/tmp}/d3dr-loop.XXXXXX")
trap '"$D3DR_WINE/bin/wineserver" -k 2>/dev/null; rm -rf "$W"' EXIT
mkdir -p "$W/direct" "$W/srv" "$W/cli"
cp "$B/d3dr_triangle.exe" "$B/d3d11bench.exe" "$W/direct/"
cp "$B/d3dr-server.exe" "$W/srv/"
cp "$B/d3d11.dll" "$B/d3dr_triangle.exe" "$B/d3d11bench.exe" "$W/cli/"
[[ -d $WINEPREFIX/drive_c ]] || WINEDLLOVERRIDES="winemenubuilder.exe=d" "$WINE" wineboot -i >/dev/null 2>&1
fail=0

direct() { WINEDLLOVERRIDES="d3d11,dxgi,d3d12=b;winemenubuilder.exe=d" "$WINE" "$W/direct/$1" "${@:2}" </dev/null 2>"$W/direct.err"; }
remote() {
    WINEDLLOVERRIDES="d3d11,dxgi,d3d12=b;winemenubuilder.exe=d" "$WINE" "$W/srv/d3dr-server.exe" --once --port "$PORT" \
        </dev/null >"$W/srv.out" 2>&1 &
    local srv=$! i
    for i in $(seq 50); do grep -q listening "$W/srv.out" 2>/dev/null && break; sleep 0.2; done
    WINEDLLOVERRIDES="d3d11=n;winemenubuilder.exe=d" D3DR_SERVER=127.0.0.1:$PORT "$WINE" "$W/cli/$1" "${@:2}" </dev/null 2>"$W/remote.err"
    local rc=$?
    wait $srv
    grep 'd3dr-server:' "$W/srv.out" | grep -v -e listening >&2
    return $rc
}

echo "== d3dr_triangle direct (oracle)"
direct d3dr_triangle.exe || { echo "loopback: oracle failed: D3DMetal setup broken"; tail -5 "$W/direct.err"; exit 1; }
echo "== d3dr_triangle through d3dr"
remote d3dr_triangle.exe || { fail=1; grep d3dr "$W/remote.err" | tail -5; }

if [[ ${BENCH:-0} == 1 ]]; then
    for cfg in "--draws 200 --overdraw 0" "--draws 2000 --overdraw 4"; do
        echo "== d3d11bench $cfg direct";  direct d3d11bench.exe --seconds "${SECS:-10}" $cfg --csv nul --label direct | grep RESULT
        echo "== d3d11bench $cfg d3dr";    remote d3d11bench.exe --seconds "${SECS:-10}" $cfg --csv nul --label d3dr | grep RESULT
    done
fi
(( fail )) && { echo "loopback: FAIL"; exit 1; }
echo "loopback: PASS"
