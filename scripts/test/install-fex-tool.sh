#!/bin/sh
# SPDX-License-Identifier: MIT
# fex-eac/install-fex-tool.sh on a fake Steam FEX tool in a throw-away HOME: install backs up Valve's build once and
# swaps the stage in, a second install is a no-op, restore puts Valve's build back, an incomplete stage is refused
# without touching the tool, and restore without a backup fails without touching the tool (it used to copy from /).
# usage: scripts/test/install-fex-tool.sh
HERE=$(cd "$(dirname "$0")" && pwd); S="$HERE/../../fex-eac/install-fex-tool.sh"
d=$(mktemp -d); fail=0
BINS="FEX FEXBash FEXGetConfig FEXOfflineCompiler FEXServer FEXServerManager FEXpidof"
mk() { mkdir -p "$1/usr/bin"; for b in $BINS; do echo "$2 $b" > "$1/usr/bin/$b"; done; echo "$2" > "$1/FEXCompatTool"
       printf 'FEX describe\t%s\n' "$2" > "$1/VERSIONS.txt"; echo "{\"$2\":1}" > "$1/ConfigTemplate.json"; }
V=$d/home/.local/share/Steam/steamapps/common/FEX-Emu; N=$d/home/fex-eac-work/stage
mk "$V" FEX-valve; mk "$N" FEX-patched
run() { HOME=$d/home sh "$S" "$@" > "$d/out" 2>&1; }
ok() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; sed 's/^/     /' "$d/out"; fail=1; fi; }
is() { grep -q "^$2 FEX$" "$V/usr/bin/FEX" && grep -q "$2" "$V/VERSIONS.txt"; }
run restore; r=$?; [ $r != 0 ] && is x FEX-valve; ok $? "restore without a backup fails and leaves the tool alone"
run; [ $? = 0 ] && is x FEX-patched; ok $? "install swaps the stage in"
[ -f "$d/home/fex-eac-work/valve-backup/FEX-valve/usr/bin/FEX" ]; ok $? "install backs up Valve's build"
run; [ $? = 0 ] && grep -q "already installed" "$d/out"; ok $? "second install is a no-op"
run restore; [ $? = 0 ] && is x FEX-valve; ok $? "restore puts Valve's build back"
rm "$N/usr/bin/FEXpidof"; run; r=$?; [ $r != 0 ] && is x FEX-valve; ok $? "incomplete stage is refused, tool untouched"
rm -rf "$d"; [ $fail = 0 ] && echo "install-fex-tool: all PASS" || { echo "install-fex-tool: FAILED"; exit 1; }
