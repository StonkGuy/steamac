#!/bin/sh
# SPDX-License-Identifier: MIT
# Checks that a plain `open` of the app starts the graphics path saved in Settings: for each path, write the settings
# that choose it (dxmt: Advanced > "Allow DXMT"; mullion, experimental: that plus the `mullion` setting; vulkan: Allow
# DXMT off), open the app in the background with --selftest-graphics-path only, and expect its PASS line naming that
# path. A path whose executor this build lacks must fall back to Vulkan. The previously saved values are restored.
# usage: scripts/test/graphics-path.sh [APP]   default: work/out/FX Steam Launcher.app
APP=${1:-"work/out/FX Steam Launcher.app"}
DOM=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$APP/Contents/Info.plist") || exit 1
KEYS="wmtr mullion graphicsPath"
saved=$(mktemp); out=$(mktemp); fail=0
for k in $KEYS; do printf '%s %s\n' "$k" "$(defaults read "$DOM" "$k" 2>/dev/null || echo -)" >> "$saved"; done
for g in dxmt mullion vulkan; do
  defaults delete "$DOM" graphicsPath 2>/dev/null
  case $g in
    dxmt) defaults write "$DOM" wmtr -bool true; defaults write "$DOM" mullion -bool false; lib=libwmtr_host.dylib ;;
    mullion) defaults write "$DOM" wmtr -bool true; defaults write "$DOM" mullion -bool true; lib=libwmtr_host_mullion.dylib ;;
    vulkan) defaults write "$DOM" wmtr -bool false; lib= ;;
  esac
  : > "$out"
  open -g -W -n --stderr "$out" --stdout "$out" "$APP" --args --selftest-graphics-path
  if grep -q "selftest-graphics-path: resolved $g," "$out" && grep -q 'selftest-graphics-path: PASS' "$out"; then
    echo "ok   $g"
  elif [ -n "$lib" ] && grep -q 'resolved vulkan' "$out" && ! [ -f "$APP/Contents/Frameworks/$lib" ]; then
    echo "ok   $g (not in this build: Vulkan used)"
  else
    echo "FAIL $g: $(grep selftest-graphics-path "$out" | tr '\n' '|')"; fail=1
  fi
done
while read -r k v; do
  case $v in -) defaults delete "$DOM" "$k" 2>/dev/null ;; 0|1) defaults write "$DOM" "$k" -bool "$v" ;; *) defaults write "$DOM" "$k" -string "$v" ;; esac
done < "$saved"
rm -f "$out" "$saved"
exit $fail
