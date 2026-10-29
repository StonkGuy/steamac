#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# host/mullion/patches/ is generated: the commits of the Mullion branch MULLION_FORK_BRANCH between the upstream
# base (MULLION_COMMIT) and the pinned commit (MULLION_FORK_COMMIT), one patch per commit, authors kept. Never edit
# the patches by hand: change the branch, move MULLION_FORK_COMMIT in host/mullion/pin.env, run this.
#   scripts/export-mullion-patches.sh [OUTDIR]     default OUTDIR: host/mullion/patches (replaced)
# MULLION_FORK_CHECKOUT names a clone that has the branch; without it the branch is fetched from MULLION_FORK_REPO.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/host/mullion/pin.env"
out=${1:-$ROOT/host/mullion/patches}
repo=${MULLION_FORK_CHECKOUT:-$ROOT/work/cache/mullion-fork.git}
[[ -d $repo ]] || git init -q --bare "$repo"
if ! git -C "$repo" cat-file -e "$MULLION_FORK_COMMIT^{commit}" 2>/dev/null; then
    [[ -n $MULLION_FORK_REPO ]] || { echo "MULLION_FORK_COMMIT is not in $repo (set MULLION_FORK_CHECKOUT)" >&2; exit 1; }
    git -C "$repo" fetch -q "$MULLION_FORK_REPO" "$MULLION_FORK_BRANCH" "$MULLION_COMMIT"
fi
git -C "$repo" merge-base --is-ancestor "$MULLION_COMMIT" "$MULLION_FORK_COMMIT" \
    || { echo "MULLION_FORK_COMMIT is not based on MULLION_COMMIT" >&2; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
# stable output: no signature, no commit ids in the From lines, full index lines
git -C "$repo" format-patch -q --zero-commit --no-signature --full-index -o "$tmp" "$MULLION_COMMIT..$MULLION_FORK_COMMIT"
rm -rf "$out"; mkdir -p "$out"; cp "$tmp"/*.patch "$out"/
echo "exported $(ls "$out" | wc -l | tr -d ' ') patches ($MULLION_COMMIT..$MULLION_FORK_COMMIT) to $out"
