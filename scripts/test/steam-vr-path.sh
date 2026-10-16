#!/bin/bash
# SPDX-License-Identifier: MIT
# guest/layer/usr/lib/steamac/steam-vr-path against a fake Steam tree (Proton Experimental) and VR payload:
#   on        Proton Experimental's vrclient_x64.dll/.so are the payload's, openvrpaths.vrpath names our runtime first
#             (Proton turns runtime[0] into PROTON_VR_RUNTIME), the user's previous vrpath is kept
#   again     a second run changes nothing (idempotent)
#   update    a Proton update (new stock files + version): the next run keeps the NEW stock files and re-applies
#   off       Proton's files (the updated ones) and the user's vrpath are back byte for byte, the state dir is gone
#   novrpath  with no vrpath before, "off" removes ours instead of leaving it
#   missing   steamac.vr=1 without the payload leaves everything stock
#   oldgfx    steamac.vr=1 with a DXMT payload that has no VR link (winemetal.so without wmtr_vr_call, or no DXMT
#             payload) leaves everything stock and says why
#   other     steamac.vr=0 / no key / another Proton: untouched
#   foreign   payload A, a Proton update that leaves vrclient_x64 alone (new version file only), payload B (another
#             bundle's layer), off: Proton's files are back (before: A's patched files were kept as "originals")
#   noorig    our file with no kept original: it stays, a WARNING names Verify integrity, no "back" claim
# usage: scripts/test/steam-vr-path.sh
set -u
S=$(cd "$(dirname "$0")/../.." && pwd)/guest/layer/usr/lib/steamac/steam-vr-path
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fails=0; fail() { fails=$((fails + 1)); echo "FAIL [$1]: $2"; }
blob() { mkdir -p "$(dirname "$1")"; head -c 300 /dev/zero | tr '\0' "$2" > "$1"; }
ST=$T/steam; P="$ST/steamapps/common/Proton - Experimental"; P2=$ST/compatibilitytools.d/Proton-custom
for p in "$P" "$P2"; do
  blob "$p/files/lib/wine/x86_64-windows/vrclient_x64.dll" v; blob "$p/files/lib/wine/x86_64-unix/vrclient_x64.so" u
  blob "$p/files/lib/wine/i386-windows/vrclient.dll" i; echo 1 > "$p/version"
done
X=$T/vr; blob "$X/x86_64-windows/vrclient_x64.dll" m; blob "$X/x86_64-unix/vrclient_x64.so" n
blob "$X/runtime/bin/linux64/vrclient.so" r
# the DXMT payload: winemetal.so with the VR link, and one from before it
G=$T/dxmt; blob "$G/x86_64-unix/winemetal.so" g; printf 'wmtr_vr_call\0' >> "$G/x86_64-unix/winemetal.so"
GOLD=$T/dxmt-old; blob "$GOLD/x86_64-unix/winemetal.so" g
CFG=$T/cfg; mkdir -p "$CFG/openvr"; echo '{"runtime":["/home/u/SteamVR"],"version":1}' > "$CFG/openvr/openvrpaths.vrpath"
snap() { (cd "$T" && find steam cfg -type f | LC_ALL=C sort | while read -r f; do echo "$f $(cksum < "$f")"; done); }
run() { echo "$1" > "$T/cmdline"
  STEAMAC_CMDLINE=$T/cmdline STEAMAC_STEAM_ROOT=$ST STEAMAC_VR_DIR=${2:-$X} STEAMAC_DXMT_DIR=${3:-$G} STEAMAC_VR_STATE=$T/state \
    XDG_CONFIG_HOME=$CFG HOME=$T bash "$S" 2>>"$T/log"; }
W=$P/files/lib/wine
# what Proton's setup_openvr_paths() makes of the vrpath: runtime[0] -> PROTON_VR_RUNTIME
proton_vr_runtime() { python3 -c 'import json,sys; c=json.load(open(sys.argv[1])); print(c["runtime"][0] if c.get("runtime") else "")' "$CFG/openvr/openvrpaths.vrpath" 2>/dev/null; }

stock=$(snap)
run "quiet steamac.vr=0"; [ "$(snap)" = "$stock" ] || fail other "steamac.vr=0 changed files"
run "quiet"; [ "$(snap)" = "$stock" ] || fail other "no key changed files"
[ -e "$T/state" ] && fail other "state dir created while off"
run "steamac.vr=1" "$T/none"; [ "$(snap)" = "$stock" ] || fail missing "missing payload changed files"
run "steamac.vr=1" "$X" "$GOLD"; [ "$(snap)" = "$stock" ] || fail oldgfx "DXMT payload without the VR link changed files"
grep -q "no VR link" "$T/log" || fail oldgfx "no reason logged"
run "steamac.vr=1" "$X" "$T/none"; [ "$(snap)" = "$stock" ] || fail oldgfx "missing DXMT payload changed files"
[ -e "$T/state" ] && fail oldgfx "state dir created without a VR-capable DXMT"

