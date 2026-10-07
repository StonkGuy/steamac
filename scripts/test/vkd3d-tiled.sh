#!/bin/bash
# DEV-ONLY end-to-end check of D3D12 tiled resources and the feature level through the real
# stack: guest vkd3d-proton (native aarch64 Linux build) -> Venus -> virglrenderer -> the
# host Vulkan driver, independent of any game. Uses a throwaway disk, never work/out/steamos.img.
#
#   scripts/test/vkd3d-tiled.sh build   vkd3d-proton at the commit Proton 11.0 ships (tests
#                                       enabled), scripts/test/d3d12-caps.c and vk-minmax.c, built
#                                       for aarch64 in debian:bookworm (glibc 2.36 <= the guest's)
#   scripts/test/vkd3d-tiled.sh disk    provisioned throwaway disk ($S/vm/test.img)
#   scripts/test/vkd3d-tiled.sh start   boot it headless with SSH on $SSH_PORT (private copies of
#                                       work/out/{Image,initramfs.cpio.gz,steamac-layer.img}; the
#                                       host libraries are work/out/host/lib as they are now)
#   scripts/test/vkd3d-tiled.sh run [TEST...]
#                                       in the guest: vulkaninfo sparse/residency/minmax lines,
#                                       d3d12-caps, vk-minmax (sampler min/max reduction), then
#                                       each vkd3d-proton test in its own process
#                                       (default: the tiled-resource/min-LOD/min-max list below);
#                                       results in $S/results-N/ (summary.txt + one log per test)
#   scripts/test/vkd3d-tiled.sh stop    shut the VM down
#   scripts/test/vkd3d-tiled.sh clean   stop, delete $S/vm (the disk); keeps the build
#
# Env: S (default work/scratch/vkd3d-tiled), SSH_PORT (default 2241), VULKAN_DRIVER
# (kosmickrisp|moltenvk, default kosmickrisp), TEST_TIMEOUT (s per test, default 300),
# GUEST_ENV (extra env for the tests, e.g. "VKD3D_DEBUG=trace"); start passes its environment
# (e.g. MESA_KK_EXPERIMENTAL=image_view_min_lod) to the VM and its Vulkan driver.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
S=${S:-$REPO/work/scratch/vkd3d-tiled}
PORT=${SSH_PORT:-2241}
# Proton 11.0's vkd3d-proton submodule (ValveSoftware/Proton branch proton_11.0)
VKD3D_PROTON_URL=https://github.com/HansKristian-Work/vkd3d-proton
VKD3D_PROTON_COMMIT=212991fc2c266bc0d59f4c4ce8f80f7126508d71
BUILD_IMAGE=docker.io/library/debian:bookworm@sha256:2c037a04925515fdd6ea85ea14a682d0e79931f5e9f5d07b6dbfc6ba12f9e858
TESTS_DEFAULT=(
    test_create_reserved_resource test_get_resource_tiling test_reserved_resource_mapping
    test_update_tile_mappings test_update_tile_mappings_remap_stress test_copy_tiles
    test_sparse_default_mapping test_sparse_buffer_memory_lifetime test_sparse_depth_stencil_rendering
    test_buffer_feedback_instructions_sm51 test_buffer_feedback_instructions_dxil
    test_texture_feedback_instructions_sm51 test_texture_feedback_instructions_dxil
    test_view_min_lod test_create_sampler
)

ssh_g() {
    sshpass -p "${STEAMOS_PASSWORD:-steamos}" ssh -q -p "$PORT" -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o ControlMaster=auto \
        -o ControlPath="/tmp/vkd3d-tiled-ssh-$PORT" -o ControlPersist=120 steamos@127.0.0.1 "$@"
}

