# Setup guide

**Play VRChat flatscreen, and other Windows games, on an Apple silicon Mac. The app runs Valve's SteamOS in a virtual
machine; Windows games run there through Proton and the FEX emulator, with the game's own Easy Anti-Cheat running
unmodified.**

This is a fork of [steamac](https://github.com/fxgl/steamac). It is not affiliated with FX GAMES, Valve, Epic Games,
VRChat or the DXMT authors. VRChat does not support virtual machines or emulation; use is at your own risk, and
enforcement is the game publisher's decision.

Status: only one machine (M2 MacBook Air, 16 GB, macOS 27.0) has been verified. VRChat is verified in game on both
graphics paths; Mini Royale's EAC launcher also reaches `301` (not played further). Results differ game by game.
VR is experimental and off by default ([docs/vr.md](docs/vr.md)).

## Requirements

| you need | notes |
|---|---|
| an Apple silicon Mac | verified on an M2 MacBook Air, 16 GB; other Macs are untested |
| macOS 15 or newer | the faster Vulkan driver (KosmicKrisp) needs macOS 26; macOS 15 uses MoltenVK. The DXMT path's host library is built for macOS 15.5 or newer. Verified on macOS 27.0 |
| memory | 16 GB minimum; less is not supported. Automatic VM RAM is half of the Mac's RAM (4-16 GiB), with at least 3 GiB or a quarter of the RAM kept for macOS; on a 16 GiB Mac that is 8 GiB VM + 4 GiB graphics + 4 GiB reserve |
| free disk space | we recommend about 20 GB free on the Mac for the SteamOS disk and its download cache, plus room for the games themselves (our estimate; it may vary). Measured: creating the disk needs about 14 GB on its volume (about 9 GB afterwards, plus about 6 GB of download cache that is deleted on success). Choose the home size at creation; Grow Disk enlarges it later |
| a Steam account | games are installed and run inside SteamOS |
| internet access | the app downloads Valve's SteamOS image and the Steam client |

## Install

1. Download the DMG attached to a [release](https://github.com/StonkGuy/steamac/releases) (from v1.9-eac.1: built by GitHub Actions from the
   tag, ad-hoc signed, SHA-256 next to it; [release builds](docs/release-build.md)), or check out `main` or a
   [release](https://github.com/StonkGuy/steamac/releases) tag and build with `./build.sh`
   ([requirements and building](docs/upstream/requirements-and-building.md)). For the DXMT path, run
   `make dxmt-payload` in `wmtr/` before the layer step; without it the app offers only the Vulkan path.
2. Copy **FX Steam Launcher.app** from the DMG or `work/out/` to Applications.
3. **First open.** The fork's builds are ad-hoc signed and not notarized, so macOS Gatekeeper blocks the first open. This
   is needed once. Right-click (or Control-click) the app, choose **Open**, then **Open** again. If macOS 15 or newer does
   not offer that, open System Settings > Privacy & Security, scroll to the "FX Steam Launcher was blocked" message,
   click **Open Anyway** and confirm. Or in Terminal:
   `xattr -dr com.apple.quarantine "/Applications/FX Steam Launcher.app"`.

## First launch

1. Open the app by double-clicking it. The app itself needs no command-line options or environment variables; the
   graphics path is a setting (below). VRChat still needs its Easy Anti-Cheat launch options (see Easy Anti-Cheat).
2. On the first start, choose **Create New Disk...**. Accept Valve's SteamOS licence agreement and the Steam Subscriber
   Agreement (the download does not start without this), choose the home size and location, and set a password for the
   `steamos` user. The app downloads Valve's signed image, verifies it and sets it up. This happens once. Only the
   `stable` and `rc` SteamOS branches are offered; use `stable` unless you have a reason not to.
3. Allow the **microphone** permission when macOS asks. Without it the guest's audio capture fails repeatedly and games can
   freeze or stutter.
4. In Settings > Display, keep a **16:10** window size (Default window size; the default, 1280x800, is 16:10). When a game
   uses 16:9 in a 16:10 window, the picture is letterboxed.

Boot and shutdown progress is shown as an overlay. The first click or key press collapses it to a progress pill at the
bottom of the window; click the pill or use View > Show Boot Progress to expand it again.

## Signing in to Steam

Sign in inside the window. With the default Steam client the sign-in screen offers a password form and an on-screen QR
code (scan it with the Steam Mobile App, Steam Guard > scan). Tick **Remember me**. Steam may restart once after sign-in.
Switching between Steam client builds downloads up to about 1 GB; progress shows in the boot overlay.

## Installing VRChat

1. In Steam, install **VRChat**.
2. Set VRChat's compatibility tool to **Proton Experimental** (Properties > Compatibility). On ARM64 Steam, VRChat installs
   its Android build by default, which cannot run here; Proton Experimental selects the Windows build. This is the only
   compatibility tool you pick; the graphics path does not change it.
3. Install **Proton EasyAntiCheat Runtime** (Steam tool 1826330) from Steam. It supplies the stock EAC launcher and client.
4. Install the patched FEX and set VRChat's launch options, as described under Easy Anti-Cheat below. Both are still
   required today.

## Choosing the graphics path

Settings > Advanced > **Graphics path**: **DXMT (lean)** or **Vulkan (classic)**. Restart the launcher to switch.

| path | what it is | when to use it |
|---|---|---|
| **DXMT (lean)** | Direct3D 11 (and 10) goes through DXMT, which produces Metal calls. They run natively on the Mac over a dedicated virtio-gpu channel and are shown in a Metal layer inside the window. DXVK, Venus, virglrenderer and the Vulkan-to-Metal layer are not involved | Direct3D 11 games, including VRChat, when the lean path works for you; simple Direct3D 12 games (experimental) |
| **Vulkan (classic)** | Direct3D goes through DXVK to Vulkan inside SteamOS (Venus), then KosmicKrisp or MoltenVK to Metal on the Mac. This is upstream steamac's path | fallback if a game misbehaves on DXMT or EAC fails; Direct3D 12 games that need more than DXMT offers |

How it works: at every boot the guest reads the launcher setting. With DXMT, every installed Proton's DXVK files (d3d11,
dxgi, d3d10core) and its vkd3d-proton d3d12.dll are replaced by DXMT's before Steam starts, and again when Steam updates a Proton during the session.
With Vulkan, Proton's own files are restored byte for byte. ARM64 Protons are left alone. There is no per-game
compatibility tool and no extra step:
the setting applies to every game, and the game just launches. If the DXMT library is missing from the app, the launcher
falls back to Vulkan.

- Direct3D 12 under the DXMT setting is experimental: DXMT's Direct3D 12 offers feature level 11_1, shader model 5.1
  (DXBC, no DXIL), no ray tracing, no tiled resources, and ExecuteIndirect stalls the GPU. A Direct3D 12 game that
  needs more fails to start or renders wrongly; use **Vulkan** for it (vkd3d-proton). There is no automatic per-game
  fallback: with DXMT's dxgi in place, vkd3d-proton's d3d12 cannot present, so the switch is the setting.
- **Repairing Proton.** The originals are kept in the guest until they are back in place. If one is missing (for
  example after switching between two builds of the app on one disk), DXMT's file stays, the boot log shows a
  `WARNING: no original of ... kept` line, and Direct3D 11 games fail on Vulkan. Repair it in Steam: the Proton tool
  (for example Proton Experimental) > Properties > Installed Files > **Verify integrity of tool files**.
- EAC on DXMT is verified with VRChat. If EAC fails on DXMT in another game, switch to Vulkan.
- The default is **Vulkan (classic)**. DXMT measured faster on the one verified Mac (see Known limits), but the
  default has not been changed.
- DXMT covers Direct3D 11 and 10, and Direct3D 12 within the limits above.
- Direct3D 11 stream output with rasterization, or from a geometry shader, is not implemented in DXMT upstream. Games
  that use it render wrongly on DXMT (Unigine Superposition shows black shadow areas); use **Vulkan** for them.

Settings > Advanced > **Vulkan driver** (KosmicKrisp or MoltenVK) applies to the Vulkan path only.

## Metal HUD and frame counter

Apple's Metal Performance HUD shows the frame rate in the window. Toggle it with **Ctrl+Cmd+P**, **View > Show Metal
Performance HUD**, or Settings > Display. It can be switched on and off while a game runs, on both graphics paths. On the
DXMT path it is drawn on the game's layer while a game has focus.

## Disk size

Settings > Advanced > **Grow Disk...** enlarges the SteamOS home partition without recreating the disk
or deleting games (grow only, up to 4096 GiB). It shuts SteamOS down, enlarges the image and boots again. The disk can live
on an external APFS, Mac OS Extended or exFAT volume (FAT32 is rejected). exFAT needs the full disk size free up front.

## Easy Anti-Cheat

- Games with Linux Easy Anti-Cheat run its stock client from Valve's Proton EasyAntiCheat Runtime. Nothing fakes, replays,
  short-circuits or modifies EAC or its result.
- Valve's FEX build in Steam's FEX tool fails EAC's `ptrace` injection and signal semantics. This project replaces it with a
  patched FEX-2610 that emulates those parts of Linux more faithfully.
- **Installing the patched FEX (required today).** Enable SSH in the guest (Settings > Advanced > Enable SSH), copy the
  `fex-eac/` folder of this repository into the guest, and run there:

  ```sh
  sh build-fex.sh            # clones FEX-2610, applies the patches, builds, stages
  sh install-fex-tool.sh     # swaps the build into Steam's FEX tool; Valve's files are backed up first
  sh install-fex-tool.sh status    # which build is installed
  sh install-fex-tool.sh restore   # put Valve's build back
  ```

  A Steam update of the FEX tool overwrites the patched build with Valve's; run `sh install-fex-tool.sh` again.
  Providing this step automatically is planned. It is not yet.
- **VRChat launch options (required today).** In Steam, VRChat > Properties > Launch Options:

  ```
  env EAC_LAUNCHERDIR=$HOME/.local/share/Steam/steamapps/compatdata/438100/pfx/drive_c/users/steamuser/AppData/Roaming/EasyAntiCheat PROTON_EAC_RUNTIME="$HOME/.local/share/Steam/steamapps/common/Proton EasyAntiCheat Runtime" WINEDEBUG=-all PROTON_USE_XALIA=0 WINE_CPU_TOPOLOGY=16:0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3 %command%
  ```

  `EAC_LAUNCHERDIR` and `PROTON_EAC_RUNTIME` load the EAC runtime. `WINE_CPU_TOPOLOGY` reports 16 CPUs mapped onto the
  guest's 4 vCPUs and removes a stall of about a minute before joining (4-6 s region lookup with it). Keep the count at
  16 and list the guest's actual vCPU ids (see `nproc`) in place of the repeated `0,1,2,3`. Setting these options
  automatically is planned. It is not yet.
- **A successful start:** the EAC launcher log (`anticheatlauncher.log`) ends with
  `Launcher finished with: 301, 'Easy Anti-Cheat successfully loaded in-game'` (about 5 s), and VRChat's log shows
  `AntiCheat Session Begin: Success` and `Finished entering world`, with no `null client`.
- **EAC with DXMT** is verified with VRChat; switch the Graphics path to Vulkan if EAC fails in another game.
- **Not expected to work:** anti-cheat that blocks virtual machines, and titles whose anti-cheat adds its own anti-debug
  checks beyond EAC's Linux module (a second title was tried and stalls inside those checks).
- Never attach a debugger, `strace` or `perf` to the running game; EAC reports it.

## Troubleshooting

| symptom | what to do |
|---|---|
| the DXMT setting seems to have no effect | the setting applies at the next VM start: restart the VM after changing it. If the app lacks the DXMT host library, Settings says so and the app uses Vulkan (boot log: `gpu: graphics path Vulkan`) |
| log flooded with capture errors, or the game freezes after a microphone start or an in-game audio/resolution change | grant the microphone permission (System Settings > Privacy & Security > Microphone) |
| the Android build of VRChat starts | set VRChat's compatibility tool to Proton Experimental |
| EAC runtime errors at start | install **Proton EasyAntiCheat Runtime** from Steam |
| EAC launcher does not finish, or worked before a Steam update | the patched FEX is missing or was replaced by Valve's; run `sh install-fex-tool.sh status`, then `sh install-fex-tool.sh` |
| ~60 s stall before joining | check that `WINE_CPU_TOPOLOGY` is in the launch options |
| stutter in new areas | expected: new shader pipelines compile before a frame can finish (50-100 ms typical, much longer in rare cases). The Vulkan path does not keep its pipeline cache between runs. The first join is slower than later ones (86 s then 32 s in one measured VRChat run) |
| window has no picture for several seconds after boot | the progress pill shows "Waiting for SteamOS to draw..." with the reason; it disappears at the first non-black frame |
| "Loading…" card while in a game | the guest has sent no GPU commands for 2 s; this is normal while shaders compile |
| low frame rate | the game is GPU-bound on a fanless M2. Lower in-game graphics, use a lower guest resolution (Retina has about 4x the pixels of the matching non-Retina mode), keep the charger connected, close other Mac apps (these suggestions were not benchmarked) |
| game freezes after a microphone start or audio change | see the microphone row above; a fix for the capture start is part of the app, but the freeze risk is not fully ruled out |
| audio | audio goes through CoreAudio with about 65 ms latency on built-in speakers; choose the output device and buffer size in Settings > Sound |
| game needs more memory than the VM has | raise VM RAM in Settings > Advanced; Settings warns when less than 2 GiB is left for graphics |
| Direct3D 11 games fail on Vulkan after using DXMT | a Proton kept DXMT's files (boot log `WARNING: no original ... kept`): Steam > the Proton tool > Properties > Installed Files > Verify integrity of tool files |

## Known limits

- One Mac verified; VRChat verified in game on both paths, Mini Royale's EAC reaches `301`. Other titles, Macs and
  macOS versions are untested, and EAC results differ game by game.
- Measured VRChat results on the M2 MacBook Air (single runs, 1280x800): about 29-44 fps on Vulkan, 76-78 fps on DXMT; lower under Mac memory pressure
  ([docs/eac-performance.md](docs/eac-performance.md)). GPU-bound and power-limited.
- 16 GB Macs: heavy titles can exceed the memory available. Unigine Superposition (720p, Low) went past 14 GB of app
  memory on both paths with an 8 GiB VM.
- VR is experimental and off by default: see [docs/vr.md](docs/vr.md).
- Anti-cheat that blocks virtual machines will not work.
- DXMT: Direct3D 11/10. Direct3D 12 under the DXMT setting is limited (see above); use Vulkan for those games.
- The DXMT setting applies to every game; there is no per-game opt-out.
- On the DXMT path the game's Metal layer covers the whole VM view (a windowed game or alt-tab shows the game layer), and a 16:9 game in a 16:10 window is letterboxed; pick a 16:10 resolution.
- Asynchronous pipeline compilation exists in the build but is off and not recommended (measured worse).
- The patched FEX and VRChat's launch options are installed by hand today.
- Direct3D 12 on the Vulkan path: feature level 12_0 on KosmicKrisp, 11_0 on MoltenVK.
- The microphone is advertised to SteamOS but has had little testing.
- A rebuilt or re-signed app asks again for microphone permission and keychain access.
- VRChat does not support virtual machines or emulation.

## Licences and third-party components

| component | licence | note |
|---|---|---|
| steamac (this fork) | Apache-2.0, FX GAMES FZ LLC and contributors | see `LICENSE` and `NOTICE` |
| FEX-Emu with the patches in this project | MIT | x86-64 emulator inside Steam's FEX tool |
| Proton / Wine | LGPL-2.1 and others, per upstream (Valve's Proton) | installed by Steam, not shipped |
| DXMT, with this project's patches (`host/dxmt/patches/`) | LGPL-2.1 | DXMT path |
| wmtr (`wmtr/`) | BSD-3-Clause | DXMT path: carries DXMT's calls from the guest to Metal on the Mac |
| DXVK | zlib | Vulkan path; part of Proton |
| libkrun and its patches | Apache-2.0 | virtual machine |
| Mesa / KosmicKrisp, virglrenderer | MIT | Vulkan path |
| MoltenVK | Apache-2.0 | Vulkan path on macOS 15 |
| Linux kernel derivatives | GPL-2.0-only | complete source for each release is attached to it |

**D3DMetal is not bundled.** Nor are game files, Easy Anti-Cheat binaries, or SteamOS itself: the app downloads Valve's
signed SteamOS image from Valve's servers after you accept Valve's agreements, and EAC comes from Valve's Proton
EasyAntiCheat Runtime installed by Steam. Screenshots and artwork of Steam, SteamOS and games belong to Valve and the
respective publishers.
