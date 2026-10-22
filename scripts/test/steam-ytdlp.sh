#!/bin/bash
# SPDX-License-Identifier: MIT
# guest/layer/usr/lib/steamac/steam-ytdlp (steamac.ytdlp= on the kernel command line) and its unit wiring:
#   default   no option: VRChat's yt-dlp.exe untouched
#   native    steamac.ytdlp=native: original kept as yt-dlp.exe.steamac-orig, stub installed read-only, in every
#             Steam library's VRChat prefix; a second run changes nothing
#   update    VRChat replaced the stub with a newer yt-dlp.exe: that one becomes the kept original
#   fresh     a prefix where VRChat never ran (no Tools dir) is left alone
#   off       option removed: original back byte for byte and writable; a stub with no original is removed
#   nostub    steamac.ytdlp=native without the stub in the layer leaves everything stock
#   serve     --serve exits 0 without the option
#   unit      steam.service runs the script; steamac-ytdlp.service serves
# usage: scripts/test/steam-ytdlp.sh
set -u
R=$(cd "$(dirname "$0")/../.." && pwd)/guest/layer
S=$R/usr/lib/steamac/steam-ytdlp
T=$(mktemp -d); trap 'chmod -R u+w "$T"; rm -rf "$T"' EXIT
fails=0; fail() { fails=$((fails + 1)); echo "FAIL [$1]: $2"; }
TOOLS=steamapps/compatdata/438100/pfx/drive_c/users/steamuser/AppData/LocalLow/VRChat/VRChat/Tools
P=$T/payload; mkdir -p "$P"; printf 'MZ steamac yt-dlp stub test\n' > "$P/yt-dlp.exe"
ST=$T/steam; L2=$T/lib2
mkdir -p "$ST/$TOOLS" "$L2/$TOOLS" "$ST/steamapps/compatdata/999/pfx"
printf '"libraryfolders"\n{\n\t"0"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n\t"1"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n}\n' "$ST" "$L2" \
    > "$ST/steamapps/libraryfolders.vdf"
head -c 5000 /dev/urandom > "$T/orig1"; head -c 6000 /dev/urandom > "$T/orig2"
cp "$T/orig1" "$ST/$TOOLS/yt-dlp.exe"; cp "$T/orig1" "$L2/$TOOLS/yt-dlp.exe"
chmod 0644 "$ST/$TOOLS/yt-dlp.exe"

run() { # cmdline [payload] [args]
    printf '%s\n' "$1" > "$T/cmdline"
    STEAMAC_CMDLINE=$T/cmdline STEAMAC_STEAM_ROOT=$ST STEAMAC_YTDLP_PAYLOAD=${2:-$P} bash "$S" ${3:-} > "$T/out" 2>&1
}
snap() { (cd "$T" && find steam lib2 -type f -print0 | LC_ALL=C sort -z | xargs -0 ls -l | awk '{print $1, $NF}'
          find steam lib2 -type f -print0 | LC_ALL=C sort -z | xargs -0 cksum); }

s0=$(snap)
run "console=hvc0 rootwait" || fail default "exit status $?"
[ "$(snap)" = "$s0" ] || fail default "files changed without the option"
run "steamac.ytdlp=native" "$T/none" || fail nostub "exit status $?"
[ "$(snap)" = "$s0" ] || fail nostub "files changed without a stub payload"

run "console=hvc0 steamac.ytdlp=native" || fail native "exit status $?"
for d in "$ST" "$L2"; do
    cmp -s "$P/yt-dlp.exe" "$d/$TOOLS/yt-dlp.exe" || fail native "$d: stub not installed"
    cmp -s "$T/orig1" "$d/$TOOLS/yt-dlp.exe.steamac-orig" || fail native "$d: original not kept"
    [ -w "$d/$TOOLS/yt-dlp.exe" ] && fail native "$d: stub writable (VRChat would replace it)"
done
s1=$(snap); run "steamac.ytdlp=native" || fail native "exit status $?"
[ "$(snap)" = "$s1" ] || fail native "second run changed files"

# VRChat managed to put a newer yt-dlp.exe in place (writable dir): it becomes the original
chmod u+w "$ST/$TOOLS/yt-dlp.exe"; cp "$T/orig2" "$ST/$TOOLS/yt-dlp.exe"
run "steamac.ytdlp=native" || fail update "exit status $?"
cmp -s "$T/orig2" "$ST/$TOOLS/yt-dlp.exe.steamac-orig" || fail update "newer original not kept"
cmp -s "$P/yt-dlp.exe" "$ST/$TOOLS/yt-dlp.exe" || fail update "stub not reinstalled"
[ -e "$ST/steamapps/compatdata/999/pfx/drive_c" ] && fail fresh "other prefix touched"

mkdir -p "$T/fresh/steamapps/compatdata/438100/pfx"
STEAMAC_CMDLINE=$T/cmdline STEAMAC_STEAM_ROOT=$T/fresh STEAMAC_YTDLP_PAYLOAD=$P bash "$S" > "$T/out" 2>&1 \
    || fail fresh "exit status $?"
[ -n "$(find "$T/fresh" -type f)" ] && fail fresh "files created in a prefix VRChat never ran in"

rm -f "$L2/$TOOLS/yt-dlp.exe.steamac-orig"
run "steamac.ytdlp=off" || fail off "exit status $?"
cmp -s "$T/orig2" "$ST/$TOOLS/yt-dlp.exe" || fail off "original not restored"
[ -w "$ST/$TOOLS/yt-dlp.exe" ] || fail off "restored original not writable"
[ -e "$ST/$TOOLS/yt-dlp.exe.steamac-orig" ] && fail off "backup left behind"
[ -e "$L2/$TOOLS/yt-dlp.exe" ] && fail off "stub without original not removed"
s2=$(snap); run "console=hvc0" || fail off "exit status $?"
[ "$(snap)" = "$s2" ] || fail off "second off run changed files"

run "console=hvc0" "$P" --serve || fail serve "exit status $?"
grep -q "not serving" "$T/out" || fail serve "served without the option"

U=$R/usr/lib/systemd/user
grep -qx 'ExecStartPre=-/usr/lib/steamac/steam-ytdlp' "$U/steam.service.d/50-steamac.conf" || fail unit "ExecStartPre missing"
grep -qx 'ExecStart=/usr/lib/steamac/steam-ytdlp --serve' "$U/steamac-ytdlp.service" || fail unit "service ExecStart missing"
for t in gamescope-session plasma-session; do
    [ "$(readlink "$U/$t.target.wants/steamac-ytdlp.service")" = ../steamac-ytdlp.service ] || fail unit "$t wants link"
done
[ -x "$S" ] && bash -n "$S" || fail unit "script not executable or invalid"
[ -x "$R/usr/lib/steamac/ytdlp/ytdlp_server.py" ] || fail unit "server not executable"

[ $fails = 0 ] && echo "steam-ytdlp: all checks passed" || { echo "steam-ytdlp: $fails failed"; exit 1; }
