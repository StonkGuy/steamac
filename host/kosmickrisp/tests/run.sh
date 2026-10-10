#!/bin/sh
# usage: run.sh <libvulkan_kosmickrisp.dylib> [test...]   (default: alloc-heap sparse-off + sparse-on)
# Builds each test against the Khronos loader (brew vulkan-loader/vulkan-headers) and runs it on that ICD.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
lib=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
out=${TMPDIR:-/tmp}/kk-tests.$$; mkdir -p "$out"
loader=$(brew --prefix vulkan-loader)/lib; hdr=$(brew --prefix vulkan-headers)/include
printf '{"file_format_version": "1.0.1", "ICD": {"library_path": "%s", "api_version": "1.4.0"}}\n' "$lib" > "$out/icd.json"
for t in alloc-heap async-stress indirect-io; do
   [ -f "$here/$t.c" ] || continue
   clang -std=gnu11 -Wall -Werror -O1 -I"$hdr" "$here/$t.c" -L"$loader" -lvulkan -Wl,-rpath,"$loader" -o "$out/$t"
done
fail=0
run() { printf '%-28s ' "$*"; VK_DRIVER_FILES=$out/icd.json "$out/$1" ${2:+"$2"} 2>&1 | grep -E 'RESULT|each' | tr '\n' ' ' ; echo; }
run alloc-heap sparse-off; run alloc-heap sparse-on
[ -x "$out/async-stress" ] && run async-stress
[ -x "$out/indirect-io" ] && run indirect-io
# deterministic model of the 0041 cleanup race (the fixed variant passes, the old one faults)
cc -O1 -pthread "$here/async-cleanup-model.c" -o "$out/m_new"; cc -O1 -pthread -DOLD_CLEANUP "$here/async-cleanup-model.c" -o "$out/m_old"
printf "%-28s " "async-cleanup-model"; "$out/m_new" | tr "\n" " "; "$out/m_old" >/dev/null 2>&1 && echo "(old variant did not fault: FAIL)" || echo "(old variant faults as expected)"
rm -rf "$out"
