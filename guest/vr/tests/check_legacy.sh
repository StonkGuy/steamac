#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Layout check of the older interface versions (runtime/legacy.gen.h, third_party/openvr/legacy_interfaces.txt):
#   check_legacy.sh OUT              the generated old vtables, and the method lists against the vendored openvr.h
#   check_legacy.sh OUT OPENVR_GIT   also against headers/openvr.h of every release tag of a ValveSoftware/openvr
#                                    clone: vtable slot + argument count of every method, and the size of every
#                                    type an old method takes against the current type the adapter passes on
#                                    (differences must be listed in tests/data/legacy_sizes.txt).
set -eu
out=$1/legacy-check
git=${2:-}
here=$(cd "$(dirname "$0")/.." && pwd)
spec=$here/third_party/openvr/legacy_interfaces.txt
gen="python3 $here/tools/gen_legacy.py"
CXX=${CXX:-g++}
mkdir -p "$out"

build_run() {  # name, include dir
  $CXX -std=c++17 -w -I"$2" -I"$here/runtime" -I"$here/include" -o "$out/$1" "$out/$1.cpp"
  "$out/$1" > "$out/$1.txt" || { grep MISMATCH "$out/$1.txt"; echo "FAIL $1"; exit 1; }
  tail -n 1 "$out/$1.txt" | sed "s/^/$1: /"
}

$gen check-adapters "$spec" "$here/third_party/openvr/openvr.h" > "$out/adapters.cpp"
build_run adapters "$here/third_party/openvr"
$gen check-header "$spec" "$here/third_party/openvr/openvr.h" > "$out/current.cpp"
build_run current "$here/third_party/openvr"
[ -n "$git" ] || exit 0

for tag in $(sed -n 's/^@[A-Za-z_0-9]* [A-Za-z]* //p' "$spec" | sort -u); do
  mkdir -p "$out/$tag"
  git -C "$git" show "$tag:headers/openvr.h" > "$out/$tag/openvr.h"
  $gen check-header "$spec" "$out/$tag/openvr.h" > "$out/hdr-$tag.cpp"
  build_run "hdr-$tag" "$out/$tag"
done
# sizes: old type vs the current type of the same name
cat "$out"/hdr-*.txt | grep '^SIZE' | sort -u > "$out/sizes-old.txt"
grep '^SIZE' "$out/adapters.txt" | sort -u > "$out/sizes-new.txt"
python3 - "$out/sizes-old.txt" "$out/sizes-new.txt" "$here/tests/data/legacy_sizes.txt" <<'PY'
import sys
def load(p):
    d = {}
    for l in open(p):
        _, ver, meth, arg, typ, size = l.split()
        d[(ver, meth, arg, typ)] = int(size)
    return d
old, new = load(sys.argv[1]), load(sys.argv[2])
known = {tuple(l.split()[:2]) for l in open(sys.argv[3]) if l.strip() and not l.startswith("#")}
bad = 0
seen = set()
for k in sorted(old):
    if k in new and old[k] != new[k]:
        seen.add((k[0], k[1]))
        if (k[0], k[1]) not in known:
            bad += 1
            print("SIZE MISMATCH %s::%s arg %s %s: %d then, %d now" % (k + (old[k], new[k])))
for k in sorted(known - seen):
    bad += 1
    print("stale entry in legacy_sizes.txt: %s %s" % k)
print("sizes: %d compared, %d methods with a changed type (listed), %d unlisted" % (len(old), len(seen), bad))
sys.exit(bad != 0)
PY
