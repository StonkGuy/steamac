# steamac with genuine-EAC games and a DXMT graphics path

**English** · [Русский](README.ru.md) · [简体中文](README.zh.md)

**A fork of [steamac](https://github.com/fxgl/steamac) for Apple silicon Macs. It runs x86-64 Windows games with genuine
Easy Anti-Cheat (verified in game: VRChat; Mini Royale also passes EAC), with a choice of two graphics paths.**

- **Vulkan (classic)**: Direct3D through DXVK on Vulkan inside SteamOS (Venus), then KosmicKrisp or MoltenVK on the Mac.
  This is upstream steamac's path.
- **DXMT (lean)**: Direct3D 11 through [DXMT](https://github.com/3Shain/dxmt). Its Metal calls run natively on the Mac
  over a dedicated virtio-gpu channel, so there are fewer translation layers.

**Found a bug?** Please report it here (an issue, or a pull request with a test) instead of fixing it only in your own
fork, so everyone gets the fix. This fork sends nothing to libkrun, Mesa, FEX or Proton itself; steamac changes are
discussed with its maintainer directly. If you have reviewed a change here and think it is good enough for one of those
projects, you are welcome to submit it there yourself: keep its test with it, follow that project's process, and mention
where it came from. FEX code changes go to [StonkGuy/FEX](https://github.com/StonkGuy/FEX) as a pull request against the
current `eac-<release>` branch, with a test in its `eac-tests/`.

The EAC client is the stock one from the Proton EasyAntiCheat Runtime. Nothing fakes, replays or short-circuits its
result. Not affiliated with FX GAMES, Valve, Epic Games, VRChat or the DXMT authors.

Status: verified on one Mac (M2, 16 GB). VRChat passes EAC and enters a world on both graphics paths. The default is
the Vulkan path; DXMT is a setting you turn on. Measured on that Mac: VRChat about 29-44 fps on Vulkan and 76-78 fps on
DXMT (single runs, see [docs/eac-performance.md](docs/eac-performance.md)).

## Requirements

| you need | notes |
|---|---|
| an Apple silicon Mac | verified on an M2 MacBook Air, 16 GB, macOS 27.0; other Macs are untested |
| macOS 15 or newer | KosmicKrisp (the faster Vulkan driver) needs macOS 26. The DXMT path's host library is built for macOS 15.5 or newer |
| free disk space | we recommend about 20 GB free on the Mac for the SteamOS disk and its download cache, plus room for the games themselves (our estimate; it may vary). Measured: about 14 GB on the disk's volume while the disk is created, about 9 GB afterwards. The home size is chosen at creation and can be enlarged later with Grow Disk |
| memory | 16 GB minimum; less is not supported. Automatic VM RAM is half of the Mac's RAM (4-16 GiB) |
| a Steam account | games are installed and run inside SteamOS |

## Install and first launch

1. Build the app from this fork's `main` branch or a [release](https://github.com/StonkGuy/steamac/releases) tag:
   `./build.sh` ([requirements and building](docs/upstream/requirements-and-building.md); the DXMT path also needs
   `make dxmt-payload` in `wmtr/` before the layer step). Or download the DMG
   attached to a [release](https://github.com/StonkGuy/steamac/releases) (from v1.9-eac.1, built by GitHub Actions, ad-hoc signed). Copy
   **FX Steam Launcher.app** from `work/out/` (or the DMG) to Applications and open it (see *First open* below).
2. On first start, choose **Create New Disk…**. The app downloads Valve's SteamOS image, verifies it and sets it up.
   This takes a while, once.
3. Sign in to Steam in the window. Keep a **16:10** window size (Settings > Display > Default window
   size; the default, 1280×800, is 16:10). A 16:9 game in a 16:10 window is letterboxed.
4. Grant the **microphone** permission when macOS asks. Without it, the guest's audio capture fails repeatedly and games
   can stutter.
5. In Steam, install VRChat, set its compatibility tool to **Proton Experimental**, and install **Proton EasyAntiCheat
   Runtime**. See [SETUP.md](SETUP.md) for the full steps.

### First open

The fork's builds are ad-hoc signed and not notarized, so macOS Gatekeeper blocks the first open. This is needed once:

- Right-click (or Control-click) the app in Applications, choose **Open**, then **Open** again; or
- if macOS 15 or newer does not offer that, open System Settings > Privacy & Security, scroll to the "FX Steam Launcher
  was blocked" message, click **Open Anyway** and confirm; or
- in Terminal: `xattr -dr com.apple.quarantine "/Applications/FX Steam Launcher.app"`.

## Choosing the graphics path

Settings > Advanced > **Graphics path**: *DXMT (lean)* or *Vulkan (classic)*; it applies from the next start. Nothing
to set per game: when Steam starts, every installed Proton gets the selected path, and Proton puts it into each game's
prefix. Switching back to *Vulkan* restores Proton's own files exactly. If Proton's original files are missing (for
example after switching between two builds of the app on one disk), the files stay DXMT's and the boot log says so;
repair Proton in Steam: the Proton tool > Properties > Installed Files > **Verify integrity of tool files**. DXMT
covers Direct3D 11 (and 10); play Direct3D 12 games on *Vulkan* (DXMT's Direct3D 12 is limited). EAC on DXMT is
verified with VRChat; if EAC fails in another game, switch to *Vulkan*.

The **Metal Performance HUD** shows the frame rate on either path: Settings > Display, the View menu, or Ctrl+Cmd+P. It can
be turned on and off while a game runs.

## Easy Anti-Cheat

EAC runs inside SteamOS under Valve's FEX emulator. This fork's patched FEX passes EAC's checks for VRChat, and for
Mini Royale (EAC `301`; that game was not played past its account prompt). Results differ from game to game: each title
ships its own EAC build and checks, so an untested game may or may not work. Today two
manual steps are required for VRChat; both are planned to become automatic:

1. Install the patched FEX: enable SSH (Settings > Advanced), then run `build-fex.sh` and `install-fex-tool.sh` in the
   guest ([fex-eac/README.md](fex-eac/README.md)).
2. Set VRChat's Steam launch options (`EAC_LAUNCHERDIR`, `PROTON_EAC_RUNTIME`, `WINE_CPU_TOPOLOGY`, ...), exactly as given
   in [fex-eac/README.md](fex-eac/README.md) and [SETUP.md](SETUP.md).

- **Expected:** EAC's launcher reports `Launcher finished with: 301, 'Easy Anti-Cheat successfully loaded in-game'` and
  the game logs `AntiCheat Session Begin: Success`.
- **Not expected to work:** anti-cheat that refuses virtual machines, and titles whose anti-cheat uses its own
  anti-debug checks beyond EAC's Linux module.
- **After a Steam update:** if Steam updates its FEX tool, it replaces the patched build. Run
  `sh install-fex-tool.sh` again.

## Troubleshooting

| symptom | what to do |
|---|---|
| the DXMT setting seems to have no effect | the setting applies at the next VM start: restart the VM after changing it. If the app lacks the DXMT host library, Settings says so and the app uses Vulkan (boot log: `gpu: graphics path Vulkan`) |
| EAC fails on DXMT | switch to *Vulkan* and restart the launcher |
| Direct3D 11 games fail on *Vulkan* after using DXMT | Proton still has DXMT's files: in Steam, the Proton tool > Properties > Installed Files > Verify integrity of tool files |
| EAC does not finish, or fails after a Steam update | the patched FEX is missing or was replaced by Valve's; run `sh install-fex-tool.sh status` |
| about a minute's stall before joining | check that `WINE_CPU_TOPOLOGY` is in the launch options |
| stutter with audio crackle | grant the microphone permission (System Settings > Privacy & Security > Microphone) |
| long first launch | shaders compile once per game and path; later launches are faster |

More: [docs/eac-troubleshooting.md](docs/eac-troubleshooting.md).

## Limits

- One machine verified (M2, 16 GB, macOS 27.0). VRChat is verified in game on both paths; Mini Royale's EAC launcher
  reaches `301` (not played further). Other titles are untested and results differ game by game.
- 16 GB Macs: heavy titles can exceed the memory available. Unigine Superposition (720p, Low) went past 14 GB of app
  memory on both paths with an 8 GiB VM.
- VR is not part of this release (experimental, on the `vr-live` branch).
- DXMT: Direct3D 11/10. Its Direct3D 12 is limited (feature level 11_1, DXBC only, no DXIL, no ray tracing;
  ExecuteIndirect stalls the GPU); use *Vulkan* for Direct3D 12 games.
- D3DMetal is not included.
- The DXMT setting applies to every game; on it the game's Metal layer covers the whole VM view (a windowed game or alt-tab
  shows the game layer).
- Games that need more memory than the VM gets (Settings > Advanced) will swap.
- Switching the graphics path changes every installed Proton at the next boot; see *Choosing the graphics path* for
  the repair if a Proton was left with DXMT's files.
- Asynchronous pipeline compilation exists in the build but is off and not recommended (measured worse).
- The microphone has had little testing; a freeze after a microphone start is not fully ruled out. A rebuilt or re-signed
  app asks again for microphone permission.

## Documentation

| page | contents |
|---|---|
| [SETUP.md](SETUP.md) | the full setup guide: install, graphics path, Easy Anti-Cheat, troubleshooting |
| [docs/fork.md](docs/fork.md) | what changed in this fork and its status |
| [docs/eac-how-it-works.md](docs/eac-how-it-works.md) | the EAC chain and why the FEX swap is needed |
| [docs/eac-performance.md](docs/eac-performance.md) | measurements and limits |
| [fex-eac/README.md](fex-eac/README.md) | patched FEX setup and launch options |
| [docs/upstream/README.md](docs/upstream/README.md) | upstream steamac's documentation (Apache-2.0, © FX GAMES FZ LLC) |

## Licences and attributions

| component | licence |
|---|---|
| steamac (this fork) | Apache-2.0, © FX GAMES FZ LLC and contributors (`LICENSE`, `NOTICE`) |
| patched FEX ([StonkGuy/FEX](https://github.com/StonkGuy/FEX/tree/eac-2610), built by `fex-eac/build-fex.sh`) | MIT, as FEX-Emu |
| DXMT, with this fork's patches (`host/dxmt/patches/`) | LGPL-2.1 |
| wmtr (the DXMT remoting layer, `wmtr/`) | BSD-3-Clause |
| libkrun and its patches | Apache-2.0 |
| virglrenderer, Mesa / KosmicKrisp | MIT |
| MoltenVK | Apache-2.0 |
| DXVK (zlib), Proton / Wine (LGPL-2.1 and others) | per upstream; installed by Steam, not shipped |
| Linux kernel derivatives | GPL-2.0-only; complete source for each release is attached to it |

**D3DMetal is not bundled.** The app ships no game files and no EAC binaries. EAC comes from Valve's Proton EasyAntiCheat Runtime,
installed by Steam.
