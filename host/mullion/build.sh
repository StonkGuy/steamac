#!/usr/bin/env bash
# Build the Mullion flavour of the DXMT graphics path from pinned sources: Mullion (a DXMT fork with Direct3D 12)
# behind the same wmtr transport, its host executor library and its guest dlls. Same steps and toolchains as
# host/dxmt/build.sh; nothing of the DXMT path's outputs is touched.
#
#   host/mullion/build.sh [step...]  steps: fetch llvm native host pe client payload (default: all, in order)
#   host/mullion/build.sh clean      drop work/build/mullion* and the unpacked source (keeps work/cache downloads
#                                    and the shared LLVM 15 under work/out/host/llvm15)
#
#   fetch    Mullion at MULLION_COMMIT (host/mullion/pin.env) + host/mullion/patches + its submodules
#            (mingw-directx-headers, nvapi) -> work/src/mullion
#   llvm     the LLVM 15 install DXMT's airconv uses (host/dxmt/build.sh llvm; same release as Mullion's CI)
#   native   Mullion's native meson build (airconv) -> work/build/mullion-native
#   host     wmtr with MULLION=1: libwmtr_host.dylib (executor + Mullion's winemetal_unix.c + airconv) and its
#            tests (make test), installed into work/out/mullion-host/lib. Mullion's unix side uses Metal 4, and
#            wmtr_host.m checks the Metal 4 mapping-operation sizes the guest uses: needs the macOS 26 SDK (Xcode 26).
#   pe       Mullion's Windows dlls (d3d12, d3d12core, d3d11, dxgi, d3d10core, winemetal) cross-built with
#            llvm-mingw against the Wine 8.16 build Mullion's CI uses (Wine builtins), without its macOS unixlib
#            (-Dbuild_unixlib=false: the unix side is the host library) -> work/out/mullion-pe
#   client   the guest's winemetal.so unixlib (x86-64 ELF, Mullion's unix-call table) + make test-linux, in a
#            linux/amd64 container (Docker/OrbStack) -> wmtr/build/mullion/x86_64-linux
#   payload  make -C wmtr MULLION=1 dxmt-payload -> work/out/mullion
#
# Needs what host/dxmt/build.sh needs (Apple silicon macOS, Xcode with its Metal toolchain, Rosetta for the pinned
# winebuild, Homebrew meson ninja cmake pkgconf zstd, Docker for `client`), with Xcode 26 for `host`.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/../.." && pwd)
# shellcheck source=pin.env
. "$here/pin.env"

# Toolchains of Mullion's CI (.github/workflows/ci.yml: WINE_URL, LLVM_MINGW_VERSION): the ones DXMT's CI uses, so
# the downloads and the unpacked toolchains are shared with host/dxmt/build.sh
WINE_URL=https://github.com/3Shain/wine/releases/download/v8.16-3shain/wine.tar.gz
WINE_SHA256=289c7f19e270a3d3d0a6fdb07691b176c70a0795f6811e5255cba82425de4f10
LLVM_MINGW="llvm-mingw-20260908-ucrt-macos-universal"
LLVM_MINGW_URL=https://github.com/mstorsjo/llvm-mingw/releases/download/20260908/$LLVM_MINGW.tar.xz
LLVM_MINGW_SHA256=d1dc5d1ecf3a3ced5ed5544c72f1acd0c8e84eb3024d520ecc6b143eec62a149
# Guest unixlib build container: host/dxmt/build.sh's (debian:trixie, gcc 14)
CLIENT_IMAGE=docker.io/library/debian@sha256:913f6706df59a68922d1dd08f78c2476560a8d367897200a6005b00e5f67c2d5
BREW_DEPS="meson ninja cmake pkgconf zstd"
# the launcher's floor (host/launcher/bundle.sh); Mullion asks at run time for what is newer (Metal 4: macOS 26)
export MACOSX_DEPLOYMENT_TARGET=15.0
# the guest dlls (all of them Wine builtins)
PE_DLLS="d3d12 d3d12core d3d11 dxgi d3d10core winemetal"

