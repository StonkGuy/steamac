# Adding tracked devices (trackers) to the steamac VR runtime

**Status: experimental.** The table, the runtime side, `svr-driverhost` and `svr-tracker-feed` have offline tests
(`guest/vr`, `make check`). See [Limits](#limits) for what has and has not been run against a real title.

steamac runs SteamVR games without SteamVR: the game's `openvr_api.dll` reaches our OpenVR runtime
(`guest/vr`, [`vr-guest.md`](vr-guest.md)), which gets the headset and hand controllers from the steamac VR link.
Anything else that should appear as a tracked device goes through the **tracked-device table** described here. This
covers full-body trackers, body points estimated in software (for example Standable: Full Body Estimation), pucks, and
whatever a project wants a game to see as a device.

## Device model

| OpenVR device index | Source | Class |
|---|---|---|
| 0 | headset (steamac VR link) | `TrackedDeviceClass_HMD` |
| 1, 2 | left and right hand controller (steamac VR link) | `TrackedDeviceClass_Controller` |
| 3 to 18 | tracked-device table, slot 0 to 15 (index = 3 + slot) | as the source sets it, default `TrackedDeviceClass_GenericTracker` |

A table device is seen by the game the way SteamVR shows a Vive Tracker:

* **Enumeration and class:** `GetTrackedDeviceClass`, `GetSortedTrackedDeviceIndicesOfClass`,
  `IsTrackedDeviceConnected`, `VREvent_TrackedDeviceActivated` / `Deactivated` when it comes and goes.
* **Poses:** in the full device arrays of `WaitGetPoses`, `GetLastPoses`, `GetDeviceToAbsoluteTrackingPose` and
  `GetLastPoseForTrackedDeviceIndex`, in the same standing space as the headset.
* **Properties:** the source's strings, otherwise the Vive Tracker 3.0 defaults below.
  `Prop_ControllerRoleHint_Int32` is `TrackedControllerRole_OptOut`: a tracker never becomes a hand.

| Property | Default |
|---|---|
| `Prop_SerialNumber_String` | the source's serial (required, unique) |
| `Prop_ControllerType_String` | `vive_tracker_<role>` (e.g. `vive_tracker_waist`), or `vive_tracker` without a role |
| `Prop_InputProfilePath_String` | `{htc}/input/vive_tracker_profile.json` |
| `Prop_RenderModelName_String` | `{htc}vr_tracker_vive_3_0` |
| `Prop_TrackingSystemName_String`, `Prop_ManufacturerName_String` | `steamac` |
| `Prop_ModelNumber_String` | `steamac tracker` |

**Roles** are SteamVR's tracker roles (`TrackerRole_Waist`, `TrackerRole_LeftFoot`, `TrackerRole_Chest`, the knees,
elbows, shoulders, wrists, ankles, `TrackerRole_Camera`, `TrackerRole_Keyboard`, `TrackerRole_Handed`). Titles
differ in how they use them. VRChat's full-body calibration takes every connected `GenericTracker` and assigns it to
a body part by position during calibration. Role-aware titles read `Prop_ControllerType_String`.

## Two ways to feed it

```
 SteamVR driver (unchanged)          your own program
 driver_x.so / driver_x.dll          (any language)
        │ IServerTrackedDeviceProvider        │
        ▼                                     │
 svr-driverhost (Linux, or .exe under Wine)   │
        │                                     │
        └──────────► /dev/shm/steamac-vr-trackers ◄── mirror: headset + hands, written by the game's runtime
                              │
                              ▼
         game's vrclient.so (guest/vr): devices 3..18 in IVRSystem / IVRCompositor
```

1. **An existing SteamVR driver, unchanged:** `svr-driverhost` loads it the way `vrserver` does and copies the
   devices it adds into the table. Use this when a product ships a driver (most tracker hardware and estimators do).
2. **Your own source:** write to the table directly. It is one fixed-layout shared file, with no daemon and no socket
   protocol. `svr-tracker-feed` is the reference source, about 150 lines.

Both can run at the same time. The table has 16 slots shared between all sources.

### The table

