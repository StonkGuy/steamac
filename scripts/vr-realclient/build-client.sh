#!/usr/bin/env bash
# Build the non-Android WiVRn v26.9 client on the Linux test host.
# Usage: build-client.sh [WiVRn-source-dir] [build-dir]
set -euo pipefail

AUDIT_DIR=${VR_REALCLIENT_LOG_DIR:-"$HOME/vr-realclient/logs"}
SOURCE_DIR=${1:-"$HOME/vr-realclient/WiVRn"}
BUILD_DIR=${2:-"$HOME/vr-realclient/build-client"}
mkdir -p "$AUDIT_DIR" "$(dirname -- "$BUILD_DIR")"
LOG="$AUDIT_DIR/client-build-$(date -u +%Y%m%dT%H%M%SZ).log"

if [[ ! -f "$SOURCE_DIR/CMakeLists.txt" || ! -d "$SOURCE_DIR/.git" ]]; then
    printf 'WiVRn source checkout not found: %s\n' "$SOURCE_DIR" >&2
    printf 'Clone the v26.9 tag there, or pass the source directory as argument 1.\n' >&2
    exit 2
fi
if ! command -v cmake >/dev/null || ! command -v ninja >/dev/null; then
    printf 'cmake and ninja are required on PATH.\n' >&2
    exit 2
fi

TAG=$(git -C "$SOURCE_DIR" describe --tags --exact-match HEAD 2>/dev/null || true)
if [[ "$TAG" != v26.9 ]]; then
    printf 'Expected WiVRn tag v26.9 at HEAD; found %s. Refusing to build a different protocol.\n' "${TAG:-untagged}" >&2
    exit 2
fi

{
    printf 'WiVRn source: %s\n' "$SOURCE_DIR"
    printf 'WiVRn revision: %s (%s)\n' "$(git -C "$SOURCE_DIR" rev-parse HEAD)" "$TAG"
    printf 'Build directory: %s\n' "$BUILD_DIR"
    printf 'Configure: WIVRN_BUILD_CLIENT=ON, WIVRN_BUILD_SERVER=OFF\n'
    cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DWIVRN_BUILD_CLIENT=ON \
        -DWIVRN_BUILD_SERVER=OFF \
        -DGIT_TAG=v26.9
    cmake --build "$BUILD_DIR" --target wivrn --parallel "${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf 4)}"
    printf '\nBuild complete. Candidate executable(s):\n'
    find "$BUILD_DIR/bin" -maxdepth 1 -type f -executable -print 2>/dev/null || true
} 2>&1 | tee "$LOG"
