# Real WiVRn client test (experimental)

This procedure is for the first real-client validation of the Mac-native `steamac-vrhost`. It is separate from the
fake-headset `loopback` test: passing loopback does not prove that a Linux WiVRn client, its graphics runtime, or the
network path works. The target client is WiVRn **v26.9**; the host reports protocol hash `fc26e619b7ab80f8`.

The procedure has not yet been completed end-to-end. The Fedora x86 host was temporarily unreachable during the client
build, so the Linux build and all real-client observations below remain pending. No host bug is inferred from that
blocker.

## Machines and prerequisites

* Mac host: Apple Silicon macOS with the `steamac-vrhost` build, CMake/Ninja build dependencies already resolved, and
  Local Network permission granted to `com.steamac.vrhost` if macOS prompts.
* Client host: Fedora 44 x86-64, same LAN as the Mac, WiVRn source checked out at exact tag `v26.9`, and a working
  graphical login session. The client build uses CMake and Ninja and enables only the non-Android client target.
* Monado with its simulated HMD driver must be installed and usable by the client. The exact Fedora package set and
  Monado simulated-driver/headless configuration have not been verified on the target host; do not assume a particular
  driver-selection environment variable. First run the client in the normal graphical session and establish that the
  simulated device appears in Monado/OpenXR. A truly headless run is out of scope until that is demonstrated.
* Permit TCP and UDP port 9757 between the two hosts. Bonjour discovery additionally needs mDNS on UDP 5353 within the
  LAN. If multicast discovery is blocked, use the WiVRn client's manual server/IP entry and the Mac's LAN address.

The x86-side scripts in `scripts/vr-realclient/` are intended to be copied to `~/vr-realclient/scripts/` (or run from a
checkout of this repository). They do not install operating-system packages, start Monado, create a virtual display,
change firewall rules, or silently select a server.

## Build WiVRn client on Fedora

On the Fedora host, install the Fedora equivalents of WiVRn v26.9's documented Linux client dependencies, plus the
compiler, CMake and Ninja. The upstream v26.9 build guide lists dependency names for Arch rather than Fedora; package
availability/names on Fedora must be checked locally. The earlier Podman attempt needed `cjson-devel`, `librsvg2-tools`
and the KTX 4.4.2 executable; building bundled KTX source with GCC 16 failed in fmt, so use the packaged KTX tool or a
verified release binary rather than repeating that source build.

```sh
mkdir -p ~/vr-realclient/{logs}
git clone --branch v26.9 --depth 1 https://github.com/WiVRn/WiVRn.git ~/vr-realclient/WiVRn
# Copy scripts/vr-realclient/build-client.sh into ~/vr-realclient/scripts/ if not running from this checkout.
chmod +x ~/vr-realclient/scripts/build-client.sh
~/vr-realclient/scripts/build-client.sh ~/vr-realclient/WiVRn ~/vr-realclient/build-client
```

The script requires an exact `v26.9` tag at `HEAD`, configures `WIVRN_BUILD_CLIENT=ON` and
`WIVRN_BUILD_SERVER=OFF`, builds the `wivrn` target, and writes its complete configure/build transcript to a UTC-stamped file under `~/vr-realclient/logs/`. The previously
attempted build stopped during Ninja compilation and its detailed diagnostic was not retrieved; inspect the earlier
`$HOME/vr-realclient/logs/client-build.log` first if it is still present, then compare it with the new attempt.

The expected non-Android executable is `build-client/bin/wivrn` per the v26.9 CMake configuration. Confirm the binary
exists before proceeding; do not treat a successful configure as a successful build.

## Start the host and client

On the Mac, from the repository checkout, run the existing host build. Use a fresh test pairing and retain the terminal
output as the host log:

```sh
mkdir -p ~/vrc-mac/audit/vr/realclient-2026-10-12
build/vrhost/steamac-vrhost --testpattern --pair --size 1440x1584 \
  2>&1 | tee ~/vrc-mac/audit/vr/realclient-2026-10-12/host-realclient.log
```

The chosen size reduces encoder load; omit `--size` only when specifically testing the headset-requested default. The
host prints its listening port, protocol hash, and pairing PIN on startup. Keep the PIN private and do not put it in a
committed log. If retaining logs with the PIN, store them only in the private audit directory and redact the PIN before
sharing. To check an existing pairing, use the host's `--list-headsets`; do not reset the pairing store as part of this
test.

On Fedora, from the active graphical login session, launch the client:

```sh
~/vr-realclient/scripts/run-client.sh ~/vr-realclient/build-client/bin/wivrn
```

Use the client UI to select the Mac's advertised `steamac-vrhost` entry, or enter the Mac's LAN IP if discovery is not
available. Do not assume the GUI accepts a URI or that an autoconnect variable is supported; neither has been validated
for this Linux v26.9 client. Enter the PIN shown by the Mac at the client prompt. The script saves client output in a
UTC-stamped file under `~/vr-realclient/logs/`. If Monado needs an explicit runtime JSON, set `XR_RUNTIME_JSON` to the
verified runtime manifest before launching; the helper passes it through but does not choose a runtime or driver.

For reproducible evidence, record the Mac LAN address used, source tag and commit, client executable path, Monado/runtime
versions and how the simulated HMD was selected, firewall state, and the exact client/host commands. Capture client logs
under `~/vr-realclient/logs/`; retain sanitized copies of both logs in the private audit directory. No debugger, tracing
or instrumentation is needed.

