#!/bin/bash
# Test of the patched vrclient_x64 (build.sh) under the Wine of the same Proton build, x86-64 Linux, no headset:
# vrclient_dxmt_test.exe (see its header) submits DXMT-style textures through vrclient_x64.dll; the Linux runtime is
# guest/vr's vrclient.so on the socket transport, and vr-fakehost checks what arrives.
#   guest/proton-vrclient/tests/run.sh WORK VR_BUILD [STOCK_WINE_LIB]
#     WORK            build.sh's WORK (uses WORK/build: Wine and the patched vrclient_x64)
#     VR_BUILD        guest/vr build dir (make -C guest/vr OUT=...): runtime/ and vr-fakehost
#     STOCK_WINE_LIB  optional: lib/wine of an unpatched Proton (e.g. ".../Proton - Experimental/files/lib/wine");
#                     the same test must FAIL with its vrclient_x64 (the test detects the patch)
# Environment: COMPOSITOR=IVRCompositor_0NN (default 028, VRChat's), MANUAL_REGISTRY=1 (skip Proton's own
# vrclient_init_registry, for a runtime without IVRCompositor_028), MINGW_CC, KEEP=1 (keep the work dir).
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=$(cd "${1:?WORK}" && pwd); vr=$(cd "${2:?guest/vr build dir}" && pwd); stock=${3:-}
build=$work/build wine=$work/build/dst-wine-x86_64/bin/wine
[[ -x $wine && -f $build/dist/files/lib/wine/x86_64-windows/vrclient_x64.dll ]] || { echo "no Proton build in $build (run build.sh)" >&2; exit 2; }
[[ -x $vr/vr-fakehost && -f $vr/runtime/bin/linux64/vrclient.so ]] || { echo "no guest/vr build in $vr" >&2; exit 2; }
tmp=$(mktemp -d)
fh=
cleanup() { [[ -n $fh ]] && kill "$fh" 2>/dev/null; WINEPREFIX=$tmp/pfx "$build/dst-wine-x86_64/bin/wineserver" -k 2>/dev/null
            [[ -n ${KEEP:-} ]] && echo "kept $tmp" || rm -rf "$tmp"; }
trap cleanup EXIT
fail() { echo "FAIL proton-vrclient: $*"; exit 1; }

# the test program: mingw on the host, else the one in Proton's SDK container
cc=${MINGW_CC:-$(command -v x86_64-w64-mingw32-gcc || true)}
if [[ -n $cc ]]; then
    "$cc" -O2 -Wall -Werror -o "$tmp/vrclient_dxmt_test.exe" "$here/vrclient_dxmt_test.c" || fail "compile"
else
    engine=$(sed -n 's/^CONTAINER_ENGINE := //p' "$build/Makefile")
    image=registry.gitlab.steamos.cloud/proton/steamrt4/sdk/x86_64:4.0.20260331.220802-0
    [[ -n $engine ]] || fail "no x86_64-w64-mingw32-gcc and no container engine in $build/Makefile"
    "$engine" run --rm --security-opt label=disable -v "$here:/src:ro" -v "$tmp:/out" "$image" \
        x86_64-w64-mingw32-gcc -O2 -Wall -Werror -o /out/vrclient_dxmt_test.exe /src/vrclient_dxmt_test.c || fail "compile"
fi

export WINEPREFIX=$tmp/pfx WINEDEBUG=${WINEDEBUG:--all} DISPLAY= WAYLAND_DISPLAY=
export WINEDLLOVERRIDES="mscoree,mshtml=d;winemenubuilder.exe=d"
timeout 180 "$wine" wineboot -i >"$tmp/wineboot.log" 2>&1 || { tail -5 "$tmp/wineboot.log"; fail "wineboot"; }

# one run: $1 = lib/wine holding x86_64-windows/vrclient_x64.dll and x86_64-unix/vrclient_x64.so, $2 = name
run() {
    local lib=$1 d=$tmp/$2 rc manual_registry_arg=
    [[ ${MANUAL_REGISTRY:-} == 1 ]] && manual_registry_arg=manual-registry
    mkdir -p "$d"
    # the game's openvr_api.dll loads vrclient_x64.dll by path; its unixlib is found through the dll path
    cp "$lib/x86_64-windows/vrclient_x64.dll" "$tmp/vrclient_dxmt_test.exe" "$d/" || return 2
    "$vr/vr-fakehost" --socket "$d/vr.sock" --log "$d/host.log" 2>/dev/null &
    fh=$!
    local i=0; while [[ ! -S $d/vr.sock && $i -lt 100 ]]; do sleep 0.05; i=$((i + 1)); done
    (cd "$d" && WINEDLLPATH=$lib PROTON_VR_RUNTIME=$vr/runtime STEAMAC_VR_SOCKET=$d/vr.sock STEAMAC_VR_LOG=1 \
        timeout 120 "$wine" vrclient_dxmt_test.exe "${COMPOSITOR:-IVRCompositor_028}" 60 $manual_registry_arg \
        >"$d/app.out" 2>"$d/app.err")
    rc=$?
    kill "$fh" 2>/dev/null; wait "$fh" 2>/dev/null; fh=
    return $rc
}

run "$build/dist/files/lib/wine" patched
rc=$?
cat "$tmp/patched/app.out"
log=$tmp/patched/host.log
[[ $rc == 0 ]] || { grep -v '^$' "$tmp/patched/app.err" | tail -15; fail "vrclient_dxmt_test.exe exit $rc"; }
# host side: 60 frames of two textures (handles 0x1000/0x1001, left whole and gamma, right a flipped half), each
# waiting for the later eye's completion value on event 0x2000; then one array texture, one slice per eye
grep -q '"ok":false' "$log" && { grep '"ok":false' "$log" | head -3; fail "bad submit"; }
n=$(grep -c '"texture":\[4096,4097\],"rect0":\[0,0,0,0\],"flags":\[2,3\],"slice":\[0,0\],"wait":\[8192,' "$log")
[[ $n == 60 ]] || { grep '"ev":"submit"' "$log" | head -3; fail "$n of 60 two-texture submits arrived as expected"; }
grep -q '"texture":\[4096,4097\],.*"wait":\[8192,60\]' "$log" || fail "completion value of the last frame"
grep -q '"texture":\[4352,4352\],"rect0":\[0,0,0,0\],"flags":\[2,2\],"slice":\[1,0\],"wait":\[8192,2\]' "$log" ||
    { grep '4352' "$log"; fail "array texture submit (SubmitWithArrayIndex)"; }
summary=$(grep '"ev":"summary"' "$log")
case $summary in *'"submits":61,"submit_errors":0'*) ;; *) fail "host summary: $summary" ;; esac
echo "host: $summary"

if [[ -n $stock ]]; then
    MANUAL_REGISTRY=1 COMPOSITOR=${COMPOSITOR:-IVRCompositor_028} run "$stock" stock
    rc=$?
    [[ $rc == 1 ]] || { cat "$tmp/stock/app.out"; fail "an unpatched vrclient_x64 must fail the test (exit $rc)"; }
    grep -q '"ev":"submit"' "$tmp/stock/host.log" && fail "an unpatched vrclient_x64 submitted a frame"
    echo "unpatched vrclient_x64: $(tail -1 "$tmp/stock/app.out") (expected)"
fi
echo "PASS proton-vrclient"
