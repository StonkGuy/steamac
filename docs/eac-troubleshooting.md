# EAC troubleshooting

## Verify success

* EAC launcher log (`anticheatlauncher.log`, in the `EAC_LAUNCHERDIR` directory): `System name 'linux64'`, `Starting Wine
  module mapping`, then `Launcher finished with: 301, 'Easy Anti-Cheat successfully loaded in-game'` (about 5 s).
* VRChat `output_log_*.txt`: `[AntiCheatClient] … bound`, `AntiCheat Session Begin: Success`, `Finished entering world`,
  and **no** `null client` (that means a stub loaded, not EAC).

Never attach a debugger, `strace` or `perf` to the running game: EAC reports it. Use the logs only.

## Symptoms

| symptom | check |
|---|---|
| launcher stops at `Starting Wine module mapping`, or the result is not `301` | the patched FEX is probably not installed: `sh install-fex-tool.sh status`. Valve's FEX-2607 fails EAC ([eac-how-it-works.md](eac-how-it-works.md)) |
| worked before, fails after a Steam update | the update replaced the FEX tool with Valve's build; re-run `sh install-fex-tool.sh` |
| the Android build starts (or the game does not use Proton) | set the game's compatibility tool to Proton Experimental and let Steam install the Windows depot |
| EAC runtime errors at start | install **Proton EasyAntiCheat Runtime** (1826330) and check `EAC_LAUNCHERDIR` / `PROTON_EAC_RUNTIME` in the launch options |
| ~60 s stall before joining (region lookup) | keep `WINE_CPU_TOPOLOGY=16:…` in the launch options; with it the region lookup took 4–6 s |
| game freezes after a microphone start, or after an in-game settings/resolution change that reinitialises the audio device; guest log shows `virtio_snd: control message (0x00000103/0x00000104/0x00000105) timeout` and the game's main thread spins | the CoreAudio capture start blocked while holding the per-stream `capture` mutex, which the snd pump thread also takes every iteration (while it holds `hosts`, which the control thread needs to answer PREPARE/START/STOP) — so *every* virtio-snd control message timed out. Root-cause fix: libkrun **0019**, which starts the capture unit outside that lock ([fork.md](fork.md)) |
| log flooded (~250 lines/s) with capture errors | macOS microphone permission is missing for the app; grant it |
| stutter in new areas | expected: Venus has no `VK_EXT_graphics_pipeline_library` and KosmicKrisp compiles each new pipeline synchronously (50–100 ms typical; a single Metal compile has been observed over 60 s, and a 349 s GPU-idle stall at world load). **The pipeline caches do not persist** — the DXVK state cache and the FOSSILIZE bucket stay empty and the Venus/Mesa cache holds headers only — so the work repeats each run. Asynchronous compilation (`MESA_KK_ASYNC_PIPELINES=1`, KosmicKrisp 0041) is **experimental and currently crashes the VM** under VRChat; the `KK_ASYNC_FAILED` fix (0043) was tested live and did not stop it, so the lever is off ([fork.md](fork.md)) |
| rebuilt app asks for microphone / keychain again | expected after re-signing ([fork.md](fork.md)) |
