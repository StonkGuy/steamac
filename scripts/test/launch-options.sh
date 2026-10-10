#!/bin/sh
# SPDX-License-Identifier: MIT
# Checks that the documented VRChat launch options expand $HOME in every path when a shell runs them.
# usage: scripts/test/launch-options.sh [FILE...]   default: fex-eac/README.md fex-eac/guest-tune.sh; each FILE holds one
#        "env EAC_LAUNCHERDIR=... %command%" line
[ $# -gt 0 ] || set -- fex-eac/README.md fex-eac/guest-tune.sh
fail=0
for f in "$@"; do
  l=$(grep -m1 '^ *env EAC_LAUNCHERDIR=.*%command%' "$f") || { echo "FAIL $f: no launch line"; fail=1; continue; }
  cmd=$(printf '%s' "$l" | sed 's/^ *//; s/%command%/env/')
  out=$(env -i HOME=/home/steamos PATH=/usr/bin:/bin sh -c "$cmd" | grep -E "^(EAC_LAUNCHERDIR|PROTON_EAC_RUNTIME)=")
  case "$out" in *'$HOME'*) echo "FAIL $f: unexpanded \$HOME: $(printf '%s' "$out" | tr '\n' '|')"; fail=1;; *) echo "ok   $f";; esac
done
exit $fail