Header: `guest/vr/include/steamac_vr_trackers.h` (Apache-2.0 OR GPL-3.0-or-later, so either side's licence can use it).
Helpers in C++: `guest/vr/runtime/trackers.{h,cpp}`. They need only the C library, and kernel32 on Windows.

* **Location:** `/dev/shm/steamac-vr-trackers`, or `STEAMAC_VR_TRACKERS=<path>`. The game's runtime creates it when
  the game starts VR. A source may create it first. Inside the Steam Linux Runtime container, `/dev/shm` is the
  guest's `/dev/shm`, so native sources, Proton games and Wine programs all see the same file. A Windows program maps
  it as `Z:\dev\shm\steamac-vr-trackers` (`CreateFileMapping` on that file).
* **Layout:** `struct svr_trk_table`: a header (magic `VTRK`, version 1, size, 16 slots), the **mirror**, and 16
  `struct svr_trk_device`. Little-endian. The size is fixed, so a different version or size is refused.
* **Claim a slot:** look for a slot whose `serial` is yours, which lets a restarted source take its old slot back.
  Otherwise compare-and-swap a free slot's `state` from `SVR_TRK_FREE` (0) to `SVR_TRK_CLAIMED` (1). Fill in
  `device_class`, `role` and the strings, then store `SVR_TRK_PRESENT` (2). The game sees the device from its first
  pose update on.
* **Update the pose:** seqlock. Increment `seq` to an odd value, write `pose` (and `updated`), then increment `seq`
  to an even value with release ordering. Readers copy the slot and retry if `seq` was odd or changed. Write at your
  tracking rate, and at least every 100 ms even when nothing moves.
* **Pose:** `struct svr_pose`. Orientation as x, y, z, w. Position in metres in the headset's standing space (+y up,
  -z forward). Optional linear and angular velocity in stage space (`SVR_POSE_VEL`), which the runtime uses to
  extrapolate to the game's frame time. Flags: `SVR_POSE_VALID`, `SVR_POSE_TRACKED` (clear = inferred or out of range).
* **Remove:** store `SVR_TRK_FREE`. The game gets `TrackedDeviceDeactivated` on its next frame.
* **No shared clock:** readers time a device by when they see its `seq` change. A device whose `seq` has not moved for
  2 s reads as disconnected, so a crashed source's devices drop out by themselves (`svr-tracker-feed --clear` frees
  their slots). A device seen for the first time connects on its next update. `updated` is informational only.
* **Mirror:** the game's runtime writes the headset and controller poses, refresh rate, FOV and IPD into
  `svr_trk_mirror` (seqlocked) each time it fetches poses. A source that derives body points from the headset reads
  it there. A source never writes the mirror.

### Writing a source: checklist

1. Open or create the table (`svr::TrackerTable::open(nullptr, true)`).
2. Claim one slot per device with a stable serial, its class (normally `TrackedDeviceClass_GenericTracker` = 3) and
   role. Leave the strings empty unless the title needs specific values.
3. In your tracking loop, `update(slot, pose, now)` for each device. Use the headset's space. If your tracking system
   has its own origin, transform into the space of the mirror's head pose (a one-time calibration on your side).
4. On exit, `release(slot)`.
5. Check with `svr-tracker-feed --list`, which shows the mirror, the slots, their sequence numbers and poses. In the
   game, the runtime logs `steamac-vr: tracker <index> connected: serial ... role ...` (`STEAMAC_VR_LOG=1`), and
   its 5-second pose line counts how often the title asked for a tracker's class and properties.

`svr-tracker-feed --count 1..8` itself is a usable test source. It places waist, feet, chest, knees and elbows around
the headset in a standing layout, which is enough to see trackers in a title's calibration screen.

### Hosting a SteamVR driver: `svr-driverhost`

```
svr-driverhost [--table PATH] [--hz 90] [--seconds N] [--verbose] [DRIVER_DIR...] [--then PROGRAM ARGS...]
```

`DRIVER_DIR` is the driver's root, the directory with `driver.vrdrivermanifest` (`"name": "<name>"`). The Linux build
loads `bin/linux64/driver_<name>.so`. The Windows build `svr-driverhost.exe` runs under Wine/Proton and loads
`bin/win64/driver_<name>.dll`. It provides what `vrserver` gives a tracker driver:

| Driver API | svr-driverhost |
|---|---|
| `HmdDriverFactory("IServerTrackedDeviceProvider_004")`, `Init`, `RunFrame` (at `--hz`), `Cleanup` | yes |
| `IVRServerDriverHost` (`_005`/`_006`): `TrackedDeviceAdded`, `TrackedDevicePoseUpdated` | devices go to the table. HMD-class devices are refused (the headset comes from the steamac VR link). Controller-class devices are published with their class |
| `GetRawTrackedDevicePoses` | headset and hands from the mirror, the table's devices by index |
| `IVRProperties` | full property store per device. Strings reach the table when the device is activated and when they change |
| `IVRSettings` (`_002`/`_003`) | the driver's `resources/settings/default.vrsettings`, plus changes in memory. The `trackers` section (`"/devices/<tracking system>/<serial>": "TrackerRole_..."`) sets the role, as in SteamVR's `steamvr.vrsettings` |
| `IVRResources`, `IVRDriverManager`, `IVRDriverLog` | yes (`{name}/...` resource paths, log lines on stderr) |
| `IVRDriverInput` (`_003`/`_004`) | components are created and their updates accepted, then ignored (no game reads tracker input) |
| `IVRIOBuffer`, `IVRDriverSpatialAnchors` | present, every call fails |
| display, direct mode, camera, compositor, watchdog components | not provided |

