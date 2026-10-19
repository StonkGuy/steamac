# VR internal interfaces (steamac-vm ⇄ steamac-vrhost ⇄ headset adapter)

Status: experimental. This file is the contract between the VR milestones being built in parallel:

* **M0** host: `host/vrhost/` — `steamac-vrhost` process, WiVRn adapter, encoder (owner: M0 agent).
* **M1** DXMT eye-texture export over the wmtr ring + host Metal compositor feeding the M0 encoder.
* **M2/M3** guest OpenVR runtime + Proton `vrclient_x64` DXMT submit + input/poses.

Change it by editing this file; add a line to the change log at the bottom (who, what). Anything marked
**[decided]** is implemented or being implemented against; **[proposed]** may still change.

## 1. Processes and licences [decided]

```
 guest (Linux, x86-64 under FEX)            steamac-vm (Apache-2.0)                  steamac-vrhost (GPL-3.0-or-later)
 VRChat → openvr_api → vrclient_x64 ─wmtr ring─▶ wmtr executor (DXMT/Metal) ─VR link─▶ VrSession ─adapter─▶ headset
 (M2/M3)                                (M1: eye textures, fence)                   (M0: WiVRn adapter, later ALVR)
```

* `steamac-vrhost` is a **separate executable**. It may contain WiVRn (GPL-3) code; `steamac-vm` must never link it.
  The only coupling is the IPC in section 3 (documented here, no shared code beyond a header of plain C structs,
  `host/vrhost/include/vrlink.h`, which is dual-licensed Apache-2.0 OR GPL-3.0-or-later so both sides can include it).
* `steamac-vm` launches `steamac-vrhost` as a child (`posix_spawn`), and kills it with the VM. **[implemented, vr-int]**
  `host/launcher/Sources/steamac-vm/VRHost.swift`: Settings > Advanced "VR (experimental)" / `--vr` (default off; DXMT
  path only), executable `Contents/MacOS/steamac-vrhost`, log `~/Library/Logs/es.fxgam.steamac/steamac-vrhost.log`.
  The host stops when its link fd reaches end-of-file: steamac-vm's end is `FD_CLOEXEC` and nothing else holds it, so
  libkrun's `_exit()` at VM stop ends the host without extra code; `VRHost.stop()` (close, then SIGTERM, SIGKILL) is
  for the launcher's own exit paths. `--selftest-vr` checks spawn, `VRL_HELLO` and teardown.

## 2. VrSession — inside steamac-vrhost [decided, M0, implemented]

C++ interface: `host/vrhost/src/session/vr_session.h` (the header is the source of truth; summary here). The wire
protocol is an adapter behind it (`WivrnSession` in `src/wivrn/`, an `AlvrSession` could follow). Nothing outside
`src/wivrn/` includes WiVRn headers.

| member | meaning |
|---|---|
| `start(SessionListener*)` / `stop()` | advertise (`_wivrn._tcp`) and accept one headset at a time; background threads |
| `connected()`, `headset()` | `HeadsetInfo`: stream size per eye, render size, refresh rates, FOV, codecs, name |
| `next_frame_timing()` | blocks until the next frame should start; returns `{wake_up, predicted_display, frame_id}` (host clock). **Changed (M0, vr-m0 after 1920a53):** WiVRn's pacer (feedback-driven, phase-locked to the headset vsync) replaces the fixed period; `frame_id` is the pacer's id and **must** be used as `Frame::frame_id` in `submit()` (the headset's feedback is matched by it). For `VRL_TIMING`, send this triple |
| `views_at(t, pose[2], fov[2])` | per-eye pose (head pose at `t` × eye offset) and FOV in tracking space — what an OpenVR `WaitGetPoses` returns |
| `pose_at(Device, t)` | head / grip / aim poses, interpolated or extrapolated (≤100 ms) from the tracking history |
| `submit(Frame)` | `frame_id`, `display_time`, `pose[2]`, `fov[2]`, `eye[2].surface`, `foveation[2]` (WiVRn `foveation_parameter` x/y the compositor applied; empty = identity; sent in `view_info`, vr-int); encodes both eyes and sends; the surfaces may be reused when it returns |
| `haptics(Device, duration, frequency, amplitude)` | to the headset |
| `request_idr()` | force an IDR on both streams |
| `SessionListener` | `on_connected`, `on_disconnected`, `on_tracking(TrackedPose[])`, `on_input(InputValue[])`, `on_refresh_rate` |

**Encoder input format [decided].** `EyeImage.surface` is an IOSurface in `kCVPixelFormatType_420YpCbCr8BiPlanarFullRange`
(`'420f'`, NV12, BT.709 **full range**: what WiVRn's server writes in `foveation.comp` and what its clients' samplers
assume) of exactly the stream size from `HeadsetInfo`. Converting the application's RGBA eye texture (sRGB/linear,
sub-rect, flip, MSAA) into this is the **M1 compositor's** job: one Metal compute pass per eye, the MSL port of WiVRn's
`foveation.comp` (M0's `src/pattern/test_pattern.mm` shows the plane mapping: `CVMetalTextureCache`, plane 0 as
`R8Unorm`, plane 1 as `RG8Unorm`).

