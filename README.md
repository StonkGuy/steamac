# steamac with genuine-EAC games (branch `fex-eac`)

**A fork of [steamac](https://github.com/fxgl/steamac) that runs x86-64 games with genuine Easy Anti-Cheat — verified with
VRChat — on an Apple silicon Mac.**

It does this by replacing the FEX inside Steam's own FEX compatibility tool (Steam app 3127680) with a patched FEX-2610
build (patches 0001–0020), plus launcher, stability and performance fixes. The EAC client is the stock one from the Proton EasyAntiCheat
Runtime; nothing fakes, replays or short-circuits its result. Valve's SteamOS rootfs is unmodified — everything lives in
the guest's home directory.

It is deliberately **one game on one machine** (VRChat on an M2 MacBook Air, 16 GB, macOS 27.0). Treat it as a starting
point. Nothing here is submitted to steamac, libkrun, Mesa, FEX or Proton. Not affiliated with FX GAMES, Valve, Epic Games
or VRChat.

## What it takes

| you need | why |
|---|---|
| **the steamac app** (1.8.x) and a booted SteamOS disk | the guest is Valve's ARM64 SteamOS (Steam Frame image); create and boot the disk with the app ([upstream docs](docs/upstream/requirements-and-building.md)) |
| **Proton Experimental** set as VRChat's compatibility tool | ARM64 Steam otherwise installs VRChat's Android build, which no FEX change can fix; Proton Experimental selects the Windows depot |
| **Proton EasyAntiCheat Runtime** (Steam tool 1826330) | supplies the stock EAC launcher and client; must be installed explicitly |
| **the patched FEX in Steam's FEX tool**, from `fex-eac/` | Valve's FEX-2607 fails EAC's `ptrace` injection and signal semantics ([docs/eac-how-it-works.md](docs/eac-how-it-works.md)) |
| **the VRChat launch options**, including `WINE_CPU_TOPOLOGY` | load the EAC runtime and remove the pre-join stall ([fex-eac/README.md](fex-eac/README.md)) |
| **macOS microphone permission** for the app | without it the guest's audio capture floods errors and the game can freeze ([docs/eac-troubleshooting.md](docs/eac-troubleshooting.md)) |

## Quick start

1. Install steamac 1.8.x on an Apple silicon Mac and create and boot the SteamOS disk with the app, as described in
   [the upstream documentation](docs/upstream/requirements-and-building.md).
2. In Steam, install **VRChat** and set its compatibility tool to **Proton Experimental**; install the **Proton
   EasyAntiCheat Runtime**.
3. Enable SSH in the guest (Settings → Advanced), copy `fex-eac/` over, and run the setup:

   ```sh
   sh build-fex.sh            # clone FEX-2610, apply patches/, build, stage
   sh install-fex-tool.sh     # swap into the compat tool (Valve's files backed up)
   sh install-fex-tool.sh status
   ```

4. Set VRChat's Steam launch options from [fex-eac/README.md](fex-eac/README.md), then start the game.

## Results

Three sessions, 2026-10-08, M2 MacBook Air 16 GB, macOS 27.0, steamac 1.8.1/1.8.2.

| measurement | result |
|---|---|
| EAC launcher | `Launcher finished with: 301, 'Easy Anti-Cheat successfully loaded in-game'` in ~5 s, every session |
| anti-cheat session | `[AntiCheatClient] … bound`, `AntiCheat Session Begin: Success`; no `null client` |
| world entry | `Finished entering world`; no Photon time-out in the minutes after |
| region lookup | 4–6 s (`Locating best region` → `Got best network region`); no pre-join stall |
| join → world | 86 s first run (cold shader cache), 32 s second run (warm caches + FEX DiskCache) |
| frame rate, light world | ~26–30 fps steady at 1280×800, GPU-bound (see [docs/eac-performance.md](docs/eac-performance.md)) |

`fex-eac/` is built and tested (3 sessions). The launcher's render scale (`--render-scale 0.25–1.0`, default 1.0) is
built and does not change behaviour at the default. The libkrun and KosmicKrisp changes are in progress or
experimental — libkrun 0017 (microphone freeze) and KosmicKrisp 0041/0042 are in progress, and libkrun 0018 (damage-only
frame copy) is being revised after a verification found it leaves stale pixels; see [docs/fork.md](docs/fork.md) for
the per-change status. Verified only on an M2 MacBook Air, 16 GB, macOS 27.0; other Macs and macOS versions are
untested. Anti-cheat that blocks VMs will not work, and a Steam update of the FEX tool overwrites it with Valve's
build — re-run `sh install-fex-tool.sh`.

## Documentation

| page | contents |
|---|---|
| [docs/fork.md](docs/fork.md) | what changed in this fork and its status |
| [docs/eac-how-it-works.md](docs/eac-how-it-works.md) | the chain and why the FEX swap is needed |
| [docs/eac-troubleshooting.md](docs/eac-troubleshooting.md) | symptoms, checks and fixes |
| [docs/eac-performance.md](docs/eac-performance.md) | measurements and limits |
| [fex-eac/README.md](fex-eac/README.md) | FEX setup and launch options |
| [docs/upstream/README.md](docs/upstream/README.md) | upstream steamac's documentation (Apache-2.0, © FX GAMES FZ LLC) |

## License

This fork keeps steamac's Apache-2.0 license; see `LICENSE` and `NOTICE`. The scripts under `fex-eac/` are Apache-2.0, and
`fex-eac/patches/` are patches to FEX-Emu under FEX's MIT license.
