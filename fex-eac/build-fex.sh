#!/bin/sh
# Build FEX-2610 with the EAC patches inside the SteamOS guest and stage the runtime for install-fex-tool.sh.
# POSIX sh; needs the guest's clang 19, cmake, ninja, lld, git, python3. Re-running is safe.
set -eu

WORK=${WORK:-$HOME/fex-eac-work}
SRC=$WORK/FEX
STAGE=$WORK/stage
TAG=FEX-2610
BASE=14c92681f4d62cf84d901460e0358de09c8847a7
PATCHES=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/patches

set -- "$PATCHES"/*.patch
NPATCH=$#

# Clone the exact upstream tag with shallow submodules unless the tree is already there.
if [ ! -d "$SRC/.git" ]; then
  mkdir -p "$WORK"
  git clone --recurse-submodules --shallow-submodules --branch "$TAG" \
    https://github.com/FEX-Emu/FEX.git "$SRC"
fi
cd "$SRC"

# The clone must be the commit the patches were made against.
TAGSHA=$(git rev-parse "$TAG^{commit}")
[ "$TAGSHA" = "$BASE" ] || { echo "$TAG is $TAGSHA, expected $BASE" >&2; exit 1; }

# Apply the patch series once; a re-run must find exactly the series already applied and nothing else.
ONTOP=$(git rev-list --count "$BASE"..HEAD)
if [ "$ONTOP" -eq 0 ]; then
  git -c user.name=build -c user.email=build@invalid am "$PATCHES"/*.patch
elif [ "$ONTOP" -ne "$NPATCH" ]; then
  echo "$(git rev-parse --short HEAD) has $ONTOP commits on top of $BASE; expected 0 or $NPATCH" >&2
  exit 1
fi

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

echo "staged: $STAGE"
