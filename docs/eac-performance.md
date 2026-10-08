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

## What limits it

The game is **GPU-bound**. `powermetrics` shows the GPU 100% active at ~1.21 GHz and ~3.6 W; the top state (1.40 GHz) is
not reached, so the fanless chip is power-limited. Emulation is not the limit (CPU headroom, benchmarks equal to Asahi).
New shader pipelines stall the frame until compiled (see [eac-troubleshooting.md](eac-troubleshooting.md)).

## What may help (not measured here)

These follow from the GPU-bound, power-limited result but were **not** benchmarked for this fork:

* lower in-game graphics settings and render scale
* a guest resolution below the Retina mode (Retina has about 4× the pixels of the matching non-Retina mode)
* MetalFX, fullscreen / Game Mode, closing other Mac apps, keeping the charger connected
