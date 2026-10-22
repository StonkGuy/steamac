# VR (experimental)

Play a game in VR on a standalone headset streamed from the Mac. The Mac runs `steamac-vrhost`, a WiVRn-compatible
server; the headset runs the stock **WiVRn v26.9** client. Inside SteamOS, the game talks to this fork's OpenVR runtime
through a patched Proton `vrclient_x64`, and its eye images go over the DXMT path to the Mac, where they are encoded
with VideoToolbox and streamed.

**Status:** experimental, off by default, on this branch only. Verified on one Mac (M2 MacBook Air, 16 GB, macOS 27)
with a Meta Quest 3 over Wi-Fi: VRChat passes EAC (`301`), enters a world in VR, head tracking is world-locked, the
controllers and hand menu work, game audio plays in the headset (2026-10-17). The reconnect behaviour below was
verified with a synthetic Quest 3 client in the VM, not yet with the headset.

## What it takes

| you need | notes |
|---|---|
| the DXMT graphics path | VR is available only with Settings > Advanced > Graphics path *DXMT* |
| the patched FEX and VRChat's launch options | as for flat play: see `fex-eac/README.md` |
| a headset with the **WiVRn v26.9** client | Meta Quest 3 verified. Quest 2/3S/Pro and Pico use the same client but are untested. Other WiVRn versions cannot connect |
| Mac and headset on the same network | allow `steamac-vrhost` in System Settings > Privacy & Security > Local Network when macOS asks |
| little else running on the Mac | VRChat in VR peaked at about 18 GiB of VM footprint on a 16 GB Mac (8 GiB guest) |

## Setup