run "quiet steamac.vr=1 splash"
cmp -s "$W/x86_64-windows/vrclient_x64.dll" "$X/x86_64-windows/vrclient_x64.dll" || fail on "vrclient_x64.dll not installed"
cmp -s "$W/x86_64-unix/vrclient_x64.so" "$X/x86_64-unix/vrclient_x64.so" || fail on "vrclient_x64.so not installed"
# the runtime must be under $HOME (the Steam Linux Runtime container does not see the guest's /usr)
[ "$(proton_vr_runtime)" = "$T/state/runtime" ] || fail on "PROTON_VR_RUNTIME would be '$(proton_vr_runtime)'"
cmp -s "$T/state/runtime/bin/linux64/vrclient.so" "$X/runtime/bin/linux64/vrclient.so" || fail on "runtime not copied"
s1=$(snap); echo "$s1" | grep -q "Proton-custom" && [ "$(echo "$s1" | grep Proton-custom)" = "$(echo "$stock" | grep Proton-custom)" ] \
  || fail other "another Proton was touched"
grep -q "i386-windows/vrclient.dll $(cd "$T" && cksum < "$P/files/lib/wine/i386-windows/vrclient.dll")" <<<"$s1" || fail on "i386 touched"

run "steamac.vr=1"; [ "$(snap)" = "$s1" ] || fail again "second run changed files"

# Proton update: Steam writes new stock files and a new version
blob "$W/x86_64-windows/vrclient_x64.dll" w; blob "$W/x86_64-unix/vrclient_x64.so" x; echo 2 > "$P/version"
upd=$(snap)
run "steamac.vr=1"
cmp -s "$W/x86_64-windows/vrclient_x64.dll" "$X/x86_64-windows/vrclient_x64.dll" || fail update "not re-applied after the update"

# layer update: a new payload replaces the old one; Proton's (updated) originals stay kept
X2=$T/vr2; cp -a "$X" "$X2"; blob "$X2/x86_64-windows/vrclient_x64.dll" q
run "steamac.vr=1" "$X2"
cmp -s "$W/x86_64-windows/vrclient_x64.dll" "$X2/x86_64-windows/vrclient_x64.dll" || fail payload "new payload not installed"
blob "$X2/runtime/bin/linux64/vrclient.so" z; run "steamac.vr=1" "$X2"
cmp -s "$T/state/runtime/bin/linux64/vrclient.so" "$X2/runtime/bin/linux64/vrclient.so" || fail payload "runtime not refreshed"

run "steamac.vr=0" "$X2"
expect=$( (echo "$upd" | grep -v openvrpaths; echo "$stock" | grep openvrpaths) | LC_ALL=C sort)
[ "$(snap | LC_ALL=C sort)" = "$expect" ] || fail off "tree differs from stock+update: $(diff <(echo "$expect") <(snap | LC_ALL=C sort) | head -4 | tr '\n' '|')"
[ -e "$T/state" ] && fail off "state dir left"

rm -f "$CFG/openvr/openvrpaths.vrpath"; base=$(snap)
run "steamac.vr=1"; [ "$(proton_vr_runtime)" = "$T/state/runtime" ] || fail novrpath "vrpath not written"
run "steamac.vr=0"; [ "$(snap)" = "$base" ] || fail novrpath "our vrpath left behind"

# foreign: fresh stock, then A -> version-only Proton update -> B -> off
blob "$W/x86_64-windows/vrclient_x64.dll" v; blob "$W/x86_64-unix/vrclient_x64.so" u; echo 1 > "$P/version"
fs=$(snap | grep "Proton - Experimental/files"); : > "$T/log"
XB=$T/vrb; cp -a "$X" "$XB"; blob "$XB/x86_64-windows/vrclient_x64.dll" c; blob "$XB/x86_64-unix/vrclient_x64.so" d
run "steamac.vr=1" "$X"; echo 3 > "$P/version"; run "steamac.vr=1" "$XB"; run "steamac.vr=0" "$XB"
[ "$(snap | grep "Proton - Experimental/files")" = "$fs" ] || fail foreign "Proton's vrclient_x64 not back after A -> update -> B -> off"
[ -e "$T/state" ] && fail foreign "state dir left"
# noorig
run "steamac.vr=1" "$XB"; rm -f "$T/state/orig/x86_64-windows/vrclient_x64.dll"; : > "$T/log"; run "steamac.vr=0" "$XB"
cmp -s "$W/x86_64-windows/vrclient_x64.dll" "$XB/x86_64-windows/vrclient_x64.dll" || fail noorig "patched dll changed without a kept original"
[ "$(tr -d u < "$W/x86_64-unix/vrclient_x64.so")" = "" ] || fail noorig "kept .so not restored"
grep -q "WARNING: no original of vrclient_x64.dll kept.*Verify integrity" "$T/log" || fail noorig "no warning: $(cat "$T/log")"
grep -q "vrclient_x64 and openvrpaths.vrpath back" "$T/log" && fail noorig "claims Proton's files back"

if [ $fails -eq 0 ]; then echo "PASS steam-vr-path"; else cat "$T/log"; exit 1; fi
