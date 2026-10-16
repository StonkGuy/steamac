#!/usr/bin/env bash
# Check a DMG built by host/launcher/dist.sh, without starting a VM:
#   layout       FX Steam Launcher.app + the /Applications link
#   signature    codesign --verify --deep --strict; every Mach-O signed the expected way
#                (adhoc: Signature=adhoc; developer-id: one team, hardened runtime, timestamp)
#   entitlements hypervisor, disable-library-validation, audio-input on the bundle
#   Info.plist   build identity present, the dev-only SteamacBuildOut key (build tree path) gone
#   home paths   no /Users/<name> string of a build machine other than the allowed ones
#   receipts     with --receipts: the bundled kernel/initramfs/layer match their receipts and this
#                checkout's sources (scripts/guest-artifacts.py check-bundle)
#
#   scripts/test/dist-verify.sh [--receipts] <dmg> [adhoc|developer-id]     (default: adhoc)
#
# Env: DIST_ALLOWED_HOMES  space-separated /Users names that may appear in the binaries
#                          (default "runner Shared": GitHub's macOS runners build in /Users/runner)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
receipts=0
if [[ ${1:-} == --receipts ]]; then receipts=1; shift; fi
DMG=${1:?usage: dist-verify.sh [--receipts] <dmg> [adhoc|developer-id]}
MODE=${2:-adhoc}
NAME="FX Steam Launcher"
read -r -a allowed <<<"${DIST_ALLOWED_HOMES:-runner Shared}"

fails=0
ok() { echo "  ok   $*"; }
bad() { echo "  FAIL $*" >&2; fails=$((fails + 1)); }

[[ -f $DMG ]] || { echo "dist-verify.sh: no such file: $DMG" >&2; exit 2; }
[[ $MODE == adhoc || $MODE == developer-id ]] || { echo "dist-verify.sh: unknown mode $MODE" >&2; exit 2; }
MNT=$(mktemp -d "${TMPDIR:-/tmp}/dist-verify.XXXXXXXX")
trap 'hdiutil detach -quiet "$MNT" 2>/dev/null || true; rmdir "$MNT" 2>/dev/null || true' EXIT
hdiutil attach -quiet -nobrowse -readonly -noautoopen -mountpoint "$MNT" "$DMG" \
    || { echo "dist-verify.sh: cannot attach $DMG (already attached elsewhere? hdiutil info)" >&2; exit 2; }
APP="$MNT/$NAME.app"

echo "$(basename "$DMG"): sha256 $(shasum -a 256 "$DMG" | cut -d' ' -f1), expecting $MODE"
if [[ -d $APP ]]; then ok "$NAME.app"; else bad "missing $NAME.app"; exit 1; fi
if [[ $(readlink "$MNT/Applications" 2>/dev/null) == /Applications ]]; then ok "/Applications link"; else bad "/Applications link"; fi

if codesign --verify --deep --strict "$APP" 2>/dev/null; then ok "codesign --verify --deep --strict"; else bad "codesign --verify --deep --strict"; fi
count=0
teams=
while IFS= read -r -d '' f; do
    file -b "$f" | grep -q '^Mach-O' || continue
    count=$((count + 1))
    info=$(codesign -dv "$f" 2>&1) || { bad "${f#"$APP/"}: unsigned"; continue; }
    if [[ $MODE == adhoc ]]; then
        grep -q '^Signature=adhoc$' <<<"$info" || bad "${f#"$APP/"}: not signed ad hoc"
    else
        grep -q 'flags=0x10000(runtime)' <<<"$info" || bad "${f#"$APP/"}: hardened runtime flag missing"
        grep -q '^Timestamp=' <<<"$info" || bad "${f#"$APP/"}: no secure timestamp"
        teams+=$(sed -n 's/^TeamIdentifier=//p' <<<"$info")$'\n'
    fi
done < <(find "$APP/Contents" -type f -print0)
if [[ $MODE == developer-id ]]; then
    n=$(sort -u <<<"$teams" | grep -c . || true)
    [[ $n == 1 ]] || bad "Mach-O files are signed by $n teams, expected 1"
fi
ok "$count Mach-O files examined"

ent=$(codesign -d --entitlements - --xml "$APP" 2>/dev/null | plutil -p - 2>/dev/null || true)
for key in com.apple.security.hypervisor com.apple.security.cs.disable-library-validation com.apple.security.device.audio-input; do
    if grep -q "\"$key\" => \(1\|true\)" <<<"$ent"; then ok "entitlement $key"; else bad "entitlement $key missing"; fi
done

plist="$APP/Contents/Info.plist"
pl() { /usr/libexec/PlistBuddy -c "Print :$1" "$plist" 2>/dev/null; }
if pl SteamacBuildOut >/dev/null; then bad "Info.plist still has SteamacBuildOut ($(pl SteamacBuildOut))"; else ok "no SteamacBuildOut"; fi
if commit=$(pl SteamacGitCommit) && [[ $commit =~ ^[0-9a-f]{10}$ ]]; then
    ok "version $(pl CFBundleShortVersionString), commit $commit"
else
    bad "Info.plist has no SteamacGitCommit"
fi
if [[ $MODE == adhoc ]] && pl SteamacDistTeamID >/dev/null; then bad "ad hoc build claims a SteamacDistTeamID"; fi

homes=$(LC_ALL=C grep -r -a -o -h -E '/Users/[A-Za-z0-9._-]+' "$APP" 2>/dev/null | sort -u | sed 's|^/Users/||' || true)
for home in $homes; do
    known=0
    for a in "${allowed[@]}"; do [[ $home == "$a" ]] && known=1; done
    [[ $known == 1 ]] || bad "a file names /Users/$home"
done
ok "home paths: ${homes:-none}"

if [[ $receipts == 1 ]]; then
    if python3 "$ROOT/scripts/guest-artifacts.py" check-bundle "$APP/Contents/Resources"; then
        ok "guest receipts match the bundled images and this checkout"
    else
        bad "guest receipts"
    fi
fi

if [[ $fails == 0 ]]; then echo "dist-verify: PASS"; else echo "dist-verify: $fails FAILED" >&2; exit 1; fi