c_build() { # container side: /src (vkd3d-proton), /out, /t (scripts/test)
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        meson ninja-build gcc g++ pkgconf glslang-tools mingw-w64-tools libvulkan-dev > /dev/null
    ln -sf /usr/bin/x86_64-w64-mingw32-widl /usr/local/bin/widl
    meson setup /out/build /src --buildtype=release -Denable_tests=true --wipe > /out/meson.log 2>&1 \
        || meson setup /out/build /src --buildtype=release -Denable_tests=true >> /out/meson.log 2>&1 \
        || { tail -30 /out/meson.log; exit 1; }
    ninja -C /out/build libs/d3d12/libvkd3d-proton-d3d12.so tests/d3d12 > /out/ninja.log 2>&1 \
        || { tail -40 /out/ninja.log; exit 1; }
    mkdir -p /out/bin
    gcc -O2 -Wall -o /out/bin/d3d12-caps /t/d3d12-caps.c -I/src/include \
        -I/out/build/libs/vkd3d-common/libvkd3d_common.a.p -I/out/build \
        -I/src/khronos/Vulkan-Headers/include -L/out/build/libs/d3d12 -lvkd3d-proton-d3d12 \
        -Wl,-rpath,'$ORIGIN'
    glslangValidator -V --target-env vulkan1.2 --quiet --vn vk_minmax_spv -o /out/build/vk_minmax_spv.h \
        /t/vk-minmax.comp
    gcc -O2 -Wall -o /out/bin/vk-minmax /t/vk-minmax.c -I/out/build -lvulkan -lm
    cp /out/build/tests/d3d12 /out/bin/
    find /out/build/libs \( -type f -o -type l \) -name 'libvkd3d-proton*.so' -exec cp -P {} /out/bin/ \;
    ldd /out/bin/d3d12 /out/bin/d3d12-caps /out/bin/vk-minmax | grep -v '^\s*/' || :
    gcc --version | head -1; ldd --version | head -1
}

build() {
    local src=$S/src
    if [[ ! -d $src/.git ]]; then git clone -q "$VKD3D_PROTON_URL" "$src"; fi
    git -C "$src" cat-file -e "$VKD3D_PROTON_COMMIT^{commit}" 2>/dev/null || git -C "$src" fetch -q origin
    git -C "$src" checkout -q "$VKD3D_PROTON_COMMIT"
    git -C "$src" submodule update -q --init --recursive
    mkdir -p "$S/out"
    docker run --rm --platform linux/arm64 -v "$src:/src:ro" -v "$S/out:/out" \
        -v "$REPO/scripts/test:/t:ro" "$BUILD_IMAGE" bash /t/vkd3d-tiled.sh _c_build
    echo "[vkd3d-tiled] built $(git -C "$src" log -1 --format='%h %s') -> $S/out/bin"
}

disk() {
    TEST_WORK=$S/vm "$REPO/scripts/test/provision-test-disk.sh" disk
    TEST_WORK=$S/vm "$REPO/scripts/test/provision-test-disk.sh" boot provision-only
}

start() {
    local vm=$S/vm log
    [[ -f $vm/test.img ]] || { echo "run '$0 disk' first" >&2; exit 1; }
    [[ -f $vm/vm.pid ]] && kill -0 "$(cat "$vm/vm.pid")" 2>/dev/null && { echo "already running"; return; }
    cp -c "$REPO/work/out/Image" "$REPO/work/out/initramfs.cpio.gz" "$REPO/work/out/steamac-layer.img" "$vm/"
    log=$vm/vm-$(date +%Y%m%d-%H%M%S).log
    nohup "$REPO/work/out/steamac-vm" --kernel "$vm/Image" --initrd "$vm/initramfs.cpio.gz" \
        --cmdline "console=hvc0 loglevel=4 rootwait" \
        --disk "$vm/test.img" --disk "$vm/steamac-layer.img:ro" \
        --ssh-port "$PORT" --no-sound --headless --cpus 4 --mem 8192 \
        --vulkan-driver "${VULKAN_DRIVER:-kosmickrisp}" --no-crash-reports \
        --log "$log.console" > "$log" 2>&1 < /dev/null &
    echo $! > "$vm/vm.pid"
    echo "MESA_KK_EXPERIMENTAL=${MESA_KK_EXPERIMENTAL:-} MESA_KK_DEBUG=${MESA_KK_DEBUG:-}" > "$vm/vm.env"
    ln -sf "$(basename "$log")" "$vm/vm.log"
    local t=0
    # boot finished (not just sshd up): systemd reports running/degraded
    until [[ $(ssh_g 'systemctl is-system-running' 2>/dev/null) =~ ^(running|degraded) ]]; do
        kill -0 "$(cat "$vm/vm.pid")" 2>/dev/null || { tail -20 "$log"; exit 1; }
        sleep 3; t=$((t + 3)); ((t < 300)) || { echo "guest not up after 300 s ($log)" >&2; exit 1; }
    done
    echo "[vkd3d-tiled] VM up (pid $(cat "$vm/vm.pid"), SSH $PORT, log $log)"
}

