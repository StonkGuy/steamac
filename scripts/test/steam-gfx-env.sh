#!/bin/bash
# Host-side gfx.txt ownership checks; use GNU sed (as in the guest).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
S=$REPO/guest/layer/usr/lib/steamac/steam-gfx-env
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export HOME=$T/home XDG_RUNTIME_DIR=$T/runtime
mkdir -p "$HOME" "$XDG_RUNTIME_DIR/steam/env" "$T/bin"
printf '#!/bin/sh\nexit 1\n' > "$T/bin/vulkaninfo"
chmod +x "$T/bin/vulkaninfo"
export PATH=$T/bin:$PATH
gfx=$XDG_RUNTIME_DIR/steam/env/gfx.txt
# Defaults must be inherited from Steam; putting them in gfx.txt would overwrite
# DD2's launch options in the client-provided steam-launch-wrapper.
grep -qx 'Environment=VKD3D_DISABLE_EXTENSIONS=VK_KHR_shader_atomic_int64,VK_EXT_shader_image_atomic_int64' \
    "$REPO/guest/layer/usr/lib/systemd/user/steam.service.d/50-steamac.conf"
for value in absent '' VK_EXT_custom 'VK_EXT_custom;VK_KHR_shader_atomic_int64'; do
    printf 'UNRELATED=keep\nVK_VRAM_REPORT_LIMIT_DEVICE_ID=stock\n' > "$gfx"
    if [[ $value != absent ]]; then
        printf 'VKD3D_DISABLE_EXTENSIONS=%s\n' "$value" >> "$gfx"
    fi
    cp "$gfx" "$T/first"
    bash "$S"
    cmp "$gfx" "$T/first"
    bash "$S"
    cmp "$gfx" "$T/first"
    printf 'PASS explicit gfx.txt precedence + repeated run: %s -> unchanged\n' "$value"
done
rm "$T/bin/vulkaninfo"
bash "$S"
cmp "$gfx" "$T/first"
echo 'PASS missing vulkaninfo leaves graphics environment intact'
