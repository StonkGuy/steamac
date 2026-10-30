#!/bin/sh
# SPDX-License-Identifier: MIT
# Checks fex-eac/build-fex.sh's source handling without building anything:
#   1. a tree left by an older build-fex.sh (upstream FEX-2610 + patch commits) is refused with the command to remove it;
#   2. with ONLINE=1, the pinned FEX_EAC_SHA is what tag FEX_EAC_TAG points at on StonkGuy/FEX.
# usage: scripts/test/build-fex-pin.sh
HERE=$(cd "$(dirname "$0")" && pwd); S="$HERE/../../fex-eac/build-fex.sh"; fail=0
d=$(mktemp -d); trap 'rm -rf "$d"' EXIT
git init -q "$d/FEX" && git -C "$d/FEX" -c user.name=t -c user.email=t@invalid commit -q --allow-empty -m "Docs: Update for release FEX-2610"
git -C "$d/FEX" -c user.name=build -c user.email=build@invalid commit -q --allow-empty -m "LinuxSyscalls: emulate x86-64 ptrace"
out=$(WORK="$d" sh "$S" 2>&1); rc=$?
if [ $rc -ne 0 ] && printf '%s' "$out" | grep -q "rm -rf '$d/FEX'"; then echo "ok   old tree refused"; else echo "FAIL old tree: rc=$rc: $out"; fail=1; fi
[ -d "$d/FEX/.git" ] && echo "ok   old tree left in place" || { echo "FAIL old tree was removed"; fail=1; }
if [ "${ONLINE:-0}" = 1 ]; then
  tag=$(sed -n 's/^FEX_EAC_TAG=//p' "$S"); sha=$(sed -n 's/^FEX_EAC_SHA=//p' "$S")
  # an annotated tag lists its commit as tag^{}; a lightweight tag only as the tag itself
  refs=$(git ls-remote https://github.com/StonkGuy/FEX.git "refs/tags/$tag" "refs/tags/$tag^{}")
  got=$(printf '%s\n' "$refs" | awk -v t="refs/tags/$tag^{}" '$2 == t {print $1}')
  [ -n "$got" ] || got=$(printf '%s\n' "$refs" | awk -v t="refs/tags/$tag" '$2 == t {print $1}')
  [ "$got" = "$sha" ] && echo "ok   $tag = $sha" || { echo "FAIL $tag is '$got', script pins $sha"; fail=1; }
fi
exit $fail