1. Install WiVRn on the headset: `WiVRn-release.apk` from the [v26.9 release](https://github.com/WiVRn/WiVRn/releases/tag/v26.9)
   with `adb install` (developer mode). If the Store version is also installed, turn off its automatic updates: a newer
   client will not connect.
2. In the launcher, Settings > VR: turn on **VR (experimental)**, then start (or restart) the VM. The launcher starts
   `steamac-vrhost` with it.
3. Start WiVRn on the headset. The Mac appears in its server list (or add it by IP address, port 9757).

### Pairing

The first connection of a headset asks for a six-digit PIN. With **Let new headsets ask to pair** (default on), a new
headset opens pairing by itself; the PIN is shown in Settings > VR and in the VM window's title. Type it into WiVRn on
the headset. The PIN never crosses the network (WiVRn's SMP check), and the link is encrypted (AES-128). Paired
headsets reconnect without a PIN. With the option off, new headsets are refused and the panel offers **Pair this
headset**. **Unpair** removes a headset (entries of the same name are grouped).

### Starting a game

**Start in VR** (Settings > VR > Game) starts the game's VR mode in the guest (`steam://launch/<id>/vr`); the preset
Steam app ID is VRChat's (438100). The same game is listed in WiVRn's app list on the headset, and picking it there
starts it the same way. Starting it again while it runs does nothing.

## VR panel options

Settings > VR. Options apply at the next VM start unless noted. Refresh rate, resolution scale and microphone are set
on the headset in the WiVRn app, as in WiVRn; the panel shows them read-only.

| option | what it does | default |
|---|---|---|
| Preset | Low / Medium / High / Ultra set bitrate (30 / 40 / 50 / 80 Mbit/s) and sharpening and keep the headset's stream size; Sharp is Ultra plus a 1248x1344 stream per eye (next start). Editing any of them makes it Custom | High |

Stream size and sharpness: the Quest 3 asks WiVRn for half its render size per axis (918x962 of 1836x1924).
WiVRn's foveation keeps the centre of the view at full resolution and compresses towards the edges, at that size
up to 6.4 source pixels per streamed pixel per axis at the edge, which reads as a soft periphery. Sharp streams
about 0.7 per axis (edge 2.7:1). On an M2, VideoToolbox HEVC needs (both eyes, p50 per frame, test pattern): 6.9 ms
at 918x962, 9.9 ms at 1280x1344, 12-15 ms at 1376x1442, 20 ms at the full 1836x1924; the 72 Hz budget is 13.9 ms.
Encoding runs on the media engine, not the GPU the game uses.
| Render resolution | 50-150 % of the headset's render size; below 100 % the Mac upscales before encoding (Upscaler: MetalFX spatial or bilinear) | 100 % |
| Sharpening | sharpening applied before encoding | off (Low and Medium add some) |
| Treat game images as sRGB | for games whose 8-bit images hold sRGB data without the sRGB format (colours look washed out) | off |
| Codec | Automatic (HEVC when the headset supports it), HEVC or H.264; hardware encoder | Automatic |
| Bitrate | total stream bitrate; applies at once to a running host (the headset can still lower it) | 50 Mbit/s |
| Keyframe interval | periodic key frames; by default a key frame is sent only when the headset reports a loss | when needed |
| Resolution per eye | stream size per eye | the headset's request |
| TCP only | stream over TCP instead of UDP | off |
| Steam app ID | the game for Start in VR and the headset's app list | 438100 |
| Game audio in the headset | the guest's sound plays in the headset and the headset microphone goes to the guest | on |
| Show live statistics | game fps, copy / compose / encode / decode times, bitrate, lost frames | off |

The panel also shows headset connection, battery, codec and encryption. Without a running host it can still list and
remove paired headsets.

## Headset connect, disconnect and reconnect

- The game can start before the headset connects: it sees a present headset in standby with a Quest 3 view, and
  frames start when the headset connects.
- When the headset disconnects (taken off and asleep, out of range, WiVRn closed), the game keeps running.
- When it reconnects, frames resume within a few seconds. A reconnect with a different stream size or refresh rate is
  accepted; the game keeps its render size until it restarts.

## Performance and limits

Measured on the M2 MacBook Air (fanless), VRChat, synthetic Quest 3 client and live headset sessions. Scope: VRChat
only.

| measurement | result |
|---|---|
| frame rate, cool start | about 20 fps, game-submit to sent about 111 ms (p50) |
| frame rate after 2-3 minutes | 14-16 fps, about 130-140 ms, then steady (thermal throttling, see below) |
| encode, both eyes, 1440x1584 at 72 Hz | 12.1 ms p50 (fits the 13.9 ms frame budget) |
| exporter copy and send per frame | about 1.3 ms |

- **Thermal throttling on fanless Macs.** In VR the GPU runs at 99-100 % from about minute 3 (flat play of the same
  world stays at 64-83 %). A fanless Mac reaches macOS's "fair" thermal state after about 2 minutes and frame rate drops
  from about 20 to 14-16 fps. Cooling the Mac (a stand, a fan) keeps the higher rate longer. Macs with fans are untested.
- The game, not the link, sets the frame rate: the stream path's own cost stays about 1 ms per frame.
- Quest 3's default stream size at 90 Hz does not fit the hardware encoder (about 20 ms per eye pair against an 11.1 ms
  budget); 72 Hz at about 1440x1584 per eye does. Set the refresh rate on the headset.
- WiVRn v26.9 clients only.
- Controller render models are not provided (VRChat draws its own hands; only SteamVR-style overlays would show them).
- One game verified (VRChat). Other OpenVR games are untested.

## How the host works

* `steamac-vrhost` speaks the **WiVRn v26.9** protocol (hash `fc26e619b7ab80f8`), using WiVRn's own protocol,
  serialization and socket code, vendored unmodified (`host/vrhost/third_party/wivrn/UPSTREAM.md`).
* Bonjour advertisement `_wivrn._tcp` on port 9757 with WiVRn's TXT keys (`protocol`, `version`, `cookie`), so the
  headset's server list shows the Mac.
* Handshake: version check (an incompatible client gets WiVRn's `incompatible_version` answer), control on TCP and
  stream on UDP, headset info.
* **Encryption and PIN pairing** as in WiVRn, with WiVRn's own code: X448 key exchange, the PIN checked by SMP (the
  PIN never crosses the network), keys derived with PBKDF2, AES-128-CTR on both the TCP control and UDP stream
  sockets. Paired headsets are remembered in `~/Library/Application Support/steamac/vrhost/known_keys` and reconnect
  without a PIN. Same modes as WiVRn: pairing (default while no headset is paired, or with `--pair`), encryption on
  with pairing off (default once a headset is paired: unknown headsets get `pairing_disabled`), `--no-encryption`.
* **Pacing: WiVRn's pacer** (`server/compositor/pacer.cpp`, unmodified): the headset's feedback packets (decode,
  blit and display times, converted with the timesync clock offset) set how early a frame is started (99.5th
  percentile of present-to-decoded + 1 ms) and the predicted display time, and phase-lock frames to the headset's
  vsync. `next_frame_timing()` returns the frame id the pacer assigned (`FrameTiming::frame_id`).
* Timesync (WiVRn's clock regression), `tracking_control`, head/controller pose history with interpolation and bounded
  extrapolation, per-eye poses from the headset's view offsets.
* VideoToolbox **HEVC** (H.264 fallback), hardware encoder required, low-latency rate control, no frame reordering,
  infinite GOP, BT.709 full range. Annex-B with VPS/SPS/PPS before every IDR, IDR on request and on reported loss
  (WiVRn's IDR state machine), IDR frames on TCP like WiVRn's encoders, 1400-byte shards with `view_info` on the first
  shard and timing on the last, identity foveation.
* Test pattern: a sky sphere with a 15° grid, coloured 45° sectors, a yellow horizon and a red marker straight ahead,
  rendered by a Metal compute kernel for the pose the frame is sent with, so it stays **world-locked** while the head
  turns (rotation; the sphere is at infinity, so walking does not move it). The frame number is encoded in the top-left

## Licence

`host/vrhost/` is **GPL-3.0-or-later** because it contains WiVRn code. It builds a separate executable; `steamac-vm`
(Apache-2.0) does not link it. They will talk over the IPC described in `docs/vr-interfaces.md`.

## Build and test (no headset)

Requires Xcode command-line tools, CMake ≥ 3.25 and Ninja (`brew install cmake ninja`). OpenSSL is **not** taken
from the system: the build downloads the pinned OpenSSL 3.5.4 release tarball (SHA-256 checked), builds a static
`libcrypto` (no shared libs, no TLS, no apps; about 2 minutes once) into the build directory and links it, so
`steamac-vrhost` loads only macOS system libraries. The binary is signed ad hoc with the identifier
`com.steamac.vrhost` after linking (stable identity for the Local Network permission). `-DVRHOST_SYSTEM_OPENSSL=ON`
uses `find_package(OpenSSL)` instead, for development only.

```sh
cmake -S host/vrhost -B build/vrhost -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build/vrhost
ctest --test-dir build/vrhost --output-on-failure
build/vrhost/steamac-vrhost --selftest-encoder          # headless VideoToolbox latency report
```

| test | checks |
|---|---|
| `protocol` | the vendored tree hashes to the pinned WiVRn v26.9 value; a first shard with `view_info` round-trips |
| `annexb` | length-prefixed → Annex-B, malformed input, start-code splitting, HEVC NAL types |
| `pose_history` | interpolation, angular/linear extrapolation and its 100 ms bound, pose composition |
| `encoder` | hardware + low-latency session, VPS/SPS/PPS only before IDRs, IDR on request, decode back (VideoToolbox) to the same frame number; HEVC and H.264 |
| `pacer` | WiVRn's pacer with synthetic feedback (headset clock 5 s ahead): learns decode time + margin and blit-to-display, phase-locks display times to the headset vsync grid, ignores stale feedback |
| `crypto` | both peers derive the same AES keys (X448 + PBKDF2), SMP accepts the right PIN and rejects a wrong one, key PEM round trip, paired-headset store (replace, remove, mode 0600) |
| `self_contained` | `otool -L steamac-vrhost` lists only `/usr/lib` and `/System` libraries |
| `loopback` | the full server against a test headset that follows the WiVRn client's connection sequence over loopback: handshake, stream description, tracking control, timesync, UDP shards, decode of 60+ frames per eye with matching frame numbers, poses 64 mm apart, display time in the headset clock, a lost frame → new IDR → decoding resumes, wrong protocol refused; pacing: with synthetic headset feedback (8 ms decode, 72 Hz vsync at a 3.3 ms phase) the announced display times land on the headset's vsync grid; encryption: PIN pairing then encrypted stream (lost frame + IDR included), reconnect of the paired headset without PIN, wrong PIN refused and not stored, pairing disabled refuses an unknown headset |

Encoder self-test on an M2 MacBook Air (16 GB, macOS 27), 2 eyes encoded concurrently, measured while a steamac VM was
running (2026-10-11):

| per-eye size, rate | encode per eye p50 / p95 (ms) | both eyes p50 / p95 (ms) | frame budget (ms) |
|---|---|---|---|
| 1832×1920, 90 Hz (Quest 3 default) | 18.3 / 20.8 | 20.2 / 21.1 | 11.1 |
| 1440×1584, 72 Hz | 10.6 / 12.3 | 12.1 / 12.4 | 13.9 |
| 1280×1408, 72 Hz | 9.0 / 10.2 | 10.1 / 10.3 | 13.9 |

Encoder tuning knobs (`--selftest-encoder`): `--layout eyes|serial|sbs` (one session per eye concurrently, one after
the other, or one side-by-side 2W×H session), `--prioritize-speed`, `--max-frame-delay N`, `--qp N` (constant QP via
Min/MaxAllowedFrameQP), `--no-low-latency-rc`, `--codec h264`, `--csv` for one summary line per run. First finding:
the low-latency HEVC encoder **rejects** `PrioritizeEncodingSpeedOverQuality` and `MaxFrameDelayCount` (the
self-test lists rejected properties); the full sweep is still to be measured on an idle machine.

The two VideoToolbox sessions share one media engine (about 350 Mpixel/s of HEVC here), so encoding both eyes in
parallel saves little. Quest 3's default stream size at 90 Hz does not fit; 72 Hz at about 1440×1584 per eye does. The
stream size is the headset's request; `--size WxH` overrides it.


## Testing with a headset

The client must be **WiVRn v26.9** exactly: WiVRn refuses any other protocol version (the headset greys the server
out). Mac and headset on the same LAN; allow `steamac-vrhost` in System Settings → Privacy & Security → Local Network
when macOS asks.

```sh
build/vrhost/steamac-vrhost --testpattern            # add --size 1440x1584 to lower the stream size
build/vrhost/steamac-vrhost --show-pin               # the PIN, while the host waits for a new headset
build/vrhost/steamac-vrhost --list-headsets          # paired headsets; --unpair NAME / --unpair-all
```

The first connection of a headset asks for the 6-digit PIN printed by the host (`--pin NNNNNN` fixes it, `--pair`
accepts new headsets when one is already paired).

* **Meta Quest 2/3/3S/Pro, Pico 4/Neo 3:** install `WiVRn-release.apk` from the
  [v26.9 release](https://github.com/WiVRn/WiVRn/releases/tag/v26.9) with `adb install` (developer mode), and turn off
  automatic updates for WiVRn if you also have the Store version: a newer client will not connect. Start WiVRn on the
  headset; the Mac appears in its server list (or add it by IP, port 9757).
* **Steam Frame:** WiVRn's Linux client runs on the Frame (Linux arm64). There is no prebuilt client in the v26.9
  release, so build the client from the `v26.9` tag (`-DWIVRN_BUILD_CLIENT=ON -DWIVRN_BUILD_SERVER=OFF`), on the Frame
  or in an arm64 Linux build environment, and connect to the Mac by IP.

What to check: the pattern appears; the red marker stays at the same place in the room while you turn your head; the
`steamac-vrhost` log prints frames, feedback and tracking counts every 5 s, the last encode time and the headset's
decode time.