Device indices are the same in the driver and in the game (`3 + slot`). A driver that matches devices by index across
`GetRawTrackedDevicePoses` therefore sees the same numbers.

**Windows drivers.** Build with `make driverhost-win MINGW=x86_64-w64-mingw32-` (llvm-mingw or GCC mingw-w64; the
binary is static). Run it with the same Wine/Proton build and prefix as the companion program that talks to the driver,
so that both share one wineserver for their pipes and shared memory. The driver host maps the table through
`Z:\dev\shm\...`, which needs Wine's usual `Z:` mapping of `/`. Note on ABI: drivers are built with MSVC, the host with
mingw. The driver interfaces are single-inheritance classes without overloaded virtuals, and the host never calls a
driver method that returns a structure by value (`ITrackedDeviceServerDriver::GetPose` is unused by contract), so
the vtables and calls match.

## Companion programs without a host link

A body estimator, overlay or utility is a second OpenVR process (`VR_Init` as `VRApplication_Overlay`, `Background` or
`Utility`). It has no steamac VR link of its own, because the link lives in the game's DXMT context. When such a
process starts while a game is in VR, the runtime serves it from the table: headset and hand poses from the mirror,
plus every table device. `WaitGetPoses` paces at the headset's refresh rate. Submitting frames and haptics are not
available to it. The runtime logs `init without a host link: poses from the scene app's mirror`. If no game is in VR
(the mirror has not moved for 250 ms), `VR_Init` fails as before.

## Standable: Full Body Estimation

Standable (Steam app 2370570, Windows) estimates up to 8 body points (waist, chest, feet, knees, elbows) from the
headset and controllers. It has two parts:

* a SteamVR **driver** that publishes the estimated points as generic trackers, and
* a desktop **application** that reads the headset and controller poses through OpenVR, runs the estimation and
  calibration, and hands the results to its driver.

The install directory is the driver root (`driver.vrdrivermanifest`, `bin/win64/driver_standable.dll`, driver name
`standable`). The application and the driver talk over Windows named pipes, so both have to run under the same
wineserver, which means the same Proton build and the same prefix.

Setup:

1. Install Standable in the guest Steam library (it is a regular Steam app).
2. Copy `steam_api64.dll` from the Steamworks SDK (`redistributable_bin/win64/`, SDK 1.59 or later) to
   `~/.local/share/steamac/vr/steam_api64.dll` in the VM. Standable's driver imports it. SteamVR ships it next to
   `vrserver.exe`; steamac does not redistribute it.
3. Launcher: Settings > VR > "Tracker app" = `2370570`, then restart the VM.

With VR on, the guest then takes care of the rest (`steam-vr-companion`, at Steam start):

* It copies `svr-driverhost.exe` and the launch wrapper `steamac-vr-companion` under `~/.local/share/steamac`.
* It sets Standable's Steam launch option to `steamac-vr-companion %command%`, unless you set your own.
* It sets Standable's compatibility tool to Proton Experimental, unless you set another one. The VR path is
  installed there only.
* When a VR game starts, it starts Standable through Steam.
* The wrapper runs `svr-driverhost.exe --then Standable.exe` in Standable's Proton session. The driver loads first, then
  the application starts as its child under the same wineserver, so their pipes connect.
* Setting the field back to 0, or turning VR off, puts the launch option and the compatibility tool back.

Then calibrate in Standable, and in the title (VRChat: "Calibrate FBT", T-pose, triggers).

Without the launcher option, the same works by hand. Set the launch option and compatibility tool yourself, with
`svr-driverhost.exe` and `steam_api64.dll` in one directory. `svr-driverhost --then PROGRAM` takes the driver from
PROGRAM's directory when that directory holds a `driver.vrdrivermanifest`.

### Live result (VRChat, synthetic Quest 3)

* `svr-tracker-feed`, 8 devices: all 8 connected in VRChat's process. VRChat polled each tracker's class every frame
  (8 class queries per frame). No freeze, EAC session and world join as usual.
* Standable's driver, unmodified, loaded in `svr-driverhost.exe`:
  - It read its settings, created its 8 devices (Hips, Chest, both elbows, knees and feet) with their roles, and
    passed its Steam licence check.
  - VRChat saw all 8 as generic trackers and read their properties.