stop() {
    local pid
    pid=$(cat "$S/vm/vm.pid" 2>/dev/null) || return 0
    kill -TERM "$pid" 2>/dev/null || :
    for _ in $(seq 60); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
    kill -KILL "$pid" 2>/dev/null || :
    rm -f "$S/vm/vm.pid"
}

run() {
    local tests=("$@") n=1 R t
    ((${#tests[@]})) || tests=("${TESTS_DEFAULT[@]}")
    while [[ -e $S/results-$n ]]; do n=$((n + 1)); done
    R=$S/results-$n; mkdir -p "$R"
    local i
    # retried: right after boot the guest's sshd occasionally drops a session
    for i in 1 2 3 4 5; do
        (cd "$S/out/bin" && COPYFILE_DISABLE=1 tar --no-xattrs -cf - .) \
            | ssh_g 'mkdir -p ~/vkd3d-tiled && tar -xf - -C ~/vkd3d-tiled' && break
        ((i < 5)) || { echo "copy to the guest failed" >&2; exit 1; }
        sleep 5
    done
    {
        echo "== host: macOS $(sw_vers -productVersion), driver ${VULKAN_DRIVER:-kosmickrisp}, $(cat "$S/vm/vm.env" 2>/dev/null)," \
            "$(stat -f '%Sm %N' "$REPO"/work/out/host/lib/libvulkan_kosmickrisp*.dylib 2>/dev/null | head -1)"
        echo "== vkd3d-proton $VKD3D_PROTON_COMMIT"
        echo "== vulkaninfo"
        ssh_g 'vulkaninfo 2>/dev/null' | grep -E 'deviceName|driverName|driverInfo|apiVersion|sparse|[Rr]esidency|shaderResourceMinLod|filterMinmax|minLod|shaderStorageImageReadWithoutFormat|SPARSE_BINDING' \
            | sed 's/^[[:space:]]*//' | sort | uniq -c | sed 's/^ *1 //'
        echo "== d3d12-caps"
        ssh_g "cd ~/vkd3d-tiled && ${GUEST_ENV:-} VKD3D_SHADER_CACHE_PATH=0 LD_LIBRARY_PATH=. ./d3d12-caps 2>&1" || echo "(exit $?)"
        echo "== vk-minmax"
        ssh_g "cd ~/vkd3d-tiled && timeout 120 ./vk-minmax 2>&1" || echo "(exit $?)"
    } > "$R/caps.txt"
    grep -v -E ':(info|fixme):' "$R/caps.txt"
    echo "== tests" | tee "$R/summary.txt"
    for t in "${tests[@]}"; do
        local rc=0 line
        ssh_g "cd ~/vkd3d-tiled && ${GUEST_ENV:-} VKD3D_SHADER_CACHE_PATH=0 VKD3D_TEST_MATCH=$t \
            LD_LIBRARY_PATH=. timeout ${TEST_TIMEOUT:-300} ./d3d12 2>&1" > "$R/$t.log" || rc=$?
        line=$(grep -E '^d3d12: [0-9]+ tests executed' "$R/$t.log" | tail -1 || :)
        local verdict=PASS skipped=0
        [[ $line =~ \ ([0-9]+)\ skipped ]] && skipped=${BASH_REMATCH[1]}
        if ((rc == 124)); then verdict=TIMEOUT
        elif [[ -z $line ]]; then verdict="CRASH(rc=$rc)"
        elif ((rc != 0)); then verdict=FAIL
        elif ((skipped)); then verdict=SKIP
        fi
        printf '%-45s %-8s %s\n' "$t" "$verdict" "${line#d3d12: }" | tee -a "$R/summary.txt"
        # first failure/skip reasons of this test
        grep -E "^$t:[0-9]+:.*(Test failed|Test skipped)" "$R/$t.log" | awk '!seen[$0]++' | head -5 | sed 's/^/    /' \
            | tee -a "$R/summary.txt" || :
    done
    echo "[vkd3d-tiled] results: $R"
}

case ${1:-} in
    _c_build) c_build ;;
    build) build ;;
    disk) disk ;;
    start) start ;;
    run) shift; run "$@" ;;
    stop) stop ;;
    clean) stop; rm -rf "$S/vm" ;;
    *) sed -n '2,26p' "$0"; exit 2 ;;
esac
