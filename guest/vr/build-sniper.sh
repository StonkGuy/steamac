#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Release build of the guest VR pieces inside the Steam Runtime 3 "sniper" SDK (glibc 2.31, the runtime Proton
# runs unixlibs in), with podman or docker, then the unit and end-to-end tests in the same container.
#   guest/vr/build-sniper.sh [OUT]   (default OUT=build-sniper; output runtime: OUT/runtime/bin/linux64/vrclient.so)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-build-sniper}
image=${SNIPER_IMAGE:-registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest}
engine=$(command -v podman || command -v docker)
"$engine" run --rm --security-opt label=disable -v "$here:/src" -w /src "$image" \
  sh -c "make -j\$(nproc) OUT=$out CC=gcc CXX=g++ && make OUT=$out check"
