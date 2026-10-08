# Fork notes

A fork of [steamac](https://github.com/fxgl/steamac) (base: 1.8.2) that adds support for genuine-Easy-Anti-Cheat x86 games,
verified with VRChat, plus launcher and stability fixes. It is standalone: nothing here is sent to steamac, libkrun, Mesa,
FEX or Proton. Not affiliated with FX GAMES, Valve, Epic or VRChat.

## Changes

| change | where | why | status |
|---|---|---|---|
| patched FEX-2610 (patches 0001–0020), `build-fex.sh`, `install-fex-tool.sh`, `guest-tune.sh` | [`fex-eac/`](../fex-eac/README.md) | Valve's FEX-2607 in Steam's compatibility tool fails EAC's ptrace injection and signal semantics ([eac-how-it-works.md](eac-how-it-works.md)) | built, tested (3 sessions) |
| triple-buffered present | launcher (Swift) | replaces a `waitUntilCompleted()` per frame | built |
| `--render-scale` (0.25–1.0) with MetalFX upscaling | launcher (Swift) | render below native and upscale, to move the frame-time cost off the GPU while keeping UI at real-world size | built (default stays 1.0 = unchanged) |
| libkrun log level default `error` | launcher | less logging overhead | built |
| Metal HUD library loaded only when the HUD is on | launcher | avoids loading it needlessly | built |
| direct-to-display presentation, key releases never dropped, HID off the main thread, Retina EDID DPI, stall-indicator timer, pad-port lock, `framebufferOnly` restore, Clipboard-writer restart | launcher | findings of a code audit of 1.8.2 | built, re-verified (adversarial repros pass) |
| kernel parameter `virtio_snd.msg_timeout_ms=10000` | guest default | a slow microphone start otherwise trips a 1 s guest time-out and freezes the game ([eac-troubleshooting.md](eac-troubleshooting.md)) | workaround, built |
| asynchronous capture start, no global lock across CoreAudio calls | libkrun patch 0017 | root cause of the microphone freeze | fixed, re-verified |
| copy only the damage owed to each frame buffer | libkrun patch 0018 | avoids copying the whole scanout per present | fixed, re-verified (0/256 trials stale across 2/3/4 rotating buffers; pre-fix control 64/64; `cargo test --features gpu` 14/14) |
| opt-in asynchronous pipeline compilation (`MESA_KK_ASYNC_PIPELINES=1`, `=2` drops the draw) | KosmicKrisp patch 0041 | Venus exposes no `VK_EXT_graphics_pipeline_library` and KosmicKrisp compiles pipelines synchronously, blocking the frame | experimental; UAF fixed, re-verified (repro exit 139 → 0, ASan clean). Set in the **launcher's** environment, not the guest launch options |
| 5 robustness fixes from the same audit (XFB bounds, `container_of` guard, minmax UAB scan, NULL plane layouts, sparse underflow) | KosmicKrisp patch 0042 | defensive fixes | fixed, re-verified |

The launcher changes are verified only on an M2 MacBook Air, 16 GB, macOS 27.0, with steamac 1.8.1/1.8.2. The
launcher build is clean (`swift build -c release`, 0 warnings). One logging-only race remains open and is not claimed
fixed: `hidInputs` is incremented on `hidQueue` and reset on the main thread.

## Audit

12 parallel code audits of steamac 1.8.2, every claim re-verified. They also confirmed upstream issues this fork does not
yet fix, including a full-frame scanout copy per present in libkrun.

## Build and deploy a local app

1. Start from the official steamac release bundle (1.8.1/1.8.2).
2. Replace its launcher with the rebuilt launcher from this fork (`build.sh`).
3. Ad-hoc sign the app with the entitlements of the release build.
4. macOS treats the re-signed app as a new app: it asks again for microphone permission and keychain access.

The libkrun 0017/0018 and KosmicKrisp 0041/0042 changes are separate components of the bundle; they are included now
that their adversarial re-verifications pass (see the status column above). 0041 remains opt-in at run time.

## Documents

| page | contents |
|---|---|
| [eac-how-it-works.md](eac-how-it-works.md) | the chain and why the FEX swap is needed |
| [eac-troubleshooting.md](eac-troubleshooting.md) | symptoms, checks and fixes |
| [eac-performance.md](eac-performance.md) | measurements and limits |
| [fex-eac/README.md](../fex-eac/README.md) | FEX setup and launch options |
