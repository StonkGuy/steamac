#!/bin/sh
# Build the patched FEX (StonkGuy/FEX, a pinned eac-* tag) inside the SteamOS guest and stage the runtime for
# install-fex-tool.sh. POSIX sh; needs the guest's clang 19, cmake, ninja, lld, git, python3. Re-running is safe.
set -eu

WORK=${WORK:-$HOME/fex-eac-work}
SRC=$WORK/FEX
STAGE=$WORK/stage
REPO=https://github.com/StonkGuy/FEX.git
FEX_EAC_TAG=eac-2610.4
FEX_EAC_SHA=b608a47670de0ce6cca519858c2e57a2590344da

# Clone the exact tag with shallow submodules unless the tree is already there.
if [ ! -d "$SRC/.git" ]; then
  mkdir -p "$WORK"
  git clone --recurse-submodules --shallow-submodules --branch "$FEX_EAC_TAG" "$REPO" "$SRC"
fi
cd "$SRC"

# The tree must be the pinned commit. A tree from an older build-fex.sh (upstream FEX-2610 with the patch files
# applied by git am) or from another tag is not updated in place.
HEADSHA=$(git rev-parse HEAD)
if [ "$HEADSHA" != "$FEX_EAC_SHA" ]; then
  if git cat-file -e "$FEX_EAC_TAG^{commit}" 2>/dev/null &&
     [ "$(git rev-parse "$FEX_EAC_TAG^{commit}")" = "$FEX_EAC_SHA" ] && git diff --quiet HEAD; then
    git checkout -q "$FEX_EAC_TAG" && git submodule update --init --recursive --depth 1
  else
    echo "$SRC is at $(git rev-parse --short HEAD), not $FEX_EAC_TAG ($FEX_EAC_SHA)." >&2
    echo "It is probably a tree from an older build-fex.sh. Remove it and run this script again:" >&2
    echo "  rm -rf '$SRC'" >&2
    exit 1
  fi
fi
[ "$(git rev-parse "$FEX_EAC_TAG^{commit}")" = "$FEX_EAC_SHA" ] ||
  { echo "$FEX_EAC_TAG is $(git rev-parse "$FEX_EAC_TAG^{commit}"), expected $FEX_EAC_SHA" >&2; exit 1; }

# Release Steam-mode build: no tests, thunks or FEXConfig, native CPU tuning, module scanning off for clang 19.
cmake -S . -B build-steam -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DBUILD_STEAM_SUPPORT=ON \
  -DBUILD_TESTING=OFF \
  -DBUILD_THUNKS=OFF \
  -DBUILD_FEXCONFIG=OFF \
  -DENABLE_LTO=OFF \
  -DENABLE_ASSERTIONS=OFF \
  -DTUNE_CPU=native \
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
  -DCMAKE_INSTALL_PREFIX=/usr

# Build and stage only the Runtime component: that is what Valve's compat tool ships.
cmake --build build-steam -j"${JOBS:-$(nproc)}"
DESTDIR=$STAGE cmake --install build-steam --component Runtime

echo "staged: $STAGE ($FEX_EAC_TAG)"
