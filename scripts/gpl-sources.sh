#!/usr/bin/env bash
# Package corresponding source for a release: the GPL kernel and static initramfs tools, the LGPL
# DXMT source (pinned tarball, host/dxmt/patches, wmtr/) behind libwmtr_host.dylib, and the GPL-3.0
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
        # Tracked files only: build output under wmtr/ stays out.
        git -C "$ROOT" ls-files -z -- host/dxmt wmtr | tar -C "$ROOT" --null -T - -cf - | tar -xf - -C "$ST/repo"
    fi
    if [[ -f $ROOT/work/out/host/bin/steamac-vrhost ]]; then
        git -C "$ROOT" ls-files -z -- host/vrhost | tar -C "$ROOT" --null -T - -cf - | tar -xf - -C "$ST/repo"
    fi
else
    git -C "$ROOT" rev-parse --verify "$REF^{commit}" >/dev/null
    git -C "$ROOT" archive "$REF" host/launcher/Info.plist scripts/config.env \
        scripts/steps/20-initramfs.sh scripts/builder/Dockerfile scripts/build-image.sh \
        guest/initramfs/init guest/kernel | tar -xf - -C "$ST/repo"
    cp "$ST/repo/host/launcher/Info.plist" "$ST/Info.plist"
    rm -rf "$ST/repo/host"
    if git -C "$ROOT" cat-file -e "$REF:host/dxmt/pin.env" 2>/dev/null; then
        git -C "$ROOT" archive "$REF" host/dxmt wmtr | tar -xf - -C "$ST/repo"
    fi
    # The VR host ships only in apps built with host/vrhost/build.sh; include its source whenever the tag has it.
    if git -C "$ROOT" cat-file -e "$REF:host/vrhost/CMakeLists.txt" 2>/dev/null; then
        git -C "$ROOT" archive "$REF" host/vrhost | tar -xf - -C "$ST/repo"
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

# DXMT (LGPL-2.1-or-later), present from the release that ships the DXMT graphics path.
dxmt_notes=
if [[ -f $ST/repo/host/dxmt/pin.env ]]; then
    . "$ST/repo/host/dxmt/pin.env"
    [[ $DXMT_COMMIT =~ ^[[:xdigit:]]{40}$ && $DXMT_SHA256 =~ ^[[:xdigit:]]{64}$ && $LLVM_VERSION =~ ^[0-9]+(\.[0-9]+)*$ ]] || {
        echo 'invalid host/dxmt/pin.env' >&2; exit 1;
    }
    copy_source "$ROOT/work/cache/dxmt/dxmt-$DXMT_COMMIT.tar.gz" "$DXMT_URL" "$DXMT_SHA256" "dxmt-$DXMT_COMMIT.tar.gz"
    dxmt_notes="
libwmtr_host.dylib (the DXMT graphics path, loaded by libkrun at run time and
replaceable by a rebuilt library of the same name): DXMT, LGPL-2.1-or-later,
sources/dxmt-$DXMT_COMMIT.tar.gz ($DXMT_REPO at that commit, SHA-256
$DXMT_SHA256). Apply repo/host/dxmt/patches/*.patch in filename order with
git apply (patch -p1 also works). repo/wmtr/ links DXMT's
src/winemetal/unix/winemetal_unix.c and its src/airconv (with DXBCParser)
into the library: build per the header of repo/wmtr/Makefile (make host).
DXMT's own git submodules (NVIDIA nvapi and misyltoad's mingw-directx-headers,
revisions recorded in the DXMT commit) are used only for the Windows-side DLLs
and are not part of the library. The shader compiler links LLVM $LLVM_VERSION
statically (Apache-2.0 with LLVM exception; its licence text is in the app's
licenses folder), built from the llvmorg-$LLVM_VERSION tag of
github.com/llvm/llvm-project."
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
FX Steam Launcher $version — corresponding GPL source ($REF)

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
$vrhost_notes
Debian snapshot archive: $snapshot
EOF

output="$ROOT/work/out/dist/FX-Steam-Launcher-$version-gpl-sources.tar"
# Write to a temporary file so an interrupted run cannot leave a partial release asset.
tar -cf "$ST/archive.tar" -C "$ST" README.txt repo sources
mv "$ST/archive.tar" "$output"
echo "GPL source archive: $output"
