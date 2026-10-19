#!/bin/sh
# Build, sign (com.apple.security.hypervisor) and run the libkrun configuration smoke
# test against work/out/host.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=$root/work/out/host
bin=$root/work/build/host-libkrun/smoke

mkdir -p "$(dirname "$bin")"
clang -std=c11 -Wall -Wextra -Werror -o "$bin" "$here/smoke.c" \
	-I"$out/include" -I"$root/host/vrhost/include" -L"$out/lib" -lkrun -Wl,-rpath,"$out/lib"
codesign --force -s - --entitlements "$here/hypervisor.entitlements" "$bin"
"$bin"
