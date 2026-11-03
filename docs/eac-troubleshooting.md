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
| `206, 'Failed to load the anti-cheat module'` right after `Starting Wine module mapping` (`Failed to map the anti-cheat module`) | the game runs on an ARM64 Proton (`Proton 11.0 (ARM64)`). EAC's module is an x86-64 Linux library and maps only into an x86-64 Wine: set the compatibility tool to **Proton Experimental** (x86-64, runs through FEX) |
| `210 'Unexpected error. (#1)'` about 3 s after `Starting Wine module mapping` | the installed FEX predates patch 0033: re-run `sh build-fex.sh` and `sh install-fex-tool.sh` |
| worked before, fails after a Steam update | the update replaced the FEX tool with Valve's build; re-run `sh install-fex-tool.sh` |
| the Android build starts (or the game does not use Proton) | set the game's compatibility tool to Proton Experimental and let Steam install the Windows depot |
| EAC runtime errors at start | install **Proton EasyAntiCheat Runtime** (1826330) and check `EAC_LAUNCHERDIR` / `PROTON_EAC_RUNTIME` in the launch options |
| ~60 s stall before joining (region lookup) | keep `WINE_CPU_TOPOLOGY=16:…` in the launch options; with it the region lookup took 4–6 s |
| game freezes after a microphone start, or after an in-game settings/resolution change that reinitialises the audio device; guest log shows `virtio_snd: control message (0x00000103/0x00000104/0x00000105) timeout` and the game's main thread spins | the CoreAudio capture start blocked while holding the per-stream `capture` mutex, which the snd pump thread also takes every iteration (while it holds `hosts`, which the control thread needs to answer PREPARE/START/STOP) — so *every* virtio-snd control message timed out. Root-cause fix: libkrun **0019**, which starts the capture unit outside that lock ([fork.md](fork.md)) |
| log flooded (~250 lines/s) with capture errors | macOS microphone permission is missing for the app; grant it |
| stutter in new areas | expected: the guest advertises no `VK_EXT_graphics_pipeline_library` (nor `VK_EXT_vertex_input_dynamic_state`), so DXVK/vkd3d compile each new pipeline in full, synchronously on the calling thread. Venus *does* advertise GPL (Mesa `vn_physical_device.c`), but KosmicKrisp does not implement it (`kk_physical_device.c`), and Venus only keeps a passthrough extension when the host reports it, so the guest never sees it ([fork.md](fork.md) has the evidence; `scripts/test/vkd3d-tiled.sh run` asserts it). KosmicKrisp's compile itself (50–100 ms typical; a single Metal compile has been observed over 60 s, and a 349 s GPU-idle stall at world load). **What repeats each run is the host's Metal compile:** the DXVK state cache persists (in the Wine prefix, `AppData/Local/dxvk/*.dxvk.bin`; `steamapps/shadercache/<appid>/DXVK_state_cache` stays empty because this build does not write there) and so does the KosmicKrisp NIR-to-MSL cache (patch 0044, `~/.cache/mesa_shader_cache` on the Mac), but KosmicKrisp's Metal libraries are rebuilt in every VM session because nothing keeps a persistent Metal cache or binary archive. Asynchronous compilation (`MESA_KK_ASYNC_PIPELINES=1`, KosmicKrisp 0041) previously crashed the VM under VRChat; the real gap was the draw gate watching the bound VS rather than the program the flush reads, fixed by KosmicKrisp 0047 and **verified live** under `MESA_KK_ASYNC_PIPELINES=2` ([fork.md](fork.md)) |
| rebuilt app asks for microphone / keychain again | expected after re-signing ([fork.md](fork.md)) |