**Poses.** `Frame.pose` is the exact per-eye pose the image was rendered with, in the headset's tracking space; the
client reprojects against it. Use `views_at(predicted_display)` to get it.

**Timing.** All `ns` values are the host clock: `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` (`src/session/clock.h`); the
adapter converts to the headset clock (WiVRn timesync).

**Input ids.** `InputValue.id` currently carries WiVRn's `device_id` value (OpenXR path table in WiVRn
`common/wivrn_packets.h`). M2/M3 owns the adapter-neutral table; change it here when defined.

## 3. VR link: steamac-vm → steamac-vrhost [decided, M1 implemented the frame path]

Header: `host/vrhost/include/vrlink.h` (plain C, `Apache-2.0 OR GPL-3.0-or-later`; drafted by M1 on branch `vr-m1`,
M0/M2/M3 add their message structs there). Two channels, both set up when `steamac-vm` spawns the host:

1. **Control/events: AF_UNIX `SOCK_DGRAM` socketpair**, fd passed as `--link-fd=N`. **Changed from SOCK_SEQPACKET:
   macOS has no AF_UNIX SOCK_SEQPACKET** (`socketpair` fails with EPROTONOSUPPORT). Datagrams on a local socketpair are
   reliable, ordered and keep message boundaries; each message must stay under `net.local.dgram.maxdgram` (2048 B).
   One fixed-size struct per datagram (`struct vrl_header {uint32_t type, size;}` first). For tests and manual runs,
   `wmtr_vr_connect(path)` / `VrLinkServer::accept_one(path)` connect over a SOCK_STREAM path and hand one end of the
   DGRAM pair over by SCM_RIGHTS. Both ends send `VRL_HELLO {version, pid}` first.
