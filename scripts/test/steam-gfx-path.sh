#!/bin/bash
# SPDX-License-Identifier: MIT
# guest/layer/usr/lib/steamac/steam-gfx-path against a fake Steam tree (two Protons, two prefixes) and DXMT set:
#   dxmt      each Proton's DXVK set is DXMT's (builtin marker cleared), winemetal added, prefixes get the placeholder
#   again     a second run changes nothing (idempotent)
#   update    a Proton update rewrites stock files (new version): the next run keeps the NEW stock files and re-applies
#   vulkan    Proton's files are back byte for byte (the updated ones), every added file and marker is gone,
#             prefixes hold Proton's DXVK again and nothing else changed
#   d3d12     a payload with d3d12.dll replaces vkd3d-proton's d3d12.dll where a Proton has one (P1, not P2); a
#             payload without it puts vkd3d-proton's back
#   missing   steamac.gfx=dxmt without the DXMT files leaves everything stock
#   poisoned  a kept original that is a DXMT file (kept by an older script from a payload it no longer knows) is never
#             kept and never restored: the file stays, a log line points at Steam's Verify integrity, the backup goes
#   foreign   switching between app bundles (DXMT payload A -> B -> Vulkan with A's layer) where neither payload's
#             checksum is kept: the DXMT file is still recognised, Proton's kept originals go back and are only then
#             dropped (before: no restore, originals deleted, "DXVK back" logged with DXMT's d3d11/dxgi in place)
#   noorig    a DXMT file with no kept original and no stock copy: it stays, a WARNING names Steam's Verify integrity,
#             no "DXVK back"
#   oldlayer  an older layer (no stock copies) dropped $STATE/orig on a Vulkan boot and left DXMT files behind: the
#             next Vulkan boot puts Proton's files back from $STATE/stock, the next DXMT boot keeps them as originals;
#             a Proton update makes the stock copy stale (never restored)
#   mullion   steamac.gfx=mullion (fresh tree): DXVK's set is Mullion's, so are vkd3d-proton's d3d12 and d3d12core
#             (where a Proton has them), winemetal is Mullion's, each x86-64 Proton gets our user_settings.py with
#             MULLION_CONFIG=d3d12.cpuVisibleHeapMemory=True and its mark names the path; a second run changes nothing
#   switch    mullion -> dxmt: DXMT's files replace Mullion's, vkd3d-proton's own d3d12core is back, user_settings.py
#             is gone; dxmt -> mullion -> vulkan: every Proton tree is stock again byte for byte
#   settings  a user_settings.py that is not ours is never written or removed (a WARNING says MULLION_CONFIG is missing)
#   mlost     Mullion's d3d12.dll (no dxmt/mullion string in it) is recognised without the kept checksums: a Vulkan
#             boot after the kept originals were lost restores vkd3d-proton's from the stock copy
#   mnomark   Mullion's d3d12.dll alone in a Proton with no mark and no state (no other file names Mullion or DXMT):
#             a Vulkan boot still recognises it as this layer's, never keeps it as Proton's stock copy, and warns
#             (before: it became the "stock" d3d12.dll silently)
#   mmissing  steamac.gfx=mullion without Mullion's files leaves everything stock
# usage: scripts/test/steam-gfx-path.sh
set -u
S=$(cd "$(dirname "$0")/../.." && pwd)/guest/layer/usr/lib/steamac/steam-gfx-path
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fails=0; fail() { fails=$((fails + 1)); echo "FAIL [$1]: $2"; }
blob() { head -c 300 /dev/zero | tr '\0' "$2" > "$1"; }
ST=$T/steam; P1="$ST/steamapps/common/Proton - Experimental"; P2=$ST/compatibilitytools.d/Proton-custom
for p in "$P1" "$P2"; do
  mkdir -p "$p/files/lib/wine/dxvk/x86_64-windows" "$p/files/lib/wine/x86_64-windows" "$p/files/lib/wine/x86_64-unix"
  for f in d3d11 dxgi d3d10core d3d9; do blob "$p/files/lib/wine/dxvk/x86_64-windows/$f.dll" v; done
  blob "$p/files/lib/wine/x86_64-windows/d3d11.dll" w; echo 1 > "$p/version"
  mkdir -p "$p/files/share/default_pfx/drive_c/windows/system32"
  ln -s ../../../../../lib/wine/x86_64-windows/d3d11.dll "$p/files/share/default_pfx/drive_c/windows/system32/d3d11.dll"
