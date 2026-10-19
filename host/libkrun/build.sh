#!/bin/sh
# Build libkrun v1.19.6 + steamac patches for the macOS launcher.
#
#   host/libkrun/build.sh        build/refresh work/out/host/{lib,include}
#   host/libkrun/build.sh clean  drop the source/build tree (next build is from scratch)
#
# Inputs (all pinned):
#   libkrun        tag v1.19.6 (227b2de6), github.com/libkrun/libkrun
#   patches/       applied in order (git format-patch series, see each header)
#   virglrenderer  host/virglrenderer/build.sh output in work/out/host (built first if
#                  missing); Venus over the steamac MoltenVK (host/moltenvk)
#   Rust           $RUST_TOOLCHAIN via rustup (deps need >= 1.87)
#   libepoxy       host/libepoxy/build.sh output (built first if missing)
#   Homebrew       dtc, xz, lld (init cross-link), pkgconf
#
# Features: make GPU=1 BLK=1 NET=1 INPUT=1 SND=1. v1.19.6 has no TIMESYNC make flag (the
# vsock timesync is always built). SND on macOS uses the CoreAudio virtio-snd backend from
# patch 0014 (no PipeWire); its debug knob STEAMAC_SND_DUMP=/path.wav records the playback
# guest PCM before speaker mapping (patch 0017 detects and routes output speakers).
#
# Output (work/out/host):
#   lib/libkrun.1.dylib      install_name @rpath/libkrun.1.dylib, ad-hoc signed
#   lib/libkrun.dylib        -> libkrun.1.dylib
#   lib/pkgconfig/libkrun.pc
#   include/libkrun.h, include/libkrun_display.h, include/libkrun_input.h
# Binaries using it need an rpath to work/out/host/lib (it also loads
# @rpath/libvirglrenderer.1.dylib and @rpath/libMoltenVK.dylib from there) and the
# com.apple.security.hypervisor entitlement. The virtio-gpu unit tests run after the build,
# the smoke test in test/ is built, signed and run at the end. test/resize-test.sh is a
# separate live check of krun_display_resize on a clone of the guest disk.
# Installed files are written next to their destination and renamed over it (new inode),
# never rewritten in place: a running VM may have the dylib mapped.
set -eu

REPO=https://github.com/libkrun/libkrun.git
TAG=v1.19.6
COMMIT=227b2de6ed323fe180e02f871c5f325a90c13cc2
RUST_TOOLCHAIN=${RUST_TOOLCHAIN:-1.90.0}
BREW_DEPS="dtc xz lld pkgconf"
export MACOSX_DEPLOYMENT_TARGET=15.0 # rustc and cc build scripts inherit this target
MAKE_FLAGS="GPU=1 BLK=1 NET=1 INPUT=1 SND=1"

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=$root/work/build/host-libkrun
src=$work/src
out=$root/work/out/host

if [ "${1:-}" = clean ]; then
	rm -rf "$work"
	exit 0
fi

for dep in $BREW_DEPS; do
	brew list --versions "$dep" > /dev/null 2>&1 || brew install "$dep"
done
rustup toolchain list | grep -q "^$RUST_TOOLCHAIN-" ||
	rustup toolchain install "$RUST_TOOLCHAIN" --profile minimal

if [ ! -f "$out/lib/libepoxy.0.dylib" ] || [ ! -f "$out/lib/pkgconfig/epoxy.pc" ]; then
	"$root/host/libepoxy/build.sh"
fi

if [ ! -f "$out/lib/pkgconfig/virglrenderer.pc" ]; then
	"$root/host/virglrenderer/build.sh"
fi

mkdir -p "$work" "$out/lib/pkgconfig" "$out/include"

# --- source at the pinned tag + patches (target/ and the auto-fetched Debian sysroot
#     for the init binary survive re-runs)
if [ ! -d "$src/.git" ]; then
	git init -q "$src"
	git -C "$src" remote add origin "$REPO"
fi
if ! git -C "$src" cat-file -e "$COMMIT^{commit}" 2> /dev/null; then
	git -C "$src" fetch -q --depth 1 origin "refs/tags/$TAG:refs/tags/$TAG"