2. **GPU objects: Mach ports, IOSurfaces only.** The host registers a receive right under `--mach-name=...`
   (`bootstrap_register2`, as DXMT's `WMTBootstrapRegister`); `steamac-vm` looks it up and sends one
   `struct vrl_mach_swapchain` (msgh_id `VRL_MACH_SWAPCHAIN_ID`, `IOSurfaceCreateMachPort` rights for up to 4 images)
   per swapchain, **before** the matching `VRL_SWAPCHAIN` datagram; the host receives the ports when it reads that
   datagram. **No MTLSharedEvent crosses**: `steamac-vm` sends `VRL_SUBMIT` from the copy's Metal completion handler,
   so the named images are complete when the datagram arrives (no private `newSharedEventWithMachPort` SPI, and a
   stalled guest can never stall the host GPU). Cost: one CPU wake-up per frame (measured below).

| dir | message (vrlink.h) | fields |
|---|---|---|
| vm→host | `VRL_SWAPCHAIN` `vrl_swapchain` | id (new id on every recreate), image_count (3 per eye now), w, h, MTLPixelFormat, `VRL_SWAPCHAIN_SRGB` |
| vm→host | `VRL_SUBMIT` `vrl_submit` | frame_id, display_time, app_submit_time, ready_time, per eye {swapchain, image, x,y,w,h, `VRL_EYE_FLIP_Y`}, pose[2][7] (xyzw, xyz, stage), fov[2][4] (l,r,u,d rad) |
| host→vm | `VRL_RELEASE` `vrl_release` | swapchain, image, frame_id, release_time — per eye, after the compositor pass (image reusable) |
| host→vm | `VRL_TIMING` `vrl_timing` | once per frame while a headset is connected: the pacer's frame_id, wake_up, predicted_display, refresh_hz (section 4) |
| host→vm | `VRL_POSES` `vrl_poses` | once per frame, sent just before `VRL_TIMING`: all devices predicted to that frame's display time |
| host→vm | `VRL_INPUT` `vrl_input` | on change: the whole `value[2][21]` table |
| vm→host | `VRL_HAPTICS` `vrl_haptics` | hand, duration_s, frequency, amplitude → `VrSession::haptics` |
| host→vm | `VRL_STATE` `vrl_state` | on headset connect / disconnect / refresh-rate change |

Both ends set `SO_RCVBUF` to `VRL_SOCKET_BUFFER` (256 KiB; the macOS default for AF_UNIX datagrams is 4 KiB and fills
in a few frames). Host sends are non-blocking, so a stalled vm side loses timing packets instead of stalling the frame
clock; the vm side reads the link on its own thread (`wmtr-vr-link`), so per-frame packets never queue in front of
`VRL_RELEASE`. A datagram socketpair gives no end-of-file on macOS: the host detects a closed vm end by `ENOTCONN`.

Answers to the M1 open questions: DXMT eye textures are **not** IOSurface-backed (private render targets), so the vm
side copies each eye into its own IOSurface swapchain image (one blit per eye; not separately timed, included in "submit → copy complete" below). MSAA
sources are rejected (OpenVR requires resolved textures; DXMT resolves when the game calls ResolveSubresource).
Array textures: `WMTVREye.slice` selects the slice; the blit copies it (single-pass stereo works). Format: the
swapchain keeps the eye texture's MTLPixelFormat (RGBA8/BGRA8 (+_sRGB), RGBA16Float); others are refused and counted.

### 3.1 M1 pieces (implemented)

* **DXMT** (StonkGuy/dxmt branch `vr-export`): `IMTLD3D11ExportableTexture` (uuid `ad6245d1-3a11-4dd6-87b7-7a58048b55d3`,
  QueryInterface on any `ID3D11Texture2D`): `GetMetalTexture(UINT64 *texture, UINT64 *shared_event, UINT64 *value)`
  flushes the immediate context and returns the MTLTexture handle and the DXMT queue's MTLSharedEvent + value covering
  all work submitted so far. New winemetal export `uint32_t WMTVRSubmit(const struct WMTVRSubmitInfo *)` = **unix call
  146** (async): `{frame_id, display_time, eye[2] {texture, x,y,w,h (w=0: whole), flags WMTVR_EYE_FLIP_Y|WMTVR_EYE_SRGB, slice},
  pose[2][7], fov[2][4], wait_event, wait_value}`; plain DXMT returns STATUS_NOT_SUPPORTED. **M2's vrclient path**: for
  each eye `QueryInterface(IID_IMTLD3D11ExportableTexture)` → `GetMetalTexture` → fill `WMTVRSubmitInfo` (wait_event/value
  from the last call) → `WMTVRSubmit` (GetProcAddress on winemetal.dll). Example: wmtr `tests/d3d11bench.cpp --vr W`.
* **wmtr** (steamac branch `vr-m1`): `WMTR_NCALLS` = 147; call 146 is executed by `wmtr_vr_submit` (`wmtr/src/wmtr_vr.m`,
  `include/wmtr_vr.h`), never blocks the executor: GPU waits for `wait_event`, blits into a free swapchain image, sends
  `VRL_SUBMIT` on completion; no free image → frame dropped (counted). Attach: `wmtr_host_set_vr(h, wmtr_vr_create(fd,
  mach_name))` from the embedder, or env `WMTR_VR_SOCKET=<path>` + `WMTR_VR_MACH_NAME=<name>` at `wmtr_host_create`.
  **[implemented, vr-int]** the launcher's path is env `WMTR_VR_FD=<n>` + `WMTR_VR_MACH_NAME` (libkrun loads
  libwmtr_host into the steamac-vm process, so the fd number is valid there): the first wmtr context claims a `dup` of
  it, later contexts get STATUS_NOT_SUPPORTED for call 146 until the owner ends; the launcher's end stays open, so the
  host's headset session survives a game restart. Test: `wmtr_loopback` "vr fd attach".