done
# an ARM64 Proton: DXVK/vkd3d-proton sets, but no x86-64 Wine; it must stay stock
P3=$ST/steamapps/common/Proton-arm64
mkdir -p "$P3/files/lib/wine/dxvk/x86_64-windows" "$P3/files/lib/wine/vkd3d-proton/x86_64-windows"
for f in d3d11 dxgi d3d10core; do blob "$P3/files/lib/wine/dxvk/x86_64-windows/$f.dll" a; done
blob "$P3/files/lib/wine/vkd3d-proton/x86_64-windows/d3d12.dll" a
mkdir -p "$P1/files/lib/wine/vkd3d-proton/x86_64-windows"
for f in d3d12 d3d12core; do blob "$P1/files/lib/wine/vkd3d-proton/x86_64-windows/$f.dll" r; done
for a in 100 200; do
  s=$ST/steamapps/compatdata/$a/pfx/drive_c/windows/system32; mkdir -p "$s"
  for f in d3d11 dxgi d3d10core; do cp "$P1/files/lib/wine/dxvk/x86_64-windows/$f.dll" "$s/"; done; blob "$s/kernel32.dll" k
done
X=$T/dxmt; mkdir -p "$X/x86_64-windows" "$X/x86_64-unix"
for f in d3d11 dxgi d3d10core winemetal d3d12; do blob "$X/x86_64-windows/$f.dll" m; done; blob "$X/x86_64-unix/winemetal.so" s
snap() { (cd "$T" && find steam -type f | LC_ALL=C sort | while read -r f; do echo "$f $(cksum < "$f")"; done
  find steam -type l | LC_ALL=C sort | while read -r f; do echo "$f -> $(readlink "$f")"; done); }
run() { echo "$1" > "$T/cmdline"
  STEAMAC_CMDLINE=$T/cmdline STEAMAC_STEAM_ROOT=$ST STEAMAC_DXMT_DIR=${2:-$X} STEAMAC_MULLION_DIR=${MU:-$T/none} \
    STEAMAC_GFX_STATE=$T/state HOME=$T bash "$S" 2>>"$T/log"; }
isdxmt() { [ "$(head -c 64 "$1" | tr -d m)" = "" ] && [ "$(tail -c +65 "$1" | head -c 16 | tr -d '\0')" = "" ] \
  && [ "$(tail -c +81 "$1" | tr -d m)" = "" ]; }

stock=$(snap)
run "quiet steamac.gfx=dxmt"
for p in "$P1" "$P2"; do
  W=$p/files/lib/wine
  for f in d3d11 dxgi d3d10core; do isdxmt "$W/dxvk/x86_64-windows/$f.dll" || fail dxmt "$p $f.dll is not DXMT (native copy)"; done
  cmp -s "$W/dxvk/x86_64-windows/d3d9.dll" "$P1/files/lib/wine/dxvk/x86_64-windows/d3d9.dll" || fail dxmt "d3d9 touched"
  cmp -s "$W/x86_64-windows/winemetal.dll" "$X/x86_64-windows/winemetal.dll" || fail dxmt "$p winemetal.dll"
  cmp -s "$W/x86_64-unix/winemetal.so" "$X/x86_64-unix/winemetal.so" || fail dxmt "$p winemetal.so"
  cmp -s "$p/files/share/default_pfx/drive_c/windows/system32/winemetal.dll" "$X/x86_64-windows/winemetal.dll" \
    || fail dxmt "$p prefix template has no winemetal.dll link"
done
V=$P1/files/lib/wine/vkd3d-proton/x86_64-windows
for f in dxvk/x86_64-windows/d3d11 vkd3d-proton/x86_64-windows/d3d12; do
  [ "$(tr -d a < "$P3/files/lib/wine/$f.dll")" = "" ] || fail arm64 "ARM64 Proton $f.dll touched"; done
