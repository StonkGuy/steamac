#!/usr/bin/env bash
# scripts/guest-toolchain.sh against a stand-in docker: the record names the pinned images and the
# answers of the containers, the busybox copyright lands under the builder image id, and an empty
# copyright or a malformed rustc answer writes no record. No Docker, no network.
#   scripts/test/guest-toolchain.sh
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/guest-toolchain.XXXXXXXX")
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/repo/scripts" "$T/bin"
cp "$REPO/scripts/guest-toolchain.sh" "$REPO/scripts/config.env" "$T/repo/scripts/"
cat > "$T/bin/docker" <<'SH'
#!/bin/sh
case "$*" in
    "image inspect"*) echo "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" ;;
    *"rustc --version") echo "${FAKE_RUSTC-rustc 1.99.0 (b940084d7 2026-09-28)}" ;;
    *dpkg-query*) printf '1:1.37.0-6+b3' ;;
    *copyright) printf '%s' "${FAKE_COPYRIGHT-busybox copyright text}" ;;
    *) echo "unexpected docker $*" >&2; exit 1 ;;
esac
SH
chmod +x "$T/bin/docker"
record=$T/repo/work/out/guest-toolchain.json
fails=0
check() { if "${@:2}"; then echo "  ok   $1"; else echo "  FAIL $1" >&2; fails=$((fails + 1)); fi; }

PATH="$T/bin:$PATH" "$T/repo/scripts/guest-toolchain.sh" >/dev/null
. "$REPO/scripts/config.env"
field() { python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$record" "$1"; }
check "rust version" test "$(field rust_version)" = 1.99.0
check "rust image pin" test "$(field rust_image)" = "$RUST_IMAGE"
check "builder image pin" test "$(field builder_image)" = "$BUILDER_IMAGE"
check "builder id" test "$(field builder_id)" = 0123456789abcdef
check "busybox version" test "$(field busybox_version)" = "1:1.37.0-6+b3"
check "busybox copyright" grep -q 'busybox copyright text' "$T/repo/work/cache/licenses/busybox-0123456789abcdef-copyright"

rm -rf "$T/repo/work"
check "empty copyright is refused" bash -c "! FAKE_COPYRIGHT= PATH='$T/bin':\$PATH '$T/repo/scripts/guest-toolchain.sh' >/dev/null 2>&1"
check "no record after a refusal" test ! -e "$record"
check "malformed rustc answer is refused" bash -c "! FAKE_RUSTC='error' PATH='$T/bin':\$PATH '$T/repo/scripts/guest-toolchain.sh' >/dev/null 2>&1"
check "no record after a malformed answer" test ! -e "$record"

if [[ $fails == 0 ]]; then echo "guest-toolchain: PASS"; else echo "guest-toolchain: $fails FAILED" >&2; exit 1; fi
