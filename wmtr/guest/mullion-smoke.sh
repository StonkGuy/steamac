#!/bin/bash
# mullion-smoke.sh [--control] [EXE [ARGS...]]: the guest smoke of the Mullion path. Runs a Windows test program
# (default: d3d12_memory.exe next to this script, wmtr/tests/d3d12_memory.cpp) the way Steam runs a game on that path:
# the FEX compatibility tool -> Steam Linux Runtime 3.0 (sniper) -> Proton 10.0 (x86-64, set up by steam-gfx-path with
# Mullion's dlls and its user_settings.py) -> waitforexitandrun, in a scratch compatdata of its own
# ($MULLION_SMOKE_DIR/cd). Never touches Steam's compatdata, settings or a game's prefix.
#   --control  MULLION_CONFIG empty for this run (an environment value wins over Proton's user_settings.py): Mullion's
#              default, MTLHeap-backed upload/readback heaps. Expected: the placed* and compute cases fail (or the
#              program faults) where the normal run passes them.
# Checks first that this boot is the Mullion path (kernel command line steamac.gfx=mullion) and that the Proton carries
# this layer's Mullion files. Prints the program's report (d3d12_memory: CASE/RESULT lines); exit status 0 only for
# "RESULT ... failed=0". Proton's log: $MULLION_SMOKE_DIR/steam-0.log (PROTON_LOG=1).
# Env: MULLION_PROTON (default: Proton 10.0), MULLION_SLR (SteamLinuxRuntime_sniper), MULLION_SMOKE_DIR
# (~/vrc/mullion), WINEDEBUG (-all), DISPLAY (:1, the gamescope session's Xwayland), STEAMAC_MULLION_DIR.
set -u
control=0
[[ ${1:-} == --control ]] && { control=1; shift; }
HERE=$(cd "$(dirname "$0")" && pwd)
EXE=${1:-$HERE/d3d12_memory.exe}
[[ $# -gt 0 ]] && shift
ARGS=("$@")
C=$HOME/.local/share/Steam/steamapps/common
PROTON=$C/${MULLION_PROTON:-Proton 10.0}
SLR=$C/${MULLION_SLR:-SteamLinuxRuntime_sniper}
FEXT=$C/FEX-Emu
PAYLOAD=${STEAMAC_MULLION_DIR:-/usr/lib/steamac/mullion}
DIR=${MULLION_SMOKE_DIR:-$HOME/vrc/mullion}
die() { echo "mullion-smoke: $*" >&2; exit 2; }

# the boot and the Proton
grep -qw 'steamac.gfx=mullion' /proc/cmdline || die "this boot is not the Mullion path (kernel command line: $(grep -o 'steamac.gfx=[a-z]*' /proc/cmdline || echo 'no steamac.gfx'))"
[[ -f $EXE ]] || die "no $EXE"
[[ -f $PAYLOAD/x86_64-unix/winemetal.so ]] || die "no Mullion payload in $PAYLOAD"
W=$PROTON/files/lib/wine
[[ -d $W/x86_64-windows ]] || die "no x86-64 Proton at $PROTON (MULLION_PROTON)"
[[ $(cat "$W/.steamac-dxmt" 2>/dev/null) == mullion ]] || die "$PROTON is not set up for Mullion (steam-gfx-path: journalctl --user -u steam -b | grep steam-gfx-path)"
cmp -s "$W/x86_64-unix/winemetal.so" "$PAYLOAD/x86_64-unix/winemetal.so" || die "$W/x86_64-unix/winemetal.so is not the payload's"
for f in dxvk/x86_64-windows/d3d11 dxvk/x86_64-windows/dxgi vkd3d-proton/x86_64-windows/d3d12core; do
    [[ ! -f $W/$f.dll ]] || grep -aqi mullion "$W/$f.dll" || die "$W/$f.dll is not Mullion's"
done
grep -q MULLION_CONFIG "$PROTON/user_settings.py" 2>/dev/null || echo "mullion-smoke: WARNING: $PROTON/user_settings.py has no MULLION_CONFIG" >&2
[[ -d $SLR && -d $FEXT ]] || die "no $SLR or $FEXT (Steam installs them with the first x86-64 game)"

# a tool's command line from its toolmanifest.vdf (Steam's composition: tool1 tool2 ... program), the verb filled in
tool() { # dir, fallback command line
    local line w first=1
    line=$(sed -n 's/^[[:space:]]*"commandline"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$1/toolmanifest.vdf" 2>/dev/null | head -n 1)
    [[ -n $line ]] || line=$2
    for w in $line; do
        w=${w//%verb%/waitforexitandrun}
        if (( first )); then printf '%s\n' "$1$w"; first=0; else printf '%s\n' "$w"; fi
    done
}
mapfile -t chain < <(tool "$FEXT" "/fex-compat-tool %verb% --"; tool "$SLR" "/_v2-entry-point --verb=%verb% --"; tool "$PROTON" "/proton %verb%")

mkdir -p "$DIR/cd"
cp -f "$EXE" "$DIR/"
OUT=$DIR/$(basename "$EXE" .exe).txt
rm -f "$OUT"
export STEAM_COMPAT_DATA_PATH=$DIR/cd
export STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.local/share/Steam
export STEAM_COMPAT_TOOL_PATHS="$PROTON:$SLR:$FEXT"
export SteamAppId=0 SteamGameId=0
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export DISPLAY=${DISPLAY:-:1} ENABLE_GAMESCOPE_WSI=1 GAMESCOPE_WAYLAND_DISPLAY=gamescope-0
export WINEDEBUG=${WINEDEBUG:--all} PROTON_USE_XALIA=0 PROTON_LOG=1 PROTON_LOG_DIR=$DIR
if (( control )); then export MULLION_CONFIG=; fi
echo "mullion-smoke: $(basename "$EXE") through $(basename "$PROTON") ($(cat "$PROTON/version" 2>/dev/null)), MULLION_CONFIG $( ((control)) && echo 'empty (control run)' || echo 'from user_settings.py')"
printf 'mullion-smoke: %q ' "${chain[@]}" "$DIR/$(basename "$EXE")" --out "Z:${OUT//\//\\}" "${ARGS[@]}"; echo
start=$(date +%s)
"${chain[@]}" "$DIR/$(basename "$EXE")" --out "Z:${OUT//\//\\}" "${ARGS[@]}"
status=$?
echo "mullion-smoke: exit status $status after $(( $(date +%s) - start )) s"
[[ -s $OUT ]] || die "no report in $OUT (the program did not start or faulted before its first line; see $DIR/steam-0.log)"
cat "$OUT"
grep -q '^RESULT passed=[0-9]* failed=0 skipped=' "$OUT" || {
    grep -q '^RESULT' "$OUT" || echo "mullion-smoke: no RESULT line: the program ended in the case after the last CASE line" >&2
    exit 1
}
exit 0