grep -q "cannot set up" "$T/log" && fail arm64 "error for the ARM64 Proton: $(grep 'cannot set up' "$T/log" | head -1)"
isdxmt "$V/d3d12.dll" || fail d3d12 "P1 d3d12.dll is not DXMT (native copy)"
[ "$(tr -d r < "$V/d3d12core.dll")" = "" ] || fail d3d12 "d3d12core.dll touched"
[ -e "$P2/files/lib/wine/vkd3d-proton" ] && fail d3d12 "P2 got a vkd3d-proton directory"
for a in 100 200; do
  cmp -s "$ST/steamapps/compatdata/$a/pfx/drive_c/windows/system32/winemetal.dll" "$X/x86_64-windows/winemetal.dll" || fail dxmt "prefix $a placeholder"
done

# a game start in between: Proton copies its (now DXMT) set into prefix 100
for f in d3d11 dxgi d3d10core; do cp "$P1/files/lib/wine/dxvk/x86_64-windows/$f.dll" "$ST/steamapps/compatdata/100/pfx/drive_c/windows/system32/"; done
s1=$(snap); run "steamac.gfx=dxmt"; [ "$(snap)" = "$s1" ] || fail again "second run changed files"

# Proton update: Steam writes new stock files (u) and a new version
for f in d3d11 dxgi d3d10core d3d9; do blob "$P1/files/lib/wine/dxvk/x86_64-windows/$f.dll" u; done; echo 2 > "$P1/version"
upd=$(cd "$T" && cksum < "$P1/files/lib/wine/dxvk/x86_64-windows/dxgi.dll")
run "steamac.gfx=dxmt"
isdxmt "$P1/files/lib/wine/dxvk/x86_64-windows/dxgi.dll" || fail update "DXMT not re-applied after the update"