fi
test "$(git -C "$src" rev-parse "$TAG^{commit}")" = "$COMMIT"
git -C "$src" checkout -q -f --detach "$COMMIT"
git -C "$src" clean -q -fdx -e /target -e /linux-sysroot
for p in "$here"/patches/*.patch; do
	echo ">> applying $(basename "$p")"
	git -C "$src" apply "$p"
done

# --- build
(
	cd "$src"
	export RUSTUP_TOOLCHAIN="$RUST_TOOLCHAIN"
	export PKG_CONFIG_PATH="$out/lib/pkgconfig"
	# Do not fall back to Homebrew bottles; Cargo also tracks this pkg-config environment.
	export PKG_CONFIG_LIBDIR="$out/lib/pkgconfig"
	# No rustc strip: its llvm-objcopy debuginfo strip leaves LC_SYMTAB.stroff 4-byte aligned,
	# which ld and dyld reject for images built against the macOS 27 SDK ("mis-aligned LINKEDIT
	# string pool"; rust-lang/rust#157750, fixed in LLVM by llvm/llvm-project#203680).
	export CARGO_PROFILE_RELEASE_STRIP=false
	# shellcheck disable=SC2086
	make $MAKE_FLAGS
	make PREFIX="$out" libkrun.pc
	# Unit tests of the patched virtio-gpu code (EDID/display resize, blob scanouts).
	cd src/devices
	RUSTFLAGS="-L native=$out/lib -C link-args=-Wl,-rpath,$out/lib" \
		cargo test -q --features gpu --lib -- virtio::gpu
	# Speaker detection/config consistency, PCM permutations, height routing and downmix.
	RUSTFLAGS="-L native=$out/lib -C link-args=-Wl,-rpath,$out/lib" \
		cargo test -q --features snd --test macos_audio
	# CoreAudio backend: control requests and the pump stalling each other (patch 0022).
	RUSTFLAGS="-L native=$out/lib -C link-args=-Wl,-rpath,$out/lib" \
		cargo test -q --features gpu,snd --lib -- virtio::snd
	# The wmtr context frees an arena blob the guest released (patch 0028).
	(cd ../rutabaga_gfx &&
		RUSTFLAGS="-L native=$out/lib -C link-args=-Wl,-rpath,$out/lib" \
			cargo test -q --features gpu,virgl_renderer --lib -- wmtr)
	# A discard/write-zeroes at the end of a raw image keeps the disk size (patch 0027).
	cargo test -q --features blk --lib -- virtio::block
	# Freed guest memory leaves the VMM's footprint (patch 0026). Runs a guest vCPU, so the
	# test binary needs the hypervisor entitlement: built, signed, then run.
	cd "$src"
	bin=$(cargo test -q -p krun-hvf --lib --no-run --message-format=json |
		sed -n 's/.*"executable":"\([^"]*\)".*/\1/p' | tail -1)
	codesign --force -s - --entitlements "$here/test/hypervisor.entitlements" "$bin"
	# A Mac without Hypervisor.framework support (GitHub's macOS runners are VMs without nested
	# virtualization) cannot run it; there it is built and signed only.
	if [ "$(sysctl -n kern.hv_support 2>/dev/null)" = 1 ]; then
		"$bin" -q
	else
		echo "libkrun: kern.hv_support is not 1, krun-hvf tests (patch 0026) built but not run" >&2
	fi
)

# --- install (temp file + rename for every output)
lib=$out/lib/libkrun.1.dylib
cp "$src/target/release/libkrun.1.19.6.dylib" "$lib.tmp"
install_name_tool -id @rpath/libkrun.1.dylib "$lib.tmp"
codesign --force -s - "$lib.tmp"
mv -f "$lib.tmp" "$lib"
ln -sfh libkrun.1.dylib "$out/lib/libkrun.dylib.tmp"
mv -f "$out/lib/libkrun.dylib.tmp" "$out/lib/libkrun.dylib"
cp "$src/libkrun.pc" "$out/lib/pkgconfig/libkrun.pc.tmp"
mv -f "$out/lib/pkgconfig/libkrun.pc.tmp" "$out/lib/pkgconfig/libkrun.pc"
for h in libkrun.h libkrun_display.h libkrun_input.h; do
	cp "$src/include/$h" "$out/include/$h.tmp"
	mv -f "$out/include/$h.tmp" "$out/include/$h"
done

echo ">> $lib"
otool -L "$lib"

"$here/test/run.sh"