cache=$root/work/cache/mullion
tcache=$root/work/cache/dxmt # the shared toolchain downloads
src=$root/work/src/mullion
llvm=$root/work/out/host/llvm15
native=$root/work/build/mullion-native
pebuild=$root/work/build/mullion-pe
pe=$root/work/out/mullion-pe
toolchains=$root/work/cache/toolchains
jobs=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}

log() { printf '\n=== %s\n' "$*"; }
die() { echo "host/mullion/build.sh: $*" >&2; exit 1; }

fetch_file() { # url sha256 dest
    [ -f "$3" ] || { mkdir -p "$(dirname "$3")"; curl -fsSL --retry 3 -o "$3.part" "$1" && mv "$3.part" "$3"; }
    echo "$2  $3" | shasum -a 256 -c - >/dev/null || die "checksum mismatch: $3 (delete it to re-download)"
}

need_metal() {
    xcrun -sdk macosx -f metal >/dev/null 2>&1 || die "no Metal toolchain (xcodebuild -downloadComponent MetalToolchain)"
}

step_fetch() {
    log "fetch Mullion ${MULLION_COMMIT:0:12} + $(ls "$here"/patches/*.patch | wc -l | tr -d ' ') patches"
    fetch_file "$MULLION_URL" "$MULLION_SHA256" "$cache/mullion-$MULLION_COMMIT.tar.gz"
    fetch_file "$DXHEADERS_URL" "$DXHEADERS_SHA256" "$cache/mingw-directx-headers-$DXHEADERS_COMMIT.tar.gz"
    fetch_file "$NVAPI_URL" "$NVAPI_SHA256" "$cache/nvapi-$NVAPI_COMMIT.tar.gz"
    rm -rf "$src"; mkdir -p "$src"
    tar -xzf "$cache/mullion-$MULLION_COMMIT.tar.gz" -C "$src" --strip-components=1
    mkdir -p "$src/include/native/directx" "$src/external/nvapi"
    tar -xzf "$cache/mingw-directx-headers-$DXHEADERS_COMMIT.tar.gz" -C "$src/include/native/directx" --strip-components=1
    tar -xzf "$cache/nvapi-$NVAPI_COMMIT.tar.gz" -C "$src/external/nvapi" --strip-components=1
    # its own repository, so `git apply` does not resolve paths against this checkout
    git -C "$src" init -q
    for p in "$here"/patches/*.patch; do git -C "$src" apply "$p" || die "$(basename "$p") does not apply"; done
    # meson's vcs_tag runs `git describe`; give it the commit the patches reproduce
    git -C "$src" add -A && git -C "$src" -c user.name=build -c user.email=build@localhost \
        commit -q -m "Mullion ${MULLION_FORK_COMMIT:0:12} (pinned base + host/mullion/patches)"
}

step_llvm() {
    local dxmt_llvm
    dxmt_llvm=$(. "$root/host/dxmt/pin.env" && echo "$LLVM_VERSION")
    [ "$dxmt_llvm" = "$LLVM_VERSION" ] || die "LLVM $LLVM_VERSION pinned here, $dxmt_llvm in host/dxmt/pin.env: no shared install"
    "$root/host/dxmt/build.sh" llvm
}

step_native() {
    log "Mullion native (airconv)"
    [ -f "$src/meson.build" ] || die "no Mullion source: run the fetch step"
    [ -f "$llvm/.pin" ] || die "no LLVM 15: run the llvm step"
    need_metal
    meson setup --wipe "$native" "$src" --buildtype release -Dnative_llvm_path="$llvm" >/dev/null
    ninja -C "$native" src/airconv/darwin/libairconv.a libs/DXBCParser/libDXBCParserNative.a
}

step_host() {
    log "wmtr host library (Mullion flavour)"
    local mk=(MULLION=1 DXMT="$src" DXMT_BUILD="$native" LLVM="$llvm" PREFIX="$root/work/out/mullion-host")
    make -C "$root/wmtr" "${mk[@]}" host install
    make -C "$root/wmtr" "${mk[@]}" test
}

step_pe() {
    log "Mullion PE dlls (llvm-mingw, Wine builtins)"
    [ -f "$src/meson.build" ] || die "no Mullion source: run the fetch step"
    arch -x86_64 /usr/bin/true 2>/dev/null || die "Rosetta is needed for the pinned winebuild: softwareupdate --install-rosetta"
    need_metal
    fetch_file "$WINE_URL" "$WINE_SHA256" "$tcache/wine-8.16-3shain.tar.gz"
    fetch_file "$LLVM_MINGW_URL" "$LLVM_MINGW_SHA256" "$tcache/$LLVM_MINGW.tar.xz"
    if [ "$(cat "$toolchains/.pin" 2>/dev/null)" != "$WINE_SHA256 $LLVM_MINGW_SHA256" ]; then
        rm -rf "$toolchains"; mkdir -p "$toolchains/wine"
        tar -xzf "$tcache/wine-8.16-3shain.tar.gz" -C "$toolchains/wine"
        tar -xJf "$tcache/$LLVM_MINGW.tar.xz" -C "$toolchains"
        echo "$WINE_SHA256 $LLVM_MINGW_SHA256" > "$toolchains/.pin"
    fi
    PATH="$toolchains/$LLVM_MINGW/bin:$PATH" meson setup --wipe "$pebuild" "$src" --cross-file "$src/build-win64.txt" \
        --buildtype release --strip --prefix "$pebuild/install" -Dwine_install_path="$toolchains/wine" \
        -Dbuild_unixlib=false >/dev/null
    PATH="$toolchains/$LLVM_MINGW/bin:$PATH" meson compile -C "$pebuild" -j "$jobs"
    PATH="$toolchains/$LLVM_MINGW/bin:$PATH" meson install -C "$pebuild" >/dev/null
    rm -rf "$pe"; mkdir -p "$pe"
    for f in $PE_DLLS; do
        dll=$pebuild/install/x86_64-windows/$f.dll
        # winebuild --builtin stamps "Wine builtin DLL" into the DOS stub; Wine treats it as a builtin only then
        grep -q 'Wine builtin DLL' "$dll" || die "$f.dll is not marked as a Wine builtin"
        install -m 644 "$dll" "$pe/$f.dll"
    done
}

step_client() {
    log "guest winemetal.so, Mullion flavour (linux/amd64 container)"
    command -v docker >/dev/null || die "the client step needs Docker (OrbStack)"
    docker run --rm --platform linux/amd64 -e DEBIAN_FRONTEND=noninteractive -e OWNER="$(id -u):$(id -g)" \
        -v "$root/wmtr:/wmtr" -v "$src:/mullion:ro" "$CLIENT_IMAGE" sh -euc '
        apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc libc6-dev make binutils >/dev/null
        make -C /wmtr CC=gcc MULLION=1 DXMT=/mullion client-linux test-linux; chown -R "$OWNER" /wmtr/build'
}

step_payload() {
    log "payload -> work/out/mullion"
    make -C "$root/wmtr" MULLION=1 DXMT_PE="$pe" PAYLOAD="$root/work/out/mullion" dxmt-payload
    (cd "$root/work/out/mullion" && find . -type f | sort | xargs shasum -a 256)
}

if [ "${1:-}" = clean ]; then
    rm -rf "$src" "$native" "$pebuild" "$root/wmtr/build/mullion"
    exit 0
fi
steps=("$@")
[ ${#steps[@]} -gt 0 ] || steps=(fetch llvm native host pe client payload)
# fetch, client and payload also run on a Linux host
case " ${steps[*]} " in *" llvm "*|*" native "*|*" host "*|*" pe "*)
    for dep in $BREW_DEPS; do
        brew list --versions "$dep" >/dev/null 2>&1 || brew install "$dep"
    done ;;
esac
for s in "${steps[@]}"; do
    case $s in
        fetch|llvm|native|host|pe|client|payload) "step_$s" ;;
        *) die "unknown step: $s (fetch llvm native host pe client payload | clean)" ;;
    esac
done
log "done"