# layer update: a new DXMT payload (n) replaces the old DXMT files, Proton's originals stay kept
X2=$T/dxmt2; mkdir -p "$X2/x86_64-windows" "$X2/x86_64-unix"
for f in d3d11 dxgi d3d10core winemetal; do blob "$X2/x86_64-windows/$f.dll" n; done; blob "$X2/x86_64-unix/winemetal.so" t
rm -f "$T"/state/orig/*/installed   # state as left by a version of this script that kept no checksums
run "steamac.gfx=dxmt" "$X2"
[ "$(head -c 64 "$P2/files/lib/wine/dxvk/x86_64-windows/d3d11.dll" | tr -d n)" = "" ] || fail payload "new payload not installed"
[ "$(tr -d r < "$V/d3d12.dll")" = "" ] || fail d3d12 "payload without d3d12.dll left DXMT's d3d12.dll in place"
X=$X2
run "steamac.gfx=vulkan" "$X2"
[ "$(cksum < "$P1/files/lib/wine/dxvk/x86_64-windows/dxgi.dll")" = "$upd" ] || fail vulkan "P1 dxgi is not the updated stock file"
for f in d3d11 dxgi d3d10core; do
  cmp -s "$P2/files/lib/wine/dxvk/x86_64-windows/$f.dll" "$P2/files/lib/wine/dxvk/x86_64-windows/d3d9.dll" || fail vulkan "P2 $f.dll not stock"
done
# expected: the stock tree with P1's update applied, nothing added
expect=$( (echo "$stock" | grep -v "Proton - Experimental/files/lib/wine/dxvk\|Proton - Experimental/version\|/compatdata/"; \
  cd "$T" && for f in "steam/steamapps/common/Proton - Experimental/files/lib/wine/dxvk/x86_64-windows/"*.dll "steam/steamapps/common/Proton - Experimental/version"; do echo "$f $(cksum < "$f")"; done) | LC_ALL=C sort)
got=$(snap | grep -v "/compatdata/" | LC_ALL=C sort)
[ "$got" = "$expect" ] || fail vulkan "Proton trees differ from stock: $(diff <(echo "$expect") <(echo "$got") | head -4 | tr '\n' '|')"
for a in 100 200; do
  s=$ST/steamapps/compatdata/$a/pfx
  [ ! -e "$s/drive_c/windows/system32/winemetal.dll" ] && [ ! -e "$s/.steamac-dxmt" ] || fail vulkan "prefix $a residue"
  for f in d3d11 dxgi d3d10core; do isdxmt "$s/drive_c/windows/system32/$f.dll" && fail vulkan "prefix $a $f.dll still DXMT"; done
done
[ -z "$(ls -A "$T/state/orig" 2>/dev/null)" ] || fail vulkan "backups left in state"
s2=$(snap); run "steamac.gfx=vulkan" "$X2"; [ "$(snap)" = "$s2" ] || fail vulkan "second Vulkan run changed files"

run "steamac.gfx=dxmt" "$T/none"; [ "$(snap)" = "$s2" ] || fail missing "changed files without DXMT files"
# poisoned backups: a DXMT build of another payload carries the dxmt:: symbols
dxblob() { { printf '_ZN4dxmt6Logger10s_instanceE'; head -c 270 /dev/zero | tr '\0' "$2"; } > "$1"; }
D2=$P2/files/lib/wine/dxvk/x86_64-windows
run "steamac.gfx=dxmt" "$X2"
B2=$(echo "$T"/state/orig/*/ | tr ' ' '\n' | while read -r b; do grep -qx "$P2" "$b/path" && echo "$b"; done)
dxblob "$B2/d3d11.dll" z                       # a poisoned kept original
rm -rf "$T/state/stock"                        # and no stock copy either
cp "$D2/d3d11.dll" "$T/ours.dll"; : > "$T/log"
run "steamac.gfx=vulkan" "$X2"
cmp -s "$D2/d3d11.dll" "$B2/d3d11.dll" 2>/dev/null && fail poisoned "poisoned backup restored"
grep -q "d3d11.dll of $P2 is DXMT's.*Verify integrity" "$T/log" || fail poisoned "no log line: $(cat "$T/log")"
cmp -s "$D2/d3d11.dll" "$T/ours.dll" || fail poisoned "d3d11.dll changed although nothing sound was kept"
cmp -s "$D2/dxgi.dll" "$D2/d3d9.dll" || fail poisoned "sound kept original of dxgi.dll not restored"
[ -z "$(ls -A "$T/state/orig" 2>/dev/null)" ] || fail poisoned "poisoned backup left in state"
# a DXMT file in Proton's tree that no payload or checksum knows is not kept as Proton's original
dxblob "$D2/dxgi.dll" y; run "steamac.gfx=dxmt" "$X2"
grep -rl dxmt "$T/state" >/dev/null 2>&1 && fail poisoned "unknown DXMT file kept as an original"
# foreign: a fresh tree, two real-looking DXMT payloads (dxmt:: symbols), checksums lost between bundles
rm -rf "$T/state" "$ST/steamapps/compatdata"; : > "$T/log"
for f in d3d11 dxgi d3d10core d3d9; do blob "$P1/files/lib/wine/dxvk/x86_64-windows/$f.dll" v; done
DA=$T/dxa; DB=$T/dxb; mkdir -p "$DA/x86_64-windows" "$DA/x86_64-unix" "$DB/x86_64-windows" "$DB/x86_64-unix"
for f in d3d11 dxgi d3d10core winemetal; do dxblob "$DA/x86_64-windows/$f.dll" a; dxblob "$DB/x86_64-windows/$f.dll" b; done
blob "$DA/x86_64-unix/winemetal.so" s; blob "$DB/x86_64-unix/winemetal.so" s
D1=$P1/files/lib/wine/dxvk/x86_64-windows
run "steamac.gfx=dxmt" "$DA"; rm -f "$T"/state/orig/*/installed
run "steamac.gfx=dxmt" "$DB"; rm -f "$T"/state/orig/*/installed
run "steamac.gfx=vulkan" "$DA"
for f in d3d11 dxgi d3d10core; do
  cmp -s "$D1/$f.dll" "$D1/d3d9.dll" || fail foreign "P1 $f.dll not Proton's after A -> B -> vulkan"