* The Standable application ran, but no estimated poses reached the driver. The driver's pipe client could not reach
  the application ("Failed to send message"), so the trackers stayed at the origin.
  - Open question: Steam starts the application inside the Steam Linux Runtime container, and the driver host ran
    outside it. Whether the two share pipes across that boundary, or whether the application waits for user input in
    its window first, is not yet known.
  - Next step: start `svr-driverhost.exe` from inside the same container (a Steam launch of its own, or the
    container's launcher service).

## Prior art and why this shape

* **SteamVR's driver model** (Valve's `openvr_driver.h`, BSD-3-Clause, shipped unmodified in
  `guest/vr/third_party/openvr`) is what tracker products already target: SlimeVR, Driver4VR, ALVR, Virtual Desktop and
  Standable all add their trackers through `IVRServerDriverHost::TrackedDeviceAdded` in a SteamVR driver. Hosting
  those drivers unchanged reuses that work instead of asking each product for a steamac integration.
* **On Linux with Monado/WiVRn**, trackers usually come from Monado drivers (Vive/Tundra natively, SlimeVR over its
  SolarXR driver or OSC). OpenVR titles see them through OpenComposite or xrizer. steamac has no OpenXR runtime in the
  game path ([`vr-guest.md`](vr-guest.md) explains why), so the equivalent sits in our own runtime.
* **A shared table** instead of a socket server keeps the per-frame cost of the game's runtime to one seqlocked copy
  per slot, with no thread, no syscall and no wait. Without sources, the table is opened once (a game in VR creates
  it) and read on each pose fetch.

## Tests

`guest/vr/tests/run_trackers.sh` (part of `make check`, native x86-64 Linux, also under FEX):

1. `svr-tracker-feed` with 3 devices and `vr-trackerapp` as the game (loads `vrclient.so` like Proton). Checks: 3
   generic trackers with the right classes, role hints and controller types, valid poses every frame in the
   head-relative layout (within 1 cm in practice, 5 cm allowed), activated and deactivated once each.
2. 8 devices and a source killed with `SIGKILL`: the stale devices disconnect within the 2 s limit.
3. `svr-driverhost` with `tests/sample_driver` (a tracker driver written only against `openvr_driver.h`, with a
   non-identity world-from-driver transform, a role from its `trackers` setting, and a refused HMD).
4. A background app without a host link gets the headset pose and the feed's trackers through the mirror.
5. `svr-driverhost --then`: the driver is taken from the program's directory, the program starts, and the host exits
   with it. The Windows build was checked the same way under Wine, with a Unix path for the program.

`make check-wine` (needs `wine64`, `tests/run_trackers_wine.sh`) runs the Windows build the way a body estimator runs:
* `svr-driverhost.exe --then companion.exe` loads the test driver, which serves a named pipe like Standable's driver;
* the companion starts in the same wineserver and sends moving waist positions over that pipe;
* the waist's pose in the table follows them (checked: x 0.2 to 2.5 m over 2.5 s), and the host exits with the
  companion.

`scripts/test/steam-vr-companion.sh` covers the guest side against a fake Steam tree:
* launch option, compatibility tool and tools are set up, and a second run changes nothing;
* the user's own launch options are kept;
* the wrapper swaps in the driver host, and runs the command unchanged without it;
* the watcher starts the app once per VR session;
* "off" restores both files.

The Windows build (`make driverhost-win`) was checked under Wine 5.0 (Ubuntu 20.04 x86-64) with the sample driver
built as a DLL. The devices it published were read by the native `svr-tracker-feed --list` from the same table file.

`tests/unit_tests.cpp` covers roles, slot claim, takeover and release, a full table, seqlock reads, stale and
first-sight rules, the mirror, and refusal of an incompatible file.

## Limits

* **Standable estimates do not reach the game yet** (see Live result). Its driver and devices work.
* **Drivers that import `steam_api64.dll`** need one beside `svr-driverhost.exe` (SteamVR ships it). steamac does not
  redistribute Valve's Steamworks DLL.
* **Pose timing:** a pose counts as current when the game's runtime first sees it, at most one source period late
  (about 11 ms at 90 Hz), then extrapolated with the source's velocities (at most 100 ms). Sources without
  velocities are not extrapolated.
* **A stale device keeps its last pose** until the 2 s timeout. SteamVR does the same while a driver reports a
  device as connected.
* **No tracker input or haptics** reach the game (`IVRDriverInput` updates are ignored). No IVRInput pose actions
  for `/user/waist` style paths: titles that bind tracker poses through SteamVR Input instead of reading device poses
  are not covered yet.
* **No render models:** `IVRRenderModels` is not implemented in the runtime, so overlays draw nothing for trackers.
* **Driver features out of scope:** headset or display drivers, camera, compositor plugins, watchdog-driven startup,
  driver `PollNextEvent` (no events are delivered), `RequestRestart` (logged only).
* **16 slots** in total for all sources.
* **One tracker app** in the launcher option. The guest script takes a comma-separated list.
* **Drivers that are not beside their application** (most hardware tracker drivers) need `svr-driverhost` started by
  hand with their driver directory.
