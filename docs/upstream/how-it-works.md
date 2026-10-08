# How it works

| Directory | Contents |
|---|---|
| `host/moltenvk/` | MoltenVK utmapp `geometry-shaders` @05604465 + patches: depth_clip_enable, YCbCr arrays, null descriptors, geometry shader emulation for zink/DXVK (vertex stride, instancing, adjacency, fans, SCALED formats, `gl_in`), transform feedback (DXVK stream output) and its queries (SO statistics), query result availability on copy (DXVK occlusion queries via Venus), atomics on vector components at buffer addresses (BDA, vkd3d-proton), texel buffers with offsets at any texel (vkd3d-proton), writes to small push-descriptor buffers with robustness2, variable-count descriptor arrays as runtime arrays (Metal kept 32 MB per vkd3d-proton heap array and program), allocation of auxiliary buffers, deferred release of Metal resources, patch hash in the pipeline cache UUID, `VK_NULL_HANDLE` descriptor sets in binds, fragment outputs converted to their color attachment's numeric type; tests in `repro/` run under Metal validation (also on KosmicKrisp: `REPRO_DRIVER=kosmickrisp`); `bench/run.sh <libdir>…` compares performance of changes between builds; `bench/shaders.sh <dump or pack>` measures a game's shader compilation (SPIR-V → MSL, MSL → Metal library, pipeline states; cold/warm, threads) from a MoltenVK shader dump or a 10% sample made with `bench/pack.py` |
| `host/kosmickrisp/` | KosmicKrisp (Mesa main @ce576c29) + open Mesa MRs and steamac patches (see “Vulkan driver”), built without LLVM at runtime (`-Dllvm=disabled`, `mesa_clc` from a first build), `-Db_ndebug=true`; macOS 26+ only |
| `host/libepoxy/` | libepoxy 1.5.10, upstream macOS Meson options, built for macOS 15.0 instead of copying a Homebrew bottle |
| `host/virglrenderer/` | virglrenderer UTM `macos-next` + merge with upstream main (venus-protocol 1.1.3) + LINEAR modifier, shm import as host memory, stubs for failed pipelines (draws dropped in virglrenderer), recreation of rejected cache, deferred shm unmap, thread QoS, Vulkan driver opened at runtime (`VKR_VULKAN_DRIVER`) |
| `host/libkrun/` | libkrun v1.19.6 + patches: `VIRTIO_GPU_F_BLOB_ALIGNMENT` (16K), SME mask for M4, 2D resources without virgl, `SET_SCANOUT_BLOB`, SHM blob mapping, Venus fence signaling, virglrenderer logs, `krun_display_resize` (resolution changes on the fly), vCPU/GPU thread QoS |
| `host/launcher/` | `steamac-vm` (Swift/AppKit): Metal window (optional MetalFX super resolution), “FX STEAM LAUNCHER” overlay with boot/shutdown progress, guest resolution = window size (× the screen's backing scale with Retina resolution) at constant DPI (EDID from the physical screen size), keyboard/mouse/tablet, the guest's Xbox 360 / DualSense / DualShock 4 pad from GameController.framework with rumble or a DualSense passed through as raw HID (`fx.pad`), network via gvproxy, VM restart on guest reboot, `--perf-stats` |
| `guest/kernel/` | Linux 7.2.9, everything built in, 4K pages, 16K blob-node alignment, Apple TSO for FEX; uhid, hidraw and `hid-playstation` (with the LED classes it needs) for the passed-through DualSense |
| `guest/mesa/` | Venus ICD for aarch64 (Proton, gamescope, zink) and x86_64/i386 (FEX graphics provider); x86_64 fault reporter for emulated games (`/usr/lib/steamac/x86_64/fault-report.so`) |
| `guest/initramfs/` | boot stage = “bootloader”: A/B slot selection with attempt counter, partsets, overlays for `/etc` and `/usr`; initial provisioning of the launcher-created disk (`steamac.provision=1`: static mkfs.fat, mke2fs, btrfstune in initramfs); `steamac.ssh=0` — no SSH server; launcher config payload (`steamac.config=1`) — new `steamos` password; `steamac.tz=` — the Mac's time zone in `/etc/localtime`; untouched procfs at `/run/steamac/proc` for Flatpak sandboxes |
| `guest/layer/` | VM layer over `/usr` (read-only erofs): file-based `splctl`, safe post-install for RAUC, `VARIANT_ID=steamdeck`, gamescope session on DRM, Desktop Mode (Plasma nested in gamescope), masks for Frame hardware services, `fx-progress-agent` progress agent (Rust, `guest/progress-agent/`, `fx.progress` virtio-console port) and its root services (`fx.clock`, `fx.sleep`, the uinput or uhid gamepad on `fx.pad`), short shutdown timeouts, QR-code Steam sign-in mode (Steam Deck client, while there is no remembered account), optional Steam client branch (`/etc/steamac/steam-client-branch`), Steam Shader Pre-Caching disabled by default (`steam-shader-defaults`) |
| `scripts/` | build of `work/out/steamos.img`: GPT with Valve's partition layout (esp, efi-A/B, rootfs-A/B, var-A/B, home); `scripts/test/provision-test-disk.sh` — dev test of provisioning against a disk from Docker; `scripts/test/vkd3d-tiled.sh` — dev end-to-end check of D3D12 tiled resources and the feature level (vkd3d-proton tests of Proton 11.0, `d3d12-caps`, `vk-minmax`) in a throwaway VM |

MoltenVK also fixes fragment helpers that discard from an otherwise empty SPIR-V block
(STEAMAC-1Q). `host/moltenvk/repro/msl_helpers.c` checks both direct and nested helpers:
discarded pixels stay clear and do not write storage buffers; surviving pixels render normally.
It renames user-defined `log10(float)` helpers to avoid Metal's builtin overload (STEAMAC-1R);
the same repro reads back the compute helper's results, not just successful pipeline creation.

`vkCmdBindDescriptorSets` with a `VK_NULL_HANDLE` among the sets (legal with graphics pipeline
libraries) no longer crashes the VM when the command buffer is submitted (Counter-Strike 2 binds
five sets with the fourth null, STEAMAC-25): as on RADV, a null set binds nothing and takes no
dynamic offsets (`repro/invalid_usage.c`).

A fragment output of another numeric type than its color attachment (a Left 4 Dead 2 pipeline from
DXVK writes an unsigned output to an `R32_SINT` attachment, STEAMAC-2C; Vulkan leaves the values
undefined) failed in Metal, and the pipeline's draws were skipped; it is now declared with the
attachment's type and its bits are written (`repro/frag_output.c`).

The SteamOS root filesystem is not modified: all changes come from initramfs and the layer. Thus
official Valve updates (RAUC + atomupd) install into the other slot and roll back normally — verified
with the 20260922 → 20260928 update and rollback.