done
[ -z "$(ls -A "$T/state/orig" 2>/dev/null)" ] || fail foreign "backups left after a full restore"
# noorig: DXMT's dxgi.dll in Proton's tree with nothing kept
run "steamac.gfx=dxmt" "$DB"; rm -rf "$T"/state/orig/*/dxgi.dll "$T"/state/stock/*/dxgi.dll; : > "$T/log"
run "steamac.gfx=vulkan" "$DB"
cmp -s "$D1/d3d11.dll" "$D1/d3d9.dll" || fail noorig "kept d3d11.dll not restored"
grep -q "dxmt" "$D1/dxgi.dll" || fail noorig "dxgi.dll changed although no original was kept"
grep -q "WARNING: no original of dxgi.dll kept for $P1.*Verify integrity" "$T/log" || fail noorig "no warning: $(cat "$T/log")"
grep -q "DXVK back in $P1" "$T/log" && fail noorig "claims DXVK back with DXMT's dxgi.dll in place"
# oldlayer: fresh state, DXMT boot, then an older layer's Vulkan boot dropped orig/ and left DXMT's files
rm -rf "$T/state"; for f in d3d11 dxgi d3d10core d3d9; do blob "$D1/$f.dll" v; blob "$D2/$f.dll" v; done; : > "$T/log"
run "steamac.gfx=dxmt" "$DB"; rm -rf "$T/state/orig"; rm -f "$P1/files/lib/wine/.steamac-dxmt"
run "steamac.gfx=dxmt" "$DB"                   # a DXMT boot first: the stock copies become the kept originals
for f in d3d11 dxgi d3d10core; do grep -q dxmt "$D1/$f.dll" || fail oldlayer "P1 $f.dll not DXMT"; done
for f in d3d11 dxgi d3d10core; do grep -rlq dxmt "$T/state/orig" && fail oldlayer "DXMT file kept as an original"; done
rm -rf "$T/state/orig"; rm -f "$P1/files/lib/wine/.steamac-dxmt" "$P1/files/lib/wine/x86_64-windows/winemetal.dll"
run "steamac.gfx=vulkan" "$DA"
for f in d3d11 dxgi d3d10core; do cmp -s "$D1/$f.dll" "$D1/d3d9.dll" || fail oldlayer "P1 $f.dll not restored from the stock copy"; done
grep -q "WARNING" "$T/log" && fail oldlayer "warning although a stock copy existed: $(grep WARNING "$T/log" | head -1)"
# stale stock copy: Proton updated after the copy was made, then DXMT left behind without originals
run "steamac.gfx=dxmt" "$DB"; rm -rf "$T/state/orig"; echo 9 > "$P1/version"; : > "$T/log"
run "steamac.gfx=vulkan" "$DA"
grep -q dxmt "$D1/dxgi.dll" || fail oldlayer "stale stock copy (older Proton version) restored"
grep -q "WARNING: no original of" "$T/log" || fail oldlayer "no warning for a stale stock copy"

# mullion: a fresh tree and state; Mullion's payload names itself in every file but d3d12.dll (a loader)
rm -rf "$T/state" "$ST/steamapps/compatdata"; : > "$T/log"
for p in "$P1" "$P2"; do
  for f in d3d11 dxgi d3d10core d3d9; do blob "$p/files/lib/wine/dxvk/x86_64-windows/$f.dll" v; done; echo 1 > "$p/version"
  rm -f "$p/files/lib/wine/x86_64-windows/winemetal.dll" "$p/files/lib/wine/x86_64-unix/winemetal.so" "$p/files/lib/wine/.steamac-dxmt"
done
for f in d3d12 d3d12core; do blob "$V/$f.dll" r; done
for a in 100 200; do
  s=$ST/steamapps/compatdata/$a/pfx/drive_c/windows/system32; mkdir -p "$s"
  for f in d3d11 dxgi d3d10core; do cp "$D1/$f.dll" "$s/"; done; blob "$s/kernel32.dll" k
done
mublob() { { printf 'mullion::D3D12Device'; head -c 279 /dev/zero | tr '\0' "$2"; } > "$1"; }
MU=$T/mullion; mkdir -p "$MU/x86_64-windows" "$MU/x86_64-unix"
for f in d3d11 dxgi d3d10core d3d12core winemetal; do mublob "$MU/x86_64-windows/$f.dll" e; done
blob "$MU/x86_64-windows/d3d12.dll" q; blob "$MU/x86_64-unix/winemetal.so" o
mstock=$(snap)
ismu() { cmp -s <(head -c 64 "$1") <(head -c 64 "$2") && [ "$(tail -c +65 "$1" | head -c 16 | tr -d '\0')" = "" ] \
  && cmp -s <(tail -c +81 "$1") <(tail -c +81 "$2"); }   # installed file $1 is a native copy of payload file $2
