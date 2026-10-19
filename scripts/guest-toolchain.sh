#!/usr/bin/env bash
# Record what host/launcher/licenses.sh otherwise asks the build containers: the guest Rust
# version (RUST_IMAGE) and the builder image's busybox-static (BUILDER_IMAGE, scripts/config.env).
# Run it on the machine that built the guest images when the app is assembled on another one
# without Docker (.github/workflows/release.yml: guest images on a Linux runner, the app on a
# macOS runner). Carry both outputs over with the images:
#   work/out/guest-toolchain.json
#   work/cache/licenses/busybox-<builder image id>-copyright
# A host with Docker never reads the record.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/config.env"
OUT=$REPO/work/out/guest-toolchain.json
LICENSES=$REPO/work/cache/licenses
mkdir -p "$(dirname "$OUT")" "$LICENSES"

builder_id=$(docker image inspect -f '{{.Id}}' "$BUILDER_IMAGE")
builder_id=${builder_id##*:}
builder_id=${builder_id:0:16}
rust_version=$(docker run --rm --platform linux/arm64 "$RUST_IMAGE" rustc --version | cut -d' ' -f2)
busybox_version=$(docker run --rm --platform linux/arm64 "$BUILDER_IMAGE" dpkg-query -W '-f=${Version}' busybox-static)
docker run --rm --platform linux/arm64 "$BUILDER_IMAGE" cat /usr/share/doc/busybox-static/copyright \
    > "$LICENSES/busybox-$builder_id-copyright.tmp"
[[ -s $LICENSES/busybox-$builder_id-copyright.tmp ]] || { echo "guest-toolchain.sh: empty busybox copyright" >&2; exit 1; }
mv -f "$LICENSES/busybox-$builder_id-copyright.tmp" "$LICENSES/busybox-$builder_id-copyright"
[[ $rust_version =~ ^[0-9]+(\.[0-9]+)+$ && -n $busybox_version && $builder_id =~ ^[0-9a-f]{16}$ ]] \
    || { echo "guest-toolchain.sh: unexpected container answers ($rust_version, $busybox_version, $builder_id)" >&2; exit 1; }

RUST_IMAGE=$RUST_IMAGE BUILDER_IMAGE=$BUILDER_IMAGE python3 - "$OUT" "$rust_version" "$builder_id" "$busybox_version" <<'PY'
import json, os, sys
out, rust_version, builder_id, busybox_version = sys.argv[1:]
record = {"version": 1, "rust_image": os.environ["RUST_IMAGE"], "rust_version": rust_version,
          "builder_image": os.environ["BUILDER_IMAGE"], "builder_id": builder_id,
          "busybox_version": busybox_version}
with open(out + ".tmp", "w") as f:
    f.write(json.dumps(record, sort_keys=True) + "\n")
os.replace(out + ".tmp", out)
PY
cat "$OUT"
