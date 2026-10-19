#!/bin/bash
# Builds Proton's vrclient_x64 (PE dll + unixlib) with patches/ applied, from the Proton commit in pin.env, with
# Proton's own build system in its SDK container (podman or docker). x86-64 Linux; about 4 GB of sources, 14 GB of
# build tree and 20 minutes on 16 threads the first time (the module needs Proton's Wine), seconds afterwards.
#   guest/proton-vrclient/build.sh [WORK] [OUT]
#     WORK  checkout and build tree (default ~/.cache/steamac/proton-vrclient)
#     OUT   result, in the layout the guest layer ships (scripts/steps/30-layer.sh, /work/out/vr):
#             OUT/x86_64-windows/vrclient_x64.dll  OUT/x86_64-unix/vrclient_x64.so   (default WORK/out)
# Then tests/run.sh WORK <guest/vr build> checks the result under the Wine that was just built.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=${1:-$HOME/.cache/steamac/proton-vrclient}
. "$here/pin.env"
# Canonicalize WORK before deriving the default output path; keep relative OUT relative to the caller.
mkdir -p "$work"
work=$(cd "$work" && pwd)
out=${2:-$work/out}
mkdir -p "$out"
out=$(cd "$out" && pwd)
engine=${CONTAINER_ENGINE:-$(basename "$(command -v podman || command -v docker)")}
src=$work/proton build=$work/build

if [[ ! -d $src/.git ]]; then
    mkdir -p "$src"
    git -C "$src" init -q
fi
if ! git -C "$src" cat-file -e "$PROTON_COMMIT^{commit}" 2>/dev/null; then
    git -C "$src" fetch -q --depth 1 "$PROTON_REPO" "$PROTON_COMMIT"
fi
current=$(git -C "$src" rev-parse HEAD 2>/dev/null || true)
if [[ $current != "$PROTON_COMMIT" ]]; then
    [[ -z $(git -C "$src" status --porcelain) ]] || { echo "refusing to replace modified Proton source in $src" >&2; exit 1; }
    git -C "$src" checkout -q --detach "$PROTON_COMMIT"
fi
# Patches may already be applied by a previous invocation, but reject unrelated source edits.
git -C "$src" submodule update --init --recursive --depth 1 --jobs 8 ||
    git -C "$src" submodule update --init --recursive --jobs 8
for p in "$here"/patches/*.patch; do
    if git -C "$src" apply --check "$p"; then
        git -C "$src" apply --index "$p"
        echo "applied $(basename "$p")"
    elif git -C "$src" apply --reverse --check "$p"; then
        echo "already applied $(basename "$p")"
    else
        echo "patch does not apply cleanly: $p" >&2
        exit 1
    fi
done

mkdir -p "$build"
cd "$build"
[[ -f Makefile ]] || "$src/configure.sh" --container-engine="$engine" --build-name=steamac-vr --relabel-volumes
# SELinux hosts (Fedora): the container must read the bind-mounted trees
grep -q '^DOCKER_OPTS := --security-opt label=disable' Makefile || sed -i '1i DOCKER_OPTS := --security-opt label=disable' Makefile
make -j"$(nproc)" vrclient

for f in x86_64-windows/vrclient_x64.dll x86_64-unix/vrclient_x64.so; do
    install -D -m 0644 "$build/dist/files/lib/wine/$f" "$out/$f"
done
# the build must contain the patch
grep -q 'metal texture' "$out/x86_64-windows/vrclient_x64.dll" || { echo "vrclient_x64.dll has no DXMT path" >&2; exit 1; }
(cd "$out" && sha256sum x86_64-windows/vrclient_x64.dll x86_64-unix/vrclient_x64.so)
echo "proton-vrclient: $PROTON_BRANCH $PROTON_COMMIT + $(ls "$here"/patches/*.patch | wc -l) patch(es) -> $out"