USET="user_settings = {\"MULLION_CONFIG\": \"d3d12.cpuVisibleHeapMemory=True\"}"
run "quiet steamac.gfx=mullion"
for p in "$P1" "$P2"; do
  W=$p/files/lib/wine
  for f in d3d11 dxgi d3d10core; do ismu "$W/dxvk/x86_64-windows/$f.dll" "$MU/x86_64-windows/$f.dll" || fail mullion "$p $f.dll is not Mullion's (native copy)"; done
  cmp -s "$W/dxvk/x86_64-windows/d3d9.dll" <(head -c 300 /dev/zero | tr '\0' v) || fail mullion "$p d3d9 touched"
  cmp -s "$W/x86_64-windows/winemetal.dll" "$MU/x86_64-windows/winemetal.dll" || fail mullion "$p winemetal.dll"
  cmp -s "$W/x86_64-unix/winemetal.so" "$MU/x86_64-unix/winemetal.so" || fail mullion "$p winemetal.so"
  [ "$(tail -n 1 "$p/user_settings.py" 2>/dev/null)" = "$USET" ] || fail mullion "$p user_settings.py: $(cat "$p/user_settings.py" 2>&1 | tr '\n' '|')"
  [ "$(cat "$W/.steamac-dxmt" 2>/dev/null)" = mullion ] || fail mullion "$p mark does not name mullion"
done
for f in d3d12 d3d12core; do ismu "$V/$f.dll" "$MU/x86_64-windows/$f.dll" || fail mullion "P1 $f.dll is not Mullion's (native copy)"; done
[ -e "$P2/files/lib/wine/vkd3d-proton" ] && fail mullion "P2 got a vkd3d-proton directory"
[ -e "$P3/user_settings.py" ] && fail mullion "ARM64 Proton got a user_settings.py"
[ "$(tr -d a < "$P3/files/lib/wine/vkd3d-proton/x86_64-windows/d3d12.dll")" = "" ] || fail mullion "ARM64 Proton d3d12.dll touched"
for a in 100 200; do
  cmp -s "$ST/steamapps/compatdata/$a/pfx/drive_c/windows/system32/winemetal.dll" "$MU/x86_64-windows/winemetal.dll" || fail mullion "prefix $a placeholder"
done
grep -q "Mullion in $P1" "$T/log" || fail mullion "no 'Mullion in' log line: $(tr '\n' '|' < "$T/log")"
s3=$(snap); run "steamac.gfx=mullion"; [ "$(snap)" = "$s3" ] || fail mullion "second run changed files"
# switch: mullion -> dxmt
X=$T/dxmt; run "steamac.gfx=dxmt"
for f in d3d11 dxgi d3d10core; do isdxmt "$D1/$f.dll" || fail switch "P1 $f.dll is not DXMT's after mullion -> dxmt"; done
isdxmt "$V/d3d12.dll" || fail switch "P1 d3d12.dll is not DXMT's after mullion -> dxmt"
[ "$(tr -d r < "$V/d3d12core.dll")" = "" ] || fail switch "vkd3d-proton's d3d12core.dll not back after mullion -> dxmt"
cmp -s "$P1/files/lib/wine/x86_64-unix/winemetal.so" "$X/x86_64-unix/winemetal.so" || fail switch "winemetal.so not DXMT's"
for p in "$P1" "$P2"; do
  [ -e "$p/user_settings.py" ] && fail switch "$p user_settings.py left on the DXMT path"
  [ "$(cat "$p/files/lib/wine/.steamac-dxmt")" = dxmt ] || fail switch "$p mark does not name dxmt"
