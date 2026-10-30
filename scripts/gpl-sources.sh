#!/usr/bin/env bash
# Package corresponding GPL/LGPL source for the kernel, static initramfs tools and
# DXMT graphics path (host executor and the guest layer's PE DLLs and winemetal.so), the experimental
# Mullion path's (the same parts, when this build has them), and the GPL-3.0
# steamac-vrhost (host/vrhost + its pinned OpenSSL tarball) when the app bundles it.
# Usage: scripts/gpl-sources.sh [GIT_REF] (HEAD uses the current working tree).
# The tagged files are read with git archive, never checked out over the working tree.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
REF=${1:-HEAD}
[[ $# -le 1 ]] || { echo 'usage: scripts/gpl-sources.sh [GIT_REF]' >&2; exit 2; }
mkdir -p "$ROOT/work/scratch" "$ROOT/work/out/dist"
ST=$(mktemp -d "$ROOT/work/scratch/gpl-sources.XXXXXXXX")
trap 'rm -rf "$ST"' EXIT
mkdir -p "$ST/repo" "$ST/sources"

if [[ $REF == HEAD ]]; then
    # Include local, not-yet-committed changes in the sources for a local build.
    cp "$ROOT/host/launcher/Info.plist" "$ST/Info.plist"
    for path in scripts/config.env scripts/steps/20-initramfs.sh scripts/builder/Dockerfile \
                scripts/build-image.sh guest/initramfs/init; do
        mkdir -p "$ST/repo/$(dirname "$path")"
        cp "$ROOT/$path" "$ST/repo/$path"
    done
    cp -R "$ROOT/guest/kernel" "$ST/repo/guest/"
    if [[ -f $ROOT/host/dxmt/pin.env ]]; then
        # Include new source files in local builds, but not ignored wmtr/build output.
        git -C "$ROOT" ls-files -z --cached --others --exclude-standard -- \
            host/dxmt wmtr host/vrhost/include/vrlink.h guest/vr/include/steamac_vr_guest.h \
            scripts/steps/30-layer.sh \
            LICENSE NOTICE LICENSES | tar -C "$ROOT" --null -T - -cf - | tar -xf - -C "$ST/repo"
    fi
    if [[ -f $ROOT/work/out/host/bin/steamac-vrhost ]]; then
        git -C "$ROOT" ls-files -z -- host/vrhost | tar -C "$ROOT" --null -T - -cf - | tar -xf - -C "$ST/repo"
    fi
    if [[ -f $ROOT/host/mullion/pin.env ]]; then
        git -C "$ROOT" ls-files -z --cached --others --exclude-standard -- host/mullion \
            | tar -C "$ROOT" --null -T - -cf - | tar -xf - -C "$ST/repo"
    fi
else
    git -C "$ROOT" rev-parse --verify "$REF^{commit}" >/dev/null
    git -C "$ROOT" archive "$REF" host/launcher/Info.plist scripts/config.env \
        scripts/steps/20-initramfs.sh scripts/builder/Dockerfile scripts/build-image.sh \
        guest/initramfs/init guest/kernel | tar -xf - -C "$ST/repo"
    cp "$ST/repo/host/launcher/Info.plist" "$ST/Info.plist"
    rm -rf "$ST/repo/host"
    if git -C "$ROOT" cat-file -e "$REF:host/dxmt/pin.env" 2>/dev/null; then
        git -C "$ROOT" archive "$REF" host/dxmt wmtr \
            host/vrhost/include/vrlink.h guest/vr/include/steamac_vr_guest.h \
            scripts/steps/30-layer.sh \
            LICENSE NOTICE LICENSES | tar -xf - -C "$ST/repo"
    fi
    # The VR host ships only in apps built with host/vrhost/build.sh; include its source whenever the tag has it.
    if git -C "$ROOT" cat-file -e "$REF:host/vrhost/CMakeLists.txt" 2>/dev/null; then
        git -C "$ROOT" archive "$REF" host/vrhost | tar -xf - -C "$ST/repo"
    fi
    if git -C "$ROOT" cat-file -e "$REF:host/mullion/pin.env" 2>/dev/null; then
        git -C "$ROOT" archive "$REF" host/mullion | tar -xf - -C "$ST/repo"
    fi
fi
version=$(plutil -extract CFBundleShortVersionString raw -o - "$ST/Info.plist")
[[ $version =~ ^[0-9]+(\.[0-9]+)*$ ]] || { echo "invalid app version: $version" >&2; exit 1; }
. "$ST/repo/scripts/config.env"
# Kernel pins are recorded by the build script itself.
KVER=$(awk -F= '$1 == "KVER" { print $2; exit }' "$ST/repo/guest/kernel/build.sh")
KSHA256=$(awk -F= '$1 == "KSHA256" { print $2; exit }' "$ST/repo/guest/kernel/build.sh")
[[ $KVER =~ ^[0-9]+(\.[0-9]+)*$ && $KSHA256 =~ ^[[:xdigit:]]{64}$ ]] || { echo 'missing kernel version or checksum' >&2; exit 1; }

fetch() { # URL, expected sha256 (or empty if verified via a downloaded .dsc), destination
    local url=$1 sha=$2 file=$3
    if [[ ! -f $file ]]; then
        echo "downloading $url"
        curl -fL --retry 5 -o "$file.part" "$url"
        mv "$file.part" "$file"
    fi
    if [[ -n $sha ]]; then
        echo "$sha  $file" | shasum -a 256 -c -
    fi
}
copy_source() { # Cached build tarball, URL, SHA256, archive filename
    local cached=$1 url=$2 sha=$3 name=$4
    if [[ -f $cached ]]; then
        cp "$cached" "$ST/sources/$name"
        echo "$sha  $ST/sources/$name" | shasum -a 256 -c -
    else
        fetch "$url" "$sha" "$ST/sources/$name"
    fi
}
copy_source "$ROOT/work/cache/kernel/linux-$KVER.tar.xz" \
    "https://cdn.kernel.org/pub/linux/kernel/v${KVER%%.*}.x/linux-$KVER.tar.xz" \
    "$KSHA256" "linux-$KVER.tar.xz"
copy_source "$ROOT/work/cache/dosfstools/$DOSFSTOOLS_VERSION/dosfstools-$DOSFSTOOLS_VERSION.tar.gz" \
    "$DOSFSTOOLS_URL" "$DOSFSTOOLS_SHA256" "dosfstools-$DOSFSTOOLS_VERSION.tar.gz"
copy_source "$ROOT/work/cache/e2fsprogs/$E2FSPROGS_VERSION/e2fsprogs-$E2FSPROGS_VERSION.tar.xz" \
    "$E2FSPROGS_URL" "$E2FSPROGS_SHA256" "e2fsprogs-$E2FSPROGS_VERSION.tar.xz"
copy_source "$ROOT/work/cache/btrfs-progs/$BTRFSPROGS_VERSION/btrfs-progs-v$BTRFSPROGS_VERSION.tar.xz" \
    "$BTRFSPROGS_URL" "$BTRFSPROGS_SHA256" "btrfs-progs-v$BTRFSPROGS_VERSION.tar.xz"

# Include DXMT even in a layer-only build: the Windows DLLs and unixlib ship there
# independently of whether this machine has built libwmtr_host.dylib.
dxmt_notes=
if [[ -f $ST/repo/host/dxmt/pin.env ]]; then
    . "$ST/repo/host/dxmt/pin.env"
    [[ $DXMT_COMMIT =~ ^[[:xdigit:]]{40}$ && $DXMT_SHA256 =~ ^[[:xdigit:]]{64}$ && $LLVM_VERSION =~ ^[0-9]+(\.[0-9]+)*$ ]] || {
        echo 'invalid host/dxmt/pin.env' >&2; exit 1;
    }
    copy_source "$ROOT/work/cache/dxmt/dxmt-$DXMT_COMMIT.tar.gz" "$DXMT_URL" "$DXMT_SHA256" "dxmt-$DXMT_COMMIT.tar.gz"
    # The pinned DirectX headers are a source dependency of the shipped PE DLLs.
    headers_record=$(awk -F= '/^DXHEADERS_(COMMIT|SHA256)=/ { print $1, $2 }' "$ST/repo/host/dxmt/build.sh")
    headers_commit=$(printf '%s\n' "$headers_record" | awk '$1 == "DXHEADERS_COMMIT" { print $2 }')
    headers_sha=$(printf '%s\n' "$headers_record" | awk '$1 == "DXHEADERS_SHA256" { print $2 }')
    [[ $headers_commit =~ ^[[:xdigit:]]{40}$ && $headers_sha =~ ^[[:xdigit:]]{64}$ ]] || {
        echo 'missing pinned DXMT DirectX headers' >&2; exit 1;
    }
    copy_source "$ROOT/work/cache/dxmt/mingw-directx-headers-$headers_commit.tar.gz" \
        "https://github.com/misyltoad/mingw-directx-headers/archive/$headers_commit.tar.gz" \
        "$headers_sha" "mingw-directx-headers-$headers_commit.tar.gz"
    dxmt_notes="
DXMT graphics path (LGPL-2.1-or-later): libwmtr_host.dylib and the guest layer's
/usr/lib/steamac/dxmt/x86_64-windows/{d3d11,dxgi,d3d10core,winemetal}.dll and
x86_64-unix/winemetal.so. Complete DXMT source:
sources/dxmt-$DXMT_COMMIT.tar.gz ($DXMT_REPO at that commit, SHA-256
$DXMT_SHA256). Apply repo/host/dxmt/patches/*.patch in filename order with
git apply. Extract sources/mingw-directx-headers-$headers_commit.tar.gz into
DXMT's include/native/directx with --strip-components=1.

repo/wmtr/ contains the BSD-3-Clause remoting implementation. Its host executor
links DXMT's src/winemetal/unix/winemetal_unix.c and src/airconv (with DXBCParser);
its guest winemetal.so uses DXMT's LGPL headers. The VR interface headers used by
the executor are in repo/host/vrhost/include/ and repo/guest/vr/include/.
Build with repo/host/dxmt/build.sh: fetch llvm native host pe client payload.
The fetch step performs the extraction and patching described above; pe builds
the four Windows DLLs, client builds winemetal.so in Docker, and payload stages
both in work/out/dxmt for scripts/steps/30-layer.sh. The script pins the Wine
and llvm-mingw build tools and every downloaded source with SHA-256.

libkrun loads libwmtr_host.dylib at run time; a rebuilt library of the same
name can replace it. To replace the guest DLLs/unixlib, rebuild the payload,
then rebuild the layer with scripts/build-image.sh layer. These layer build
scripts and the rest of steamac are available at https://github.com/StonkGuy/steamac
at $REF. The shader compiler statically links LLVM $LLVM_VERSION (Apache-2.0
WITH LLVM-exception), built from github.com/llvm/llvm-project's
llvmorg-$LLVM_VERSION release; its license is in repo/LICENSES/ and the app's
licenses folder."
fi

# Mullion (LGPL-2.1-or-later, a DXMT fork): the experimental Mullion path's executor
# (Frameworks/libwmtr_host_mullion.dylib) and guest payload, present when this build made them.
mullion_notes=
if [[ -f $ST/repo/host/mullion/pin.env ]] && [[ -f $ROOT/work/out/mullion-host/lib/libwmtr_host.dylib \
        || -f $ROOT/work/out/mullion/x86_64-unix/winemetal.so ]]; then
    . "$ST/repo/host/mullion/pin.env"
    for v in MULLION_COMMIT DXHEADERS_COMMIT NVAPI_COMMIT; do
        [[ ${!v} =~ ^[[:xdigit:]]{40}$ ]] || { echo "invalid host/mullion/pin.env ($v)" >&2; exit 1; }
    done
    for v in MULLION_SHA256 DXHEADERS_SHA256 NVAPI_SHA256; do
        [[ ${!v} =~ ^[[:xdigit:]]{64}$ ]] || { echo "invalid host/mullion/pin.env ($v)" >&2; exit 1; }
    done
    copy_source "$ROOT/work/cache/mullion/mullion-$MULLION_COMMIT.tar.gz" "$MULLION_URL" "$MULLION_SHA256" \
        "mullion-$MULLION_COMMIT.tar.gz"
    copy_source "$ROOT/work/cache/mullion/mingw-directx-headers-$DXHEADERS_COMMIT.tar.gz" "$DXHEADERS_URL" \
        "$DXHEADERS_SHA256" "mingw-directx-headers-$DXHEADERS_COMMIT.tar.gz"
    copy_source "$ROOT/work/cache/mullion/nvapi-$NVAPI_COMMIT.tar.gz" "$NVAPI_URL" "$NVAPI_SHA256" \
        "nvapi-$NVAPI_COMMIT.tar.gz"
    mullion_notes="
Mullion path (experimental; LGPL-2.1-or-later): Contents/Frameworks/
libwmtr_host_mullion.dylib and the guest layer's /usr/lib/steamac/mullion/
x86_64-windows/{d3d11,dxgi,d3d10core,d3d12,d3d12core,winemetal}.dll and
x86_64-unix/winemetal.so. Complete Mullion source:
sources/mullion-$MULLION_COMMIT.tar.gz ($MULLION_REPO at that commit,
SHA-256 $MULLION_SHA256). Apply repo/host/mullion/patches/*.patch in filename
order with git apply. Extract sources/mingw-directx-headers-$DXHEADERS_COMMIT.tar.gz
into Mullion's include/native/directx and sources/nvapi-$NVAPI_COMMIT.tar.gz
into external/nvapi, both with --strip-components=1 (MIT, NVIDIA's NVAPI
headers). Build with repo/host/mullion/build.sh: fetch llvm native host pe
client payload (the same toolchains and the same repo/wmtr/ as the DXMT path,
with MULLION=1); host builds wmtr's executor with Mullion's
src/winemetal/unix/winemetal_unix.c and airconv, installed as
work/out/mullion-host/lib/libwmtr_host.dylib, which the app bundles as
libwmtr_host_mullion.dylib (a rebuilt library of that name can replace it)."
fi

# steamac-vrhost (GPL-3.0-or-later: contains WiVRn code), present when the app bundles the VR host.
vrhost_notes=
if [[ -f $ST/repo/host/vrhost/CMakeLists.txt ]]; then
    ossl_version=$(sed -n 's/^[[:space:]]*set(VRHOST_OPENSSL_VERSION \(.*\))$/\1/p' "$ST/repo/host/vrhost/CMakeLists.txt")
    ossl_sha=$(sed -n 's/^[[:space:]]*set(VRHOST_OPENSSL_SHA256 \(.*\))$/\1/p' "$ST/repo/host/vrhost/CMakeLists.txt")
    [[ $ossl_version =~ ^[0-9]+(\.[0-9]+)*$ && $ossl_sha =~ ^[[:xdigit:]]{64}$ ]] || {
        echo 'missing OpenSSL pin in host/vrhost/CMakeLists.txt' >&2; exit 1;
    }
    copy_source "$ROOT/work/build/host-vrhost/openssl_static-prefix/src/openssl-$ossl_version.tar.gz" \
        "https://github.com/openssl/openssl/releases/download/openssl-$ossl_version/openssl-$ossl_version.tar.gz" \
        "$ossl_sha" "openssl-$ossl_version.tar.gz"
    vrhost_notes="
Contents/MacOS/steamac-vrhost (experimental VR host, a separate executable):
GPL-3.0-or-later, repo/host/vrhost/ (licence: repo/host/vrhost/COPYING). It
contains WiVRn v26.9 code, vendored unmodified in
repo/host/vrhost/third_party/wivrn/ (see UPSTREAM.md there). Build with
repo/host/vrhost/build.sh (CMake + Ninja). It links libcrypto from OpenSSL
$ossl_version (Apache-2.0) statically: sources/openssl-$ossl_version.tar.gz
(SHA-256 $ossl_sha), configured as in repo/host/vrhost/CMakeLists.txt.
Header-only build inputs fetched by CMake at pinned tags: OpenXR-SDK
release-1.1.58 and Boost.PFR boost-1.89.0."
fi

# Resolve busybox-static's *source* version from the same snapshot/architecture
# apt used in Dockerfile. The binary can have a different +bN rebuild suffix.
snapshot=https://snapshot.debian.org/archive/debian/$DEBIAN_SNAPSHOT
# Cache immutable snapshot indexes and tarballs shared by all tags at this snapshot.
snapshot_cache="$ROOT/work/cache/gpl-sources/$DEBIAN_SNAPSHOT"
mkdir -p "$snapshot_cache"
fetch "$snapshot/dists/trixie/main/binary-arm64/Packages.xz" '' "$snapshot_cache/Packages.xz"
source_version=$(xz -dc "$snapshot_cache/Packages.xz" | awk '
    /^Package: / { pkg=$2; version=""; source="" }
    pkg == "busybox-static" && /^Version: / { version=$2 }
    pkg == "busybox-static" && /^Source: / { source=$3; gsub(/[()]/, "", source) }
    /^$/ && pkg == "busybox-static" { if (source == "") source=version; result=version " " source }
    END { print result }
')
read -r binary_version busybox_version <<< "$source_version"
[[ -n ${binary_version:-} && -n ${busybox_version:-} && $busybox_version =~ ^[0-9]+:[0-9A-Za-z.+~-]+$ ]] || {
    echo 'cannot resolve busybox-static source in snapshot Packages.xz' >&2; exit 1;
}
fetch "$snapshot/dists/trixie/main/source/Sources.xz" '' "$snapshot_cache/Sources.xz"
# The Sources index supplies the sha256 of the .dsc; that signed Debian
# descriptor supplies the sha256s for both upstream and Debian patch tarballs.
dsc_record=$(xz -dc "$snapshot_cache/Sources.xz" | awk -v version="$busybox_version" '
    /^Package: / { pkg=$2; matched=0; sha=""; name=""; dir="" }
    pkg == "busybox" && /^Version: / { matched=($2 == version) }
    pkg == "busybox" && /^Directory: / { dir=$2 }
    pkg == "busybox" && /^Checksums-Sha256:/ { checksums=1; next }
    checksums && /^ / && $3 ~ /\.dsc$/ { sha=$1; name=$3 }
    checksums && !/^ / { checksums=0 }
    /^$/ { if (pkg == "busybox" && matched && sha != "" && dir != "") result=sha " " dir " " name }
    END { print result }
')
read -r dsc_sha source_dir dsc_name <<< "$dsc_record"
[[ $dsc_sha =~ ^[[:xdigit:]]{64}$ && $source_dir == pool/main/b/busybox && $dsc_name =~ ^busybox_[0-9A-Za-z.+~-]+\.dsc$ ]] || {
    echo "cannot resolve busybox $busybox_version .dsc in snapshot Sources.xz" >&2; exit 1;
}
fetch "$snapshot/$source_dir/$dsc_name" "$dsc_sha" "$snapshot_cache/$dsc_name"
cp "$snapshot_cache/$dsc_name" "$ST/sources/$dsc_name"
source_checksums=$(awk '
    /^Checksums-Sha256:/ { checksums=1; next }
    checksums && /^ / { print $1, $3; next }
    checksums && !/^ / { exit }
' "$ST/sources/$dsc_name")
count=0
while read -r sha name; do
    [[ $sha =~ ^[[:xdigit:]]{64}$ && $name =~ ^busybox_[0-9A-Za-z.+~-]+\.(orig\.tar\.(bz2|gz|xz)|debian\.tar\.(xz|gz))$ ]] || {
        echo "unexpected file in $dsc_name: $name" >&2; exit 1;
    }
    fetch "$snapshot/$source_dir/$name" "$sha" "$snapshot_cache/$name"
    cp "$snapshot_cache/$name" "$ST/sources/$name"
    count=$((count + 1))
done <<< "$source_checksums"
[[ $count -ge 2 ]] || { echo "incomplete source in $dsc_name" >&2; exit 1; }

cat > "$ST/README.txt" <<EOF
FX Steam Launcher $version — corresponding GPL/LGPL source ($REF)

The DMG contains the following binaries. The tarballs in sources/ are the
complete upstream sources; repo/ contains this release's exact configuration,
patches and scripts. All tarballs were checked against their pinned SHA-256,
and Debian's .dsc was checked against the snapshot Sources index; the .dsc
contains SHA-256 checksums for both busybox source tarballs.

Image: Linux $KVER, sources/linux-$KVER.tar.xz (SHA-256 $KSHA256), patched in
filename order with repo/guest/kernel/patches/*.patch. The kernel config is
repo/guest/kernel/config-steamac (and its generating fragments are in
repo/guest/kernel/config/). Follow repo/guest/kernel/build.sh, Dockerfile and
container-build.sh; build.sh verifies the tarball, applies patches with -F0,
merges config fragments, builds arm64 Image and saves the resolved config.

initramfs.cpio.gz: built by repo/scripts/steps/20-initramfs.sh using the pinned
repo/scripts/config.env and repo/scripts/builder/Dockerfile (the builder image
is provisioned by repo/scripts/build-image.sh). /init is
repo/guest/initramfs/init. Its static binaries are:
  /bin/busybox: Debian busybox-static $binary_version (source busybox
    $busybox_version), from the snapshot.debian.org Debian trixie arm64
    Packages/Sources indexes at $DEBIAN_SNAPSHOT. Apply the Debian packaging
    patches via dpkg-source -x sources/$dsc_name, then follow Debian's
    debian/rules to build busybox-static. The .dsc, orig.tar and debian.tar are
    all in sources/; the binary is installed from the builder image.
  /bin/fsck.fat and /bin/mkfs.fat: sources/dosfstools-$DOSFSTOOLS_VERSION.tar.gz,
    built statically in step 20 (see the configure/make commands there).
  /bin/mke2fs: sources/e2fsprogs-$E2FSPROGS_VERSION.tar.xz, built statically in
    step 20, using the builder image's /etc/mke2fs.conf.
  /bin/btrfstune: sources/btrfs-progs-v$BTRFSPROGS_VERSION.tar.xz, built
    statically in step 20. See that script for all configure/make flags.

$dxmt_notes
$mullion_notes
$vrhost_notes
Debian snapshot archive: $snapshot
EOF

output="$ROOT/work/out/dist/FX-Steam-Launcher-$version-gpl-sources.tar"
# Write to a temporary file so an interrupted run cannot leave a partial release asset.
tar -cf "$ST/archive.tar" -C "$ST" README.txt repo sources
mv "$ST/archive.tar" "$output"
echo "GPL source archive: $output"
