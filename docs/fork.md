# Fork notes

A fork of [steamac](https://github.com/fxgl/steamac) (base: 1.9; results on 1.9 are marked, older ones were measured on 1.8.1/1.8.2) that adds support for genuine-Easy-Anti-Cheat x86 games,
verified in game with VRChat (Mini Royale's EAC also reaches `301`; results differ game by game), plus launcher and stability fixes. Nothing here is sent to libkrun, Mesa, FEX or Proton by this fork;
steamac changes are discussed with its maintainer directly. Bug reports are welcome here (see the README). Not affiliated with FX GAMES, Valve, Epic or VRChat.

## Changes

| change | where | why | status |
|---|---|---|---|
| patched FEX-2610 ([StonkGuy/FEX](https://github.com/StonkGuy/FEX/tree/eac-2610) tag `eac-2610.3`: patches 0001–0026, 0033, 0035–0039, 0041–0053), `build-fex.sh`, `install-fex-tool.sh`, `guest-tune.sh` | [`fex-eac/`](../fex-eac/README.md) | Valve's FEX-2607 in Steam's compatibility tool fails EAC's ptrace injection and signal semantics ([eac-how-it-works.md](eac-how-it-works.md)) | built, tested (3 sessions) — **the verified setup** |
| triple-buffered present | launcher (Swift) | replaces a `waitUntilCompleted()` per frame | experimental |
| `--render-scale` (0.25–1.0) with MetalFX upscaling | launcher (Swift) | render below native and upscale, to move the frame-time cost off the GPU while keeping UI at real-world size | experimental (default stays 1.0 = unchanged) |
| libkrun log level default `error` | launcher | less logging overhead | experimental |
| Metal HUD library loaded only when the HUD is on | launcher | avoids loading it needlessly | experimental |
| direct-to-display presentation, key releases never dropped, HID off the main thread, Retina EDID DPI, stall-indicator timer, pad-port lock, `framebufferOnly` restore, Clipboard-writer restart | launcher | findings of a code audit of 1.8.2 | experimental |
| kernel parameter `virtio_snd.msg_timeout_ms=10000` | guest default | a slow microphone start otherwise trips a 1 s guest time-out and freezes the game ([eac-troubleshooting.md](eac-troubleshooting.md)) | workaround, built |
| asynchronous capture start, no global lock across CoreAudio calls | libkrun patch 0018 | root cause of the microphone freeze | experimental |
| start the capture unit **outside** the `capture` mutex | libkrun patch 0020 | `AudioOutputUnitStart` can block (microphone open / permission prompt) while holding a lock the snd pump takes every iteration, wedging *every* virtio-snd control message — the real cause of the microphone freeze | verified live |
| copy only the damage owed to each frame buffer | libkrun patch 0019 | avoids copying the whole scanout per present | experimental |
| opt-in asynchronous pipeline compilation (`MESA_KK_ASYNC_PIPELINES=1`, `=2` drops the draw) | KosmicKrisp patch 0041 | the guest advertises no `VK_EXT_graphics_pipeline_library` (nor `VK_EXT_vertex_input_dynamic_state`), so DXVK/vkd3d compile each pipeline in full on the calling thread, synchronously; KosmicKrisp has no pipeline-library implementation to hand the work to, so this fork adds the async path behind an option. The extension is absent even though Venus advertises it (Mesa `vn_physical_device.c`: `.EXT_graphics_pipeline_library = true`), because KosmicKrisp does not (Mesa `kk_physical_device.c` `kk_get_device_extensions` has no such entry) — Venus filters passthrough extensions against the host with `vkEnumerateDeviceExtensionProperties` (`vn_physical_device_init_supported_extensions`), so the guest never sees it. Evidence: `scripts/test/vkd3d-tiled.sh run` asserts the four names absent in the guest; `vulkaninfo` on the running guest confirms it. If the host driver ever advertises it, DXVK's fast-pipeline path becomes reachable and this option's premise no longer holds | **experimental; off by default, not recommended.** The 0041 SIGSEGV in the render encoder survived 0043; 0047 addresses the real gap (the draw gate watched the bound VS instead of the program the flush reads), but no live run on record is shown to contain 0047. A `=2` run (90 s passive sample, ~29 fps, several world loads, no SIGSEGV) was on a build not shown to contain 0047. An idle in-world run with `=1` was clearly worse than off. Set in the **launcher's** environment, not the guest launch options |
| 5 robustness fixes from the same audit (XFB bounds, `container_of` guard, minmax UAB scan, NULL plane layouts, sparse underflow) | KosmicKrisp patch 0042 | defensive fixes | experimental |
| drop a draw whose async render pipeline state FAILED instead of recording it with a NULL state | KosmicKrisp patch 0043 | the mode-1 async path recorded the draw with `pipe->gfx.render == NULL` and called `mtl_render_set_pipeline_state(enc, NULL)` → the nil-Metal **VM crash** (`AGXMetal … drawIndexedPrimitives:`). Reachable only with `MESA_KK_ASYNC_PIPELINES=1` | experimental (async stays **off**) |
| opt-in frame-latency toggle (**Settings > Advanced "Frame latency"**; `dxgi.maxFrameLatency = 1`) | launcher (Swift) + guest `steam-frame-latency` | stock DXVK option that caps the frames the CPU runs ahead of the GPU, for lower input latency at some cost in throughput; the guest writes it into the DXVK config (`$DXVK_CONFIG_FILE`, else `$HOME/vrc/dxvk.conf`) so no game needs a launch option | experimental; **off by default**, the guest's DXVK config is left unchanged. Vulkan path only (the DXMT path has its own config) |
| wire the Mesa on-disk shader cache (`disk_cache_create`) | KosmicKrisp patch 0044 | the Mesa NIR-to-MSL step is cached on the Mac across runs; Metal's own library compile still repeats each session (the DXVK state cache persists in the Wine prefix, `AppData/Local/dxvk`) | experimental |
| bound the indirect-deref branch tree (`nir_lower_indirect_derefs_to_if_else_trees`, threshold 16) | KosmicKrisp patch 0046 | bounds a compile-time blow-up that shows as a long synchronous-compile stall | experimental |
| gate the async draw on the program the draw actually uses (not the bound VS) | KosmicKrisp patch 0047 | the gate watched the bound VS; when a reduction sampler bound the min/max companion (0029), the companion could be async on its own and the draw was recorded with a NULL render state → the surviving 0041 SIGSEGV. Also makes the flush/dispatch bails structural instead of `b_ndebug`-compiled-out asserts | code-complete; live verification with 0047 not yet on record |
| DXMT graphics path (**Settings > Advanced > Graphics path**): Direct3D 11/10 (and experimentally 12) through DXMT, its Metal calls remoted over a dedicated virtio-gpu context to a host executor. DXMT is [StonkGuy/dxmt](https://github.com/StonkGuy/dxmt) branch `steamac` (upstream plus fixes adopted from other DXMT forks with their authors, ledger in its STEAMAC-README.md), pinned by commit in `host/dxmt/pin.env` | `wmtr/`, libkrun patch 0021, `host/dxmt/patches/` (generated by `scripts/export-dxmt-patches.sh`), guest `steam-gfx-path`, launcher | fewer translation layers than DXVK + Venus + virglrenderer + Vulkan-to-Metal | **verified live on 1.9**: VRChat EAC `301` + session + world on DXMT (DXMT pin upstream e94c312 + steamac d5576ef, 2026-10-11); measured ahead of Vulkan ([eac-performance.md](eac-performance.md)). Default stays Vulkan |
| `steam-gfx-path`: Direct3D 12 routed to DXMT when the payload has it, ARM64 Protons left stock, Proton's kept originals deleted only once they are back (a DXMT file from another app build is recognised; with no original kept, a warning names Steam's Verify integrity) | guest `steam-gfx-path` | switching paths, or between two app builds on one disk, could leave DXMT's d3d11/dxgi in Proton and Direct3D 11 games failing on Vulkan | built; `scripts/test/steam-gfx-path.sh` (cases foreign/noorig fail before the fix, pass after) |
| virtio-snd: control requests and the playback pump no longer stall each other; a failed control request completes the stream's queued I/O; control deadline 8 s | libkrun patches 0022, 0023 | a slow CoreAudio start under load otherwise left the guest driver refusing every PREPARE ("invalid I/O queue state") | experimental; offline unit tests pass, live verification of 0023 pending |
| virtio-input worker runs at user-interactive QoS, like the vCPUs and the GPU worker (0013) | libkrun patch 0024 | key presses and pointer motion reach the guest through that thread; a busy host should not delay them | experimental; unit test for the QoS helper passes, the thread's runtime QoS is unverified |
| DXMT path: arena blobs idle after a load peak go back to the Mac. Idle blobs beyond a 128 MiB warm reserve are released once they have been idle for 1 s (`WMTR_ARENA_KEEP_MB`, `WMTR_ARENA_TRIM_MS`; `WMTR_ARENA_KEEP_MB=-1` keeps them all). On release the guest closes the blob and the wmtr context drops its own mapping | `wmtr/` client and transports, libkrun patch 0028 | each blob is shared memory mapped three times (VMM, Hypervisor.framework stage-2, executor), so a level-load peak otherwise stays in `phys_footprint` until the game exits | experimental; `wmtr_loopback` (footprint 504 -> 203 MiB after the peak) and the libkrun `wmtr` detach test pass; a live footprint check in the VM is still owed |

The app-side changes (the launcher changes above, libkrun 0018/0019/**0020**, KosmicKrisp 0041/0042/0047) are **experimental**, except libkrun 0020, which is verified live. The async-pipeline crash seen with `MESA_KK_ASYNC_PIPELINES=1` (SIGSEGV in the render encoder under VRChat) is addressed by KosmicKrisp 0047, which gates the async draw on the program the draw uses rather than the bound VS; 0041 and 0043 alone did not address it. No live run on record is shown to contain 0047. Async pipelines stay **off** by default and are not recommended: an idle in-world run with `=1` was clearly worse than off, and no stutter improvement is shown. Treat the release app plus `fex-eac/` as the recommended setup; each change was built and adversarially re-verified in isolation, and the live runs of the combined app are listed above. They are verified only on an M2 MacBook Air, 16 GB, macOS 27.0, with steamac 1.8.1/1.8.2. The launcher build is
clean (`swift build -c release`, 0 warnings). One logging-only race remains open and is not claimed fixed: `hidInputs` is
incremented on `hidQueue` and reset on the main thread.

## Audit

12 parallel code audits of steamac 1.8.2, every claim re-verified. They also confirmed upstream issues this fork does not
yet fix, including a full-frame scanout copy per present in libkrun.

## Build and deploy a local app

1. Start from the official steamac release bundle (1.8.1/1.8.2).
2. Replace its launcher with the rebuilt launcher from this fork (`build.sh`).
3. Ad-hoc sign the app with the entitlements of the release build.
4. macOS treats the re-signed app as a new app: it asks again for microphone permission and keychain access.

The libkrun 0018/0019 and KosmicKrisp 0041/0042/0047 changes are separate components of the bundle. They are **experimental**: each passed its adversarial re-verification. The async-pipeline crash of the combined app is addressed by 0047 in code (see above), without live confirmation on record. KosmicKrisp 0041 remains opt-in at run time (`MESA_KK_ASYNC_PIPELINES`, off by default, not recommended). Prefer the release app plus `fex-eac/`.

## Documents

| page | contents |
|---|---|
| [eac-how-it-works.md](eac-how-it-works.md) | the chain and why the FEX swap is needed |
| [eac-troubleshooting.md](eac-troubleshooting.md) | symptoms, checks and fixes |
| [eac-performance.md](eac-performance.md) | measurements and limits |
| [fex-eac/README.md](../fex-eac/README.md) | FEX setup and launch options |
