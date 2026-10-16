#!/bin/bash
# d3dr-host.sh [start|stop|status]: run d3dr-server.exe on the Mac with the user's own D3DMetal, for the guest's
# d3dr d3d11.dll (guest/d3dr-run.sh). Nothing of Apple's ships with steamac: the server runs in the user's
# CrossOver-derived Wine with the user's D3DMetal, both used in place.
# Env (defaults = a Sikarugir install):
#   D3DR_WINE      Wine engine dir with bin/wine        (Sikarugir: an extracted engine, e.g. WS12WineSikarugir10.0_6)
#   D3DR_D3DMETAL  renderer dir with wine/ + external/  (Sikarugir: <Template>.app/Contents/Frameworks/renderer/d3dmetal)
#   D3DR_SERVER_EXE  d3dr-server.exe (default: next to this script's ../build)
#   D3DR_PREFIX    Wine prefix (default ~/Library/Application Support/steamac/d3dr/pfx)
#   D3DR_PORT      TCP port on 127.0.0.1 (default 47811; the guest reaches it as 192.168.127.254 through gvproxy)
set -u
S=$(cd "$(dirname "$0")" && pwd)
STATE="$HOME/Library/Application Support/steamac/d3dr"
PIDF=$STATE/server.pid LOGF=$STATE/server.log
tpl() { ls -d "$HOME/Library/Application Support/Sikarugir/Template/"*.app/Contents/Frameworks/renderer/d3dmetal 2>/dev/null | tail -1; }
WINE_DIR=${D3DR_WINE:-}
D3DM=${D3DR_D3DMETAL:-$(tpl)}
EXE=${D3DR_SERVER_EXE:-$S/../build/d3dr-server.exe}
export WINEPREFIX=${D3DR_PREFIX:-$STATE/pfx}
PORT=${D3DR_PORT:-47811}

running() { [[ -f $PIDF ]] && kill -0 "$(cat "$PIDF")" 2>/dev/null; }
case ${1:-start} in
status) running && echo "d3dr-host: running (pid $(cat "$PIDF"), log $LOGF)" || echo "d3dr-host: not running"; exit 0 ;;
stop)
    running && kill "$(cat "$PIDF")"
    [[ -n $WINE_DIR ]] && "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    rm -f "$PIDF"; echo "d3dr-host: stopped"; exit 0 ;;
start) ;;
*) echo "usage: $0 [start|stop|status]"; exit 2 ;;
esac
running && { echo "d3dr-host: already running (pid $(cat "$PIDF"))"; exit 0; }
[[ -x $WINE_DIR/bin/wine ]] || { echo "d3dr-host: set D3DR_WINE to a CrossOver-derived Wine engine (bin/wine)"; exit 1; }
[[ -e $D3DM/external/libd3dshared.dylib && -d $D3DM/wine/x86_64-windows ]] \
    || { echo "d3dr-host: no D3DMetal found; set D3DR_D3DMETAL (GPTK/Sikarugir renderer dir with wine/ and external/)"; exit 1; }
[[ -f $EXE ]] || { echo "d3dr-host: no $EXE (make -C d3dr)"; exit 1; }
mkdir -p "$STATE"
export WINEDLLPATH_PREPEND=$D3DM/wine CX_APPLEGPTK_LIBD3DSHARED_PATH=$D3DM/external/libd3dshared.dylib
export ROSETTA_ADVERTISE_AVX=1 WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES="d3d11,dxgi,d3d12=b;winemenubuilder.exe=d"
[[ -d $WINEPREFIX/drive_c ]] || "$WINE_DIR/bin/wine" wineboot -i </dev/null >"$STATE/wineboot.log" 2>&1
nohup "$WINE_DIR/bin/wine" "$EXE" --port "$PORT" --log </dev/null >"$LOGF" 2>&1 &
echo $! > "$PIDF"
for _ in $(seq 50); do grep -q listening "$LOGF" 2>/dev/null && break; sleep 0.2; done
grep -q listening "$LOGF" && echo "d3dr-host: listening on 127.0.0.1:$PORT (pid $(cat "$PIDF"), log $LOGF)" \
    || { echo "d3dr-host: server did not start, see $LOGF"; exit 1; }
