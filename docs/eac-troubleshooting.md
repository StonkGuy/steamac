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
| game freezes after a microphone start; guest log shows `virtio_snd: control message (0x00000104) timeout` | the CoreAudio capture start blocked over 1 s, PipeWire was killed and the audio thread spins. The fork defaults `virtio_snd.msg_timeout_ms=10000`; the root-cause fix is libkrun 0017 |
| log flooded (~250 lines/s) with capture errors | macOS microphone permission is missing for the app; grant it |
| stutter in new areas | expected: Venus has no `VK_EXT_graphics_pipeline_library` and KosmicKrisp compiles each new pipeline synchronously (50–100 ms typical, one over 60 s); Metal caches results on disk, so revisits are smooth. Asynchronous compilation (`MESA_KK_ASYNC_PIPELINES=1`) is experimental, in progress |
| rebuilt app asks for microphone / keychain again | expected after re-signing ([fork.md](fork.md)) |