done
# dxmt -> mullion -> vulkan: stock byte for byte
run "steamac.gfx=mullion"; isdxmt "$V/d3d12.dll" && fail switch "DXMT's d3d12.dll left on the Mullion path"
ismu "$V/d3d12core.dll" "$MU/x86_64-windows/d3d12core.dll" || fail switch "d3d12core.dll not Mullion's after dxmt -> mullion"
run "steamac.gfx=vulkan"
got=$(snap | grep -v "/compatdata/"); want=$(echo "$mstock" | grep -v "/compatdata/")
[ "$got" = "$want" ] || fail switch "Proton trees differ from stock after mullion -> vulkan: $(diff <(echo "$want") <(echo "$got") | head -4 | tr '\n' '|')"
for a in 100 200; do
  s=$ST/steamapps/compatdata/$a/pfx/drive_c/windows/system32
  [ -e "$s/winemetal.dll" ] && fail switch "prefix $a winemetal.dll left"
  for f in d3d11 dxgi d3d10core; do grep -q mullion "$s/$f.dll" && fail switch "prefix $a $f.dll still Mullion's"; done
done
# settings: the user's own user_settings.py stays as it is
printf 'user_settings = {"PROTON_LOG": "1"}\n' > "$P2/user_settings.py"; cp "$P2/user_settings.py" "$T/user.py"; : > "$T/log"
run "steamac.gfx=mullion"
cmp -s "$P2/user_settings.py" "$T/user.py" || fail settings "the user's user_settings.py was rewritten"
grep -q "WARNING: $P2/user_settings.py is not steamac's" "$T/log" || fail settings "no warning: $(tr '\n' '|' < "$T/log")"
[ "$(tail -n 1 "$P1/user_settings.py")" = "$USET" ] || fail settings "P1 lost its user_settings.py"
run "steamac.gfx=vulkan"
cmp -s "$P2/user_settings.py" "$T/user.py" || fail settings "the user's user_settings.py was removed on the Vulkan path"
[ -e "$P1/user_settings.py" ] && fail settings "our user_settings.py left on the Vulkan path"
rm -f "$P2/user_settings.py"
# mlost: the kept originals and checksums lost after a Mullion boot (an older layer's Vulkan boot dropped them)
run "steamac.gfx=mullion"; rm -rf "$T/state/orig"; rm -f "$P1/files/lib/wine/.steamac-dxmt"; : > "$T/log"
run "steamac.gfx=vulkan"
[ "$(tr -d r < "$V/d3d12.dll")" = "" ] || fail mlost "Mullion's d3d12.dll (no marker) not replaced by vkd3d-proton's stock copy"
[ "$(tr -d r < "$V/d3d12core.dll")" = "" ] || fail mlost "vkd3d-proton's d3d12core.dll not back from the stock copy"
for f in d3d11 dxgi d3d10core; do cmp -s "$D1/$f.dll" "$D1/d3d9.dll" || fail mlost "P1 $f.dll not restored from the stock copy"; done
grep -rlq q "$T/state/stock" 2>/dev/null && fail mlost "Mullion's d3d12.dll kept as Proton's stock copy"
# mnomark: only Mullion's (marker-less) d3d12.dll left behind, nothing kept
rm -rf "$T/state"; : > "$T/log"
for f in d3d12 d3d12core; do blob "$V/$f.dll" r; done
cp "$MU/x86_64-windows/d3d12.dll" "$V/d3d12.dll"; dd if=/dev/zero of="$V/d3d12.dll" bs=1 seek=64 count=16 conv=notrunc status=none
run "steamac.gfx=vulkan"
grep -rlq q "$T/state/stock" 2>/dev/null && fail mnomark "Mullion's d3d12.dll kept as Proton's stock copy"
grep -q "WARNING: no original of d3d12.dll kept for $P1" "$T/log" || fail mnomark "no warning: $(tr '\n' '|' < "$T/log")"
for f in d3d12 d3d12core; do blob "$V/$f.dll" r; done; rm -rf "$T/state"
# mmissing: no Mullion files in the layer
s4=$(snap); MU=$T/none run "steamac.gfx=mullion"; [ "$(snap)" = "$s4" ] || fail mmissing "changed files without Mullion's files"
echo "steam-gfx-path: $([ $fails = 0 ] && echo passed || echo FAILED)"
exit $((fails > 0))