* **Texture lifetime [decided, vr-int].** The exporter copies on its own Metal queue after `wait_event`; Metal does
  not order that against the producer's *later* work on DXMT's queue. A texture passed to `WMTVRSubmit` must not be
  written again before the copy ran: the producer renders (or `CopyResource`s) each eye into a ring of **3** textures
  and submits the frame's slot (`d3d11bench --vr` renders into the ring; **M2's `vrclient_x64` must copy the game's
  eye texture into such a ring** before `GetMetalTexture`, since the game reuses its texture next frame, which is what
  an OpenVR runtime's Submit copy guarantees on Windows). Without it `vr_e2e` decoded frame N carrying frame N+1.
* **steamac-vrhost** (`host/vrhost/src/compositor/`, `compositor.cmake`): `VrLinkServer(fd, mach_name, sink)` receives
  swapchains/submits and releases images after the sink returns; `CompositorBridge(out_w, out_h, EyeEncoderSink&)` runs
  `EyeCompositor` per frame: Metal compute port of WiVRn's `foveation.comp` (same per-pixel source ranges from a port of
  WiVRn's `foveation.cpp`: `solve_foveation`/`fill_param_2d`/`fill_ubo`, no-gaze centre = 1 m convergence, 10° down),
  sRGB encode + BT.709 full range → the encoder's `'420f'` CVPixelBuffer (Y + CbCr planes via CVMetalTextureCache).
  `EyeEncoderSink { acquire(eye) → CVPixelBufferRef; encode(eye, buf, LinkFrame, FoveationParams) }` — M0 adapts
  `VtEncoder` (`acquire` = `pool()`, `encode` = `VtEncoder::encode`) and sends `FoveationParams` (WiVRn
  `foveation_parameter` x/y vectors) with the stream. Output size = the stream eye size from `HeadsetInfo`.

Measured (M2 Mac, `host/vrhost/tests/m1_pipeline_test`, both link ends in one process, another VM running on the
GPU): 2064x2208 → 1248x1344 per eye at 90 Hz, 300 frames, 0 dropped: guest submit → copy complete p50 2.6 ms (includes
the producer's own render it waits for), → host receive p50 0.015 ms, → encoder input p50 2.0 ms (compositor GPU
1.7 ms for both eyes), total p50 4.6 / p99 8.9 ms.

### 3.2 Integrated frame path (vr-int)

