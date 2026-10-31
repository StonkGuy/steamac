#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build steamac-vrhost (experimental VR host, docs/vr.md) for the app: Release, macOS 15 deployment target, static
# libcrypto, no tests. Output: work/out/host/bin/steamac-vrhost (ad hoc signed, identifier com.steamac.vrhost) and
# work/out/host/VRHOST.txt (provenance for licenses.sh / gpl-sources.sh). bundle.sh puts it into the app only when
# it exists, so builds without VR are unchanged. Needs CMake >= 3.25 and Ninja (brew install cmake ninja).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BUILD="${VRHOST_BUILD_DIR:-$ROOT/work/build/host-vrhost}"
OUT="$ROOT/work/out/host"

for tool in cmake ninja; do
    command -v "$tool" >/dev/null || { echo "vrhost/build.sh: $tool not found (brew install cmake ninja)" >&2; exit 1; }
done
cmake -S "$HERE" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DVRHOST_TESTS=OFF -DVRHOST_SYSTEM_OPENSSL=OFF
ninja -C "$BUILD" steamac-vrhost

exe="$BUILD/steamac-vrhost"
# Self-contained: only macOS system libraries (the same rule as the self_contained test).
if otool -L "$exe" | awk 'NR > 1 {print $1}' | grep -Ev '^(/usr/lib/|/System/)'; then
    echo "vrhost/build.sh: steamac-vrhost links non-system libraries (above)" >&2
    exit 1
fi
mkdir -p "$OUT/bin"
cp -f "$exe" "$OUT/bin/steamac-vrhost.new"
codesign --force --sign - --identifier com.steamac.vrhost "$OUT/bin/steamac-vrhost.new"
mv -f "$OUT/bin/steamac-vrhost.new" "$OUT/bin/steamac-vrhost"

openssl=$(sed -n 's/^[[:space:]]*set(VRHOST_OPENSSL_VERSION \(.*\))$/\1/p' "$HERE/CMakeLists.txt")
{
    echo "steamac-vrhost built from $(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "build dir: $BUILD"
    echo "openssl: $openssl"
    echo "protocol: $("$OUT/bin/steamac-vrhost" --protocol)"
} > "$OUT/VRHOST.txt"
echo "built $OUT/bin/steamac-vrhost"