## Checks and pass criteria

Observe the client and the host logs together. Mark each item PASS/FAIL/NOT RUN with timestamps and evidence; do not infer
a real-client pass from the existing fake-headset loopback test.

1. **Discovery / pairing:** the client shows the Mac host (or connects by manual IP); the v26.9 client accepts the PIN;
   host reports the headset connected and prints its headset name, per-eye size and refresh rate.
2. **Tracking and timesync:** host's periodic five-second report shows `tracking` and `feedback` counts increasing; client
   remains connected without protocol/version errors. Confirm time synchronization through ongoing feedback/logs where
   available. The host's report is aggregate rather than a per-packet trace.
3. **Video:** the grid and colored sectors render in the client; host `frames` and `idr` increase; no persistent decode
   or stream errors appear in the client. Verify the grid/frame content visually rather than treating sent-frame counts
   as proof of decode.
4. **World lock:** with the simulated headset, rotate the view left/right and up/down. The red forward marker and grid
   should stay fixed in room coordinates rather than follow the view. The pattern is rotation-locked; it is at infinity,
   so translation is not expected to produce parallax.
5. **Stability:** leave the stream connected for at least two minutes, note disconnects, stalls, decode errors and reported
   encode/decode timings. Repeat once after disconnect/reconnect to check the paired reconnect path.

Only if the client reports a concrete mismatch should it be treated as a candidate host defect. Preserve the relevant
logs and minimal reproduction, add a regression test that reproduces the real-client failure where practical, and run the
host's full CTest suite before proposing a fix. A mismatch must be attributable to the host rather than client build,
Monado setup, firewall, discovery or runtime selection.

## Containerised harness (x86, verified 2026-10-12)

`scripts/vr-realclient/run-in-container.sh` runs Monado (`SIMULATED_ENABLE`, simulated HMD) and the v26.9 client in the
Fedora 44 Podman image from `build-client.sh`'s Containerfile (add `spirv-tools`, `iproute-tc`, `nftables`; the client
build also needs `-DGIT_TAG=v26.9` inside a container because git refuses the mounted checkout).
The Linux client has no pairing UI that a headless run can drive, so `wivrn-test-uri.patch` (test only, never shipped)
makes it connect once to `wivrn://:PIN@host:port` taken from `WIVRN_TEST_URI`.

* Software Vulkan (lavapipe) crashes in the client's timestamp queries. Use the GPU: `--device /dev/dri`, radv ICD, and the
  host Wayland socket for Monado's compositor (`COMPOSITOR=WAYLAND`); radv under Xvfb fails swapchain acquisition.
* `--network pasta --cap-add NET_ADMIN` gives the container its own netns, so `LOSS=<percent>` can drop ingress UDP from the
  host (nftables `numgen random`) to exercise IDR recovery.
* Run the host with `steamac-vrhost --testpattern --test-tone --pair --pin 123456`, then
  `run-in-container.sh <mac-ipv6> 9757 123456 <seconds>`. IPv6 (`2001:...::1001`) is reachable both ways; IPv4 ICMP between
  the two machines is filtered, so use IPv6.
  The container, from the directory holding `run-in-container.sh` and `build-client/` (radv on the host GPU, the
  host Wayland socket for Monado's compositor, host networking):

  ```sh
  podman run --rm --network host --device /dev/dri --security-opt label=disable \
    -e COMPOSITOR=WAYLAND -e WAYLAND_DISPLAY=wayland-0 -e XDG_RUNTIME_DIR=/tmp/xdg \
    -e VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.x86_64.json \
    -v /run/user/$(id -u)/wayland-0:/tmp/xdg/wayland-0 -v "$PWD":/work -w /work \
    localhost/vr-realclient bash /work/run-in-container.sh <mac-ipv6> <port> <pin> <seconds>
  ```

  Checked 2026-10-12 against the demo bundle's `steamac-vrhost --testpattern --pair --pin <pin> --port 9761
  --no-advertise`: about 60 frames/s, encode about 9 ms, headset decode about 1.6 ms, tracking flowing.

### Results against vrhost (protocol `fc26e619b7ab80f8`, ctest 10/10)

| Check | Result |
| --- | --- |
| Handshake, protocol hash | ok; `Monado: Simulated HMD`, 536x604 per eye, 60 Hz, HEVC |
| PIN pairing, key persisted, AES-128-CTR | ok (wrong PIN not tried; paired key stored in known_keys) |
| Video decode | ok: 60 fps, `headset decode` 1-3 ms, 7-46 Mbit/s as the client's bitrate request ramps |
| Tracking | ok: ~10k packets per 10 s |
| IDR recovery | with 3 % ingress UDP loss the stream recovers: every lost frame is answered by exactly one IDR (355 requests, 365 IDRs in 35 s); no stall |
| Reconnect | repeated connects from fresh client keys pair and stream |

Limits: the desktop client has no audio backend (AAudio is Android-only), so game audio cannot be received by this stand-in;
the bridge is covered by `test_audio` and `--test-tone` only shows host-side packet counts. The simulated HMD's controllers
do not change state, so button input is covered by the fake-headset tests, not here. With sustained loss the client's
bitrate request keeps rising (101 Mbit/s at 3 % loss), which is the client's adaptation, not a host fault.