`steamac-vrhost --link-fd N --mach-name NAME` (spawned by steamac-vm; `--link-socket PATH` for manual runs with
`WMTR_VR_SOCKET`) runs `WivrnSession` + `VrPipeline` (`src/pipeline/`): `VrLinkServer` → `CompositorBridge` →
`SessionEncoderSink` (`EyeEncoderSink` over `VrSession`: `'420f'` IOSurface pool of the stream size, both eyes →
`VrSession::submit` with the link frame's id/display time/poses/FOVs and the foveation parameters). Frames arriving
while no headset is connected are released without work. PIN pairing as in `--testpattern`. Every 5 s the log prints
per-stage latency p50/p99.

* **Frame ids:** the pipeline passes the guest's `frame_id` through. WiVRn's pacer learns only from frames submitted
  with its own id (`FrameTiming::frame_id`); the guest gets it with `VRL_TIMING` (section 4) — until M0 sends that
  packet, game frames stream with the guest's ids and the pacer runs on its defaults.
* **Tests:** `vr_e2e` (ctest, `-DVRHOST_DXMT_DIR=<dxmt vr-export>`): producer (two sRGB eye textures, ring of 3, frame
  number blocks, left red / right green, session frame ids and `views_at` poses) → `wmtr_vr_submit` → link →
  `VrPipeline` → `WivrnSession` (PIN pairing, AES-128-CTR) → loopback test headset: frame numbers, eye colours, eye
  poses, identity and foveated (WiVRn-consistent parameters received) streams, link-close teardown.
  `vrhost-test-headset` is the same headset as a tool for a running host on the Mac.

## 4. Guest side (M2/M3) [implemented by M2/M3 on branch `vr-m23`, not yet live-tested]

Guest pieces: Proton `vrclient_x64` patch (`guest/proton-vrclient/patches`, LGPL) and our OpenVR runtime
`vrclient.so` (`guest/vr`, Apache-2.0; x86-64 Linux, loaded by Proton's unix side under FEX). Overview and tests:
`docs/vr-guest.md`. Guest wire protocol: `guest/vr/include/steamac_vr_guest.h` (dual-licensed like vrlink.h).

* **Submit [decided, uses M1's call 146].** `vrclient_x64.dll`: D3D11 texture → `QueryInterface(IMTLD3D11ExportableTexture)`
  → `GetMetalTexture` → `struct svr_metal_texture {magic "SVRT", handle, event, value, w, h, dxgi_format, array_size,
  samples}` passed as `Texture_t{handle, TextureType_Metal}` (Proton's unix side passes non-Vulkan handles through).
  The runtime collects both eyes and calls **winemetal call 146 directly** through the loaded wmtr client's
  `__wine_unix_call_funcs[146]` with a `WMTVRSubmitInfo` (`struct svr_wmtvr_submit`, layout checked by a
  `_Static_assert` in `wmtr_vr.m`): frame_id and display_time (host clock) of the frame `WaitGetPoses` returned,
  per-eye rect from the OpenVR bounds (`w = 0` whole texture, `WMTVR_EYE_FLIP_Y` when vMin > vMax), `WMTVR_EYE_SRGB`
  for 8-bit UNORM textures unless the colour space is Linear, slice (SubmitWithArrayIndex, or eye for a 2-slice
  array), per-eye pose = the frame's head pose ± IPD/2 along head x, fov of the frame, wait_event/value = the later
  eye's. MSAA textures are refused in the guest. Depth/motion attachments are not forwarded.
* **Queries [decided, new control op].** `WMTR_CTL_VR_QUERY` (= 4, a wmtr *control* request, so the winemetal call
  numbering is untouched): sync, params = one guest request, overwritten in place with the reply. Guest entry point:
  `int wmtr_vr_call(req, req_size, rep, rep_cap)` exported by the wmtr client. Executor side: `wmtr_vr_query()` in
  `wmtr_vr.m`, answered **without waiting** from the newest host packets:
  * `SVR_HELLO` → `vrl_state` (connected, eye size, refresh, IPD, fov, controller family, name).
  * `SVR_FRAME {last_frame_id}` → the pacer's `vrl_timing` (frame_id, wake_up, predicted_display); if the guest
    already has that frame (or no timing arrived), the cadence continues locally at the refresh rate. Plus
    `vrl_poses` (with the time they are for) and `vrl_input`. The guest sleeps until wake_up itself.
  * `SVR_HAPTIC` → `vrl_haptics` to the host.
* **vrlink.h additions (host → vm unless noted), for M0 to send:** `VRL_STATE` `vrl_state` on connect/disconnect/
  refresh change; `VRL_TIMING` `vrl_timing` once per frame (the pacer's next frame); `VRL_POSES` `vrl_poses`
  {time, pose[VRL_DEV_COUNT] (orientation, position, lin_vel, ang_vel, flags VALID/TRACKED/VEL), stage space} once per
  frame, predicted to that frame's display time; `VRL_INPUT` `vrl_input` {time, profile, value[2][21]} on change;
  vm → host `VRL_HAPTICS` `vrl_haptics` {hand, duration_s, frequency (0 = default), amplitude}. Devices = `vrhost::Device`
  order. Controller family `VRL_PROFILE_TOUCH` (Quest/Pico) or `VRL_PROFILE_INDEX`. **Input ids** = `enum svr_input`
  in `steamac_vr_guest.h` (trigger value/click/touch, squeeze value/click/force, thumbstick x/y/click/touch, A/X and B/Y
  click/touch, menu, system, thumbrest touch, trackpad x/y/touch/force); the adapter maps WiVRn `device_id`s to them.
* **Host side (done on `vr-int`):** `VrPipeline` is the session's `SessionListener` (construct it, then
  `VrSession::start(&pipeline)`): `VRL_STATE` from connect/disconnect/refresh, a frame-clock thread sends `VRL_POSES` +
  `VRL_TIMING` from `next_frame_timing()`, `VRL_INPUT` on change through `src/wivrn/input_map.cpp` (WiVRn `device_id` →
  `svr_input`), `VRL_HAPTICS` → `VrSession::haptics`. `vr_e2e` drives its producer the way the guest runtime does
  (`SVR_HELLO` / `SVR_FRAME` / `SVR_HAPTIC` through `wmtr_vr_query`). **Open for the launcher:** `PROTON_VR_RUNTIME`, deploying the
  patched `vrclient_x64.dll`.

### 4.1 VRChat static interface probe (M4, 2026-10-12)

Read-only ASCII/UTF-16 string scan of the installed VRChat x86-64 OpenVR/Unity plugins and IL2CPP metadata (no game
launch). Full provenance and interface inventory: `/Volumes/LEANWORK/vr-vrchat-interfaces.txt`. These strings show
versions present in the build, not proof every interface is requested at runtime; `STEAMAC_VR_LOG=1` remains the live
request oracle.

* `VRChat_Data/Plugins/x86_64/XRSDKOpenVR.dll`: `IVRSystem_022`, `IVRCompositor_028`, `IVRChaperone_004`,
  `IVROverlay_027`, `IVROverlayView_003`.
* `openvr_api.dll`: `IVRClientCore_003`, `IVRPaths_001`, `IVRControlPanel_006`, `IVRHeadsetView_001`,
  `IVRVirtualDisplay_002`.
* `global-metadata.dat` contains the current-generation set above plus `IVRInput_010`, `IVRChaperoneSetup_006`,
  `IVRApplications_007`, `IVRSettings_003`, `IVRRenderModels_006`, `IVRScreenshots_001`, `IVRNotifications_002`,
  `IVRTrackedCamera_006`, `IVRExtendedDisplay_001`, `IVRDriverManager_001`, `IVRResources_001`,
  `IVRSpatialAnchors_001`, `IVRIOBuffer_002`, `IVRDebug_001`, `IVRProperties_001`, and `IVRBlockQueue_005`.
  It also retains an older SDK generation: System_019, Compositor_022, Input_004, Overlay_018, Chaperone_003,
  ChaperoneSetup_005, Applications_006, Settings_002, TrackedCamera_003, and IOBuffer_001.
* **Critical Proton bootstrap:** Proton's `vrclient_init_registry` itself asks the runtime for `IVRClientCore_003`
  and `IVRCompositor_028`, before the game factory/interface requests. Missing `_028` causes registry state -1 and
  the game's runtime load to stop. Proton's Unix loader resolves both `HmdSystemFactory` and `VRClientCoreFactory`; the
  runtime must export both. The M4 runtime adds `HmdSystemFactory` as a wrapper around `VRClientCoreFactory`.
* These are C++ version interfaces. Proton's PE `vrclient_main.c` strips `FnTable:` before forwarding generic requests
  to Unix and constructs Windows FnTables there; the Unix runtime need not supply FnTable variants for this path.
* VRChat's Unity OpenVR subsystem manifest names OpenVR Display/Input and disables legacy input. Its SteamVR settings
  select scene initialization, stereo mode 2 and mirror view. The binding assets include Touch, Knuckles, Vive and
  other controller families; this is not evidence each binding is selected at runtime.

### 4.2 Proton DXMT test (M4, offline)

The pinned Proton PE+Unix `vrclient` build and a COM-object fake-texture test were run on x86 Linux in the prior M4
session. That run verified exportable D3D11 texture routing, completion-event/value propagation, `[0,1]`
`SubmitWithArrayIndex` slices, and the foreign-texture fallback; an installed unpatched Proton was a negative control.
The harness has now been changed to assert reversed explicit slices `[1,0]`, specifically ensuring slice zero is not
treated as “unset,” but this updated assertion has not yet been run. Local checks on macOS validate both Proton scripts'
shell syntax, `MANUAL_REGISTRY` argument handling with the variable unset and set, generated OpenVR stub consistency,
and portable runtime unit tests (32 checks, 0 failures). The Proton fake-texture harness itself requires the pinned x86
Proton/Wine build and has not been rerun because the x86 host is offline. Rerun it in non-manual `_028` bootstrap mode
once the M6 `_028` adapter is integrated. The guest/FEX run is separately blocked on explicit coordinator clearance of
the VM memory guard; no live VRChat test has been run.

The Proton harness invokes `vr-fakehost` as a background child; its return status and socket readiness are not yet
asserted explicitly. Host logs and the app's per-submit checks currently provide the substantive assertions. Strengthen
child/readiness failure handling when a deterministic failure fixture is available.

## 5. Launcher control socket [decided, M7]

The launcher and `steamac-vrhost` use a separate local control channel; this does not carry VR frames or GPU objects and
is not a link to GPL code. `VRHost.swift` passes `--control-socket PATH` when spawning the separate host executable.
The default path is `~/Library/Application Support/steamac/vrhost/control.sock`.

* Transport: AF_UNIX `SOCK_STREAM`, one connection per command. The launcher writes one UTF-8 request terminated by `\n`,
  reads one UTF-8 JSON object terminated by `\n`, and closes. The host accepts one request line (maximum 1024 bytes)
  and closes after its reply. Protocol version is the integer `v: 1` in every JSON response. The socket is local to the
  user account; it is not exposed on the LAN.
* Requests are literal command lines (not JSON):

| request | response |
|---|---|
| `status` | Current host/session state and stream counters. Includes `state` (`waiting`, `pairing`, `connected`), `port`, `bonjour`, `encryption`, `pin`, nullable `headset` (with the current `refresh_hz`, `refresh_rates`, `codec` `hevc`/`h264` and, once the headset reported it, `battery` `{charge 0..1, charging}`), nullable `pairing_request` (`{name, at, refused}`: the last unpaired headset that connected; `refused` = pairing was off and `--pair-on-request` not given), `app_request` (`{id, seq}`: the headset's last start request from the app list given with `--app`; `seq` counts requests, 0 = none), and `stream` statistics (frames, bitrate, encode/decode/pipeline ms, `idr_requests`; with the game path also
`game_fps`, `received`, `no_headset`, `compose_gpu_ms`, `submit_copied_ms`, `upscaler`). |
| `headsets` | `{"v":1,"headsets":[{"name":"…","last_connection":UNIX_TIME},…]}` for paired headsets. |
| `pair on` | Enable pairing and generate a fresh PIN; returns the updated `status`. |
| `pair off` | Stop accepting new pairings; already paired headsets can reconnect; returns the updated `status`. |
| `unpair NAME` | Remove the named headset; returns the updated paired-headset list. |
| `unpair-all` | Remove all paired headsets; returns the updated list. |
| `bitrate MBPS` | Total bitrate (1-400 Mbit/s) for the running encoders and the next connections; returns `status`. |
| `refresh HZ` | Ask the connected headset to switch to a refresh rate it offers (WiVRn `refresh_rate_change`); returns `status`, or an error without a headset or for a rate it does not offer. |

Fields are only added within protocol version 1; a launcher reads the new ones as optional (older hosts lack them).
Host options that shape these replies: `--render-scale` (the `render_width/height` reported, below 1 with the
`stream.upscaler` in use), `--pair-on-request` (an unpaired headset opens pairing with a fresh PIN instead
of being refused; pairing closes after it pairs), `--codec auto|hevc|h264`, `--app ID=NAME` (repeatable; the WiVRn
`application_list` the headset shows; its `start_app` only moves `app_request`, the launcher starts the game in the
guest through fx-progress-agent `launch-vr ID`).

Unknown requests and invalid unpair commands return `{"v":1,"error":"…"}`. If encryption is disabled, `pair on`
returns an error because pairing is unavailable. Pairing PINs are local status data and are never sent to the headset by
this control channel; the WiVRn pairing protocol verifies the PIN over its own encrypted pairing exchange. With the host
stopped, the launcher can list or unpair saved headsets by invoking `steamac-vrhost --list-headsets --json` or its
unpair CLI mode; status and pairing require the running control socket.

The host starts its `VrSession` once with the active `SessionListener` (the integrated pipeline, optionally wrapped by
the audio bridge), then creates this control server. On shutdown the control server is stopped, the session is stopped,
and listener resources are released in their owning order. The control server is optional: failure to bind logs a
warning and does not prevent the VR host from serving a headset.

## 6. VR audio region: steamac-vm / libkrun ↔ steamac-vrhost [decided, M5, implemented]

Game audio goes to the headset and the headset microphone comes back through one shared-memory region, contract in
`host/vrhost/include/vraudio.h` (plain C, mirrored in Rust by libkrun patch 0025 with a layout test).

* steamac-vm creates the region (`vra_region_init`), hands it to libkrun (`krun_snd_set_vr_audio`) and passes the fd
  to the host as `--audio-fd N`.
* Two lock-free SPSC rings of interleaved S16LE frames (play: stereo, mic: mono, 8192 frames each); each side polls on
  its own clock (libkrun: CoreAudio callback / virtio-snd pump; vrhost: `AudioBridge` 5 ms tick, the WiVRn quantum).
* Routing follows the headset: vrhost publishes `VRA_HEADSET_SPEAKER` / `VRA_HEADSET_MIC` plus a heartbeat; libkrun
  forwards playback and records from the mic ring only while they are set and the beat is younger than 1 s,
  otherwise the guest's audio is the Mac's again.
* In vrhost, `AudioBridge` wraps the session listener (the pipeline or the test pattern) and adds the microphone and
  connect/disconnect handling. Tests: `test_audio` (vrhost ctest), the libkrun `vr_audio` unit tests.
* Launcher: with VR on and a virtio-snd device, `VRHost.apply` creates the region (`steamac_vra_create` in
  `CKrun/shim.h`), attaches it with `KRUN_SND_VR_MUTE_LOCAL` and passes `--audio-fd`; `--selftest-vr` round 2 checks
  that the host beats on it with the headset word 0 (Mac output kept). Failure leaves VR video on and audio on the Mac.

## Change log

* 2026-10-12 M7: document the launcher's local control socket, request/response contract and session lifecycle.

* 2026-10-11 M0 agent: initial version (sections 1-3; section 4 placeholder).
* 2026-10-11 M0 agent: section 2 rewritten to match the implemented `vr_session.h` (encoder input = `'420f'` IOSurface, `views_at`, timing, input ids).
* 2026-10-11 M1 agent: section 3 decided and implemented for the frame path: **SOCK_DGRAM instead of SOCK_SEQPACKET** (not supported on macOS), no shared event across processes (VRL_SUBMIT sent on copy completion), vrlink.h drafted; new 3.1 (DXMT `IMTLD3D11ExportableTexture` + `WMTVRSubmit` call 146, wmtr exporter, vrhost compositor + `EyeEncoderSink`).
* 2026-10-11 integration agent (vr-int): section 1 launcher spawn implemented (`VRHost.swift`, `--vr`, Settings toggle);
  `Frame::foveation` (section 2); `WMTR_VR_FD` attach and the texture-ring requirement (3.1, **action for M2: ring copy in
  vrclient**); new 3.2 (pipeline, frame ids, tests).
* 2026-10-11 M2/M3 agent: section 4 written (submit via call 146 from the guest runtime, `WMTR_CTL_VR_QUERY`,
  `VRL_STATE/TIMING/POSES/INPUT/HAPTICS` structs added to vrlink.h on `vr-m23`).
* 2026-10-11 integration agent (vr-int): host emits `VRL_STATE/TIMING/POSES/INPUT` and routes `VRL_HAPTICS` (section 3
  table, section 4); `VRL_SOCKET_BUFFER`; link reader thread in the wmtr exporter.
* 2026-10-12 M5: section 6 (VR audio region).
