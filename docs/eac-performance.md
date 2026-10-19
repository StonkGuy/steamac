# Performance

Measured on an Apple M2 MacBook Air (16 GB, fanless), macOS 27.0, VRChat, three sessions (2026-10-08).

| measurement | result |
|---|---|
| frame rate, light world | ~26–30 fps steady, game renders 1280×800 |
| vs Asahi (same M2, native driver) | high phase 20–30% below, without Asahi's deep dips |
| join → world | 86 s first run (cold shader cache), 32 s second run (warm caches + FEX DiskCache) |
| region lookup | 4–6 s |
| CPU | not saturated: game main thread ~23% of a core, vCPUs ~55% each |
| FEX micro-benchmarks | 50M seq-cst atomics 219–233 ms (Asahi 237 ms); cold translation 14 ms |

## Graphics paths (steamac 1.9, 2026-10-11 and 2026-10-13)

Same Mac. Single runs, so treat the numbers as indicative: Mac memory pressure (swap) varied between runs.

| measurement | Vulkan (DXVK, KosmicKrisp) | DXMT |
|---|---|---|
| VRChat, first minute / idle / moving, fps | 29 / 39 / 44 | 76 / 78 / 78 (DXMT pin of 1.9 main); 66 / 55 / 44 under heavy swap (current pin) |
| VRChat EAC | `301`, session, world | `301`, session, world |
| d3d11bench, 2000 draws, overdraw 4, avg fps (2 runs each, interleaved) | 56.5 / 59.3 (may be capped at 60 Hz) | 111.3 / 111.4 |
| d3d11bench, 100 draws, overdraw 16, avg fps | 19.6 / 19.6 | 28.3 / 30.2 |
| Unigine Superposition 1.1, 720p Low | did not fit: app memory passed 14 GB (8 GiB VM) | same |

Scope: the VRChat row is title-specific; d3d11bench is synthetic and shows the general per-draw and fill-rate cost of
each path.

## Real titles, DXMT vs stock steamac 1.9.1 (2026-10-18)

Same Mac, 1280x720 windowed, one VM boot per run, runs interleaved, each title's own benchmark output. Control: the
stock FX Steam Launcher 1.9.1 release with default settings (8 GiB guest, 4 vCPUs, KosmicKrisp), Valve's FEX, stock
Proton Experimental (DXVK). DXMT: a build with only the DXMT path added, same guest, same Valve FEX. Posted in
[fxgl/steamac#14](https://github.com/fxgl/steamac/issues/14).

| title (D3D11) | DXMT | Vulkan, stock 1.9.1 | scope |
|---|---|---|---|
| Star Swarm Stress Test, Low, Follow, 120 s: avg / 1% low fps | 102.5 / 39 (earlier series 110.4 / 104.0 / 103.9 avg) | 59.1 / 15.8, 51.6 / 13.3 | general mechanism (per-draw cost), measured on this title |
| Unity SpaceshipDemo 2021.2 (HDRP), `--benchmark`, High: avg / worst fps | 46.7 / 18.3, 47.1 / 27.6 | no result: GPU hang 60-100 s in on every warm run (5 of 5) | title-specific |
| Unigine Superposition | renders black shadow areas (no D3D11 stream output in DXMT) | renders correctly | title-specific; no valid speed comparison |

On Star Swarm, the Vulkan path's low frames follow the scenario's heavy-batch stretches; DXMT holds about 10 ms frames
there. Frame pacing (p99 frame time) was 18-26 ms on DXMT and 64-75 ms on Vulkan.

d3d11bench is the fork's synthetic Direct3D 11 test (`wmtr/tests/d3d11bench.cpp`). With an 8 GiB VM, the app's memory
use on a 16 GB Mac is 9-10 GB at idle, so heavy titles can run out of memory on either path.

## What limits it

The game is **GPU-bound**. `powermetrics` shows the GPU 100% active at ~1.21 GHz and ~3.6 W; the top state (1.40 GHz) is
not reached, so the fanless chip is power-limited. Emulation is not the limit (CPU headroom, benchmarks equal to Asahi).
New shader pipelines stall the frame until compiled (see [eac-troubleshooting.md](eac-troubleshooting.md)).

## What may help (not measured here)

These follow from the GPU-bound, power-limited result but were **not** benchmarked for this fork:

* lower in-game graphics settings and render scale
* a guest resolution below the Retina mode (Retina has about 4× the pixels of the matching non-Retina mode)
* MetalFX, fullscreen / Game Mode, closing other Mac apps, keeping the charger connected
