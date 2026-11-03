# Vulkan driver

The host Vulkan driver behind Venus is chosen in **Settings → Advanced → Vulkan driver** (applies on
next start) or for one launch with `--vulkan-driver moltenvk|kosmickrisp`. The default is KosmicKrisp
where the Mac and the build have it (macOS 26+, a build made on macOS 26+), MoltenVK otherwise; a
driver chosen in Settings stays. virglrenderer opens the
driver at runtime (no Vulkan loader): the launcher sets `VKR_VULKAN_DRIVER` to
`@rpath/libMoltenVK.dylib` or `@rpath/libvulkan_kosmickrisp.dylib` before the VM starts. The boot
overlay shows the driver (“Venus → KosmicKrisp”); crash reports carry `vulkan_driver` and the
driver's patch revision.

Before Metal initialises, the launcher checks that its runtime compiler's module cache
(`DARWIN_USER_CACHE_DIR/<bundle id>/com.apple.metalfe`, including existing hash directories and
`.pcm` files) is writable. If permissions, ACLs or immutable file flags block it, the launcher logs
the failing path and redirects Metal to `~/Library/Caches/es.fxgam.steamac/metal-compiler`.
It does not delete the old cache or change its permissions. This uses Metal's optional cache-path
SPI; if the override is unavailable or the replacement is also unwritable, that is logged and
shader compilation errors remain reportable. `work/out/steamac-vm --selftest-metal-cache` reproduces
the `monolithic_metal.pcm: Operation not permitted` failure in a private immutable cache, then
verifies actual Metal source compilation with the replacement (no VM or Sentry events).

| Option | What it is | Pros and cons |
|---|---|---|
| **KosmicKrisp** — Mesa on Metal 4 · macOS 26+ (`kosmickrisp`, default where available) | `host/kosmickrisp/`: Mesa main + open MRs (geometry shaders !44786, transform feedback !44928, tiled images in host-pointer memory !44929, device-local memory type !44221, linear render targets !44782/!44222) + steamac's patches (explicit LINEAR row pitch, LINEAR input attachments, `fillModeNonSolid`, which DXVK requires, 8-sample requests as 4, single texel alignment for texel buffers and timestamp pools over several Metal counter heaps, which vkd3d-proton requires; R32G32B32 uniform texel buffers, as which vkd3d-proton views 12-byte structured buffers (such a view crashed the VM in The Witcher 3, STEAMAC-2Z); one Metal compiler shared by all guest devices, also on macOS 27 (on its betas 1–4 a compiler per device corrupted the heap when a guest session started, STEAMAC-31); zero-element texel buffer views become null descriptors and oversized views are clamped to Metal's per-format capacity after alignment (2^28 texels, three per RGB32 element), not to the conservative advertised limit: no invalid width reaches Metal and no asynchronous Venus creation failure kills the guest at first use (Spider-Man, Uncharted, Forza Horizon 6, Dragon's Dogma 2, STEAMAC-3J/3F/3S/3R); buffer-backed texel buffer views share their buffer's residency (over ~1M views exhausted the residency set and aborted, DOOM, Wolfenstein, STEAMAC-3V/3T); occlusion queries past one 32768-entry visibility buffer and timestamp pools on counter heaps shared by every guest device of the process (Metal allows 32 per process), so query pools of Dota 2 / Counter-Strike 2 no longer fail to create; timestamp writes outside render encoders coalesce resolves and split recordings every 16384 writes, avoiding Metal 4 command-buffer capacity failures (Ratchet & Clank: Rift Apart, #45; writes beyond 131072 still read 0); sparse binding/residency on Metal 4 placement sparse resources and sampler min/max reduction emulated in shaders before Apple10, which give vkd3d-proton Tiled Resources Tier 2 and so D3D12 feature level 12_0; a compile pool queues only the predicted plain/minmax program and compiles the other at first use; depth/stencil state is built once at creation independently of that choice, so bind never waits; translated shader caches include codegen settings such as `force_robustness`, and compiled Metal objects use bounded LRU tables with ownership-checked publication) | faster: Stellar Blade Demo ~29 FPS against ~18 on MoltenVK on an M1 Max (split-screen video: `../media/stellar-blade-moltenvk-vs-kosmickrisp.mp4`); Steam UI, DXVK games and Stellar Blade Demo (D3D12, vkd3d-proton) run, its first run spends ~28 min compiling shaders on an M1 Max. macOS 26+ only; built only when the build host runs macOS 26+, otherwise MoltenVK is used. Known gaps (host repros): transform feedback with strip geometry shaders and its overflow counter (draft MR); within one render pass, depth writes to unmapped tiles stay in tile memory and later draws of the pass test against them (Tiled Resources Tier 2 allows this cache; vkd3d-proton's `test_sparse_depth_stencil_rendering` expects them dropped); no sparse 3D textures (Metal's 3D tiles are not Vulkan's standard 3D blocks), hence no Tiled Resources Tier 3 |
| **MoltenVK** — Metal 3 · macOS 15+ (`moltenvk`) | `host/moltenvk/`: the UTM fork + steamac's patches | every supported Mac; the default on macOS 15 and in builds without KosmicKrisp (source builds on macOS 15; the release DMG includes KosmicKrisp since 1.7) |

Switching changes the Venus driver identity (pipeline cache UUID), so Steam and games rebuild their
shader caches. A pipeline the host driver cannot build is a placeholder in virglrenderer: its draws
and dispatches are dropped, the driver never sees `VK_NULL_HANDLE`.

The host drivers refuse the Vulkan inputs that would otherwise trip a Metal assertion that ends the
whole VM, and log the Vulkan arguments. KosmicKrisp: an image view with an empty or out-of-range layer
range, or between color formats of different block sizes, fails with `VK_ERROR_FORMAT_NOT_SUPPORTED`
(STEAMAC-62/3); a descriptor a shader uses but the pipeline layout lacks reads zero and drops writes
(STEAMAC-6H); image copies through a scratch buffer stay inside the mip and cover every volume slice
(STEAMAC-5E/5F); LINEAR images with more mips, layers, depth or samples than the format query
advertises fail in `vkCreateImage` (STEAMAC-3F/6Y); a cached precompiled NIR shader carries a BLAKE3
checked before it is deserialized, so a damaged entry is a cache miss instead of a crash (STEAMAC-6M/6K).
Mesa's barrier fallback on macOS let `util_queue_finish` hang a device teardown (shader disk cache);
`host/kosmickrisp/test/util_queue_finish.c` runs in the KosmicKrisp build. MoltenVK: a pipeline blending
an attachment Metal cannot blend (R32_UINT) fails (STEAMAC-3A); vertex-stage auxiliary buffers are
packed into Metal's 31 buffer slots (STEAMAC-67); full 32-bit specialization constant IDs work
(STEAMAC-6A). SPIRV-Cross fixes the MSL errors of STEAMAC-33/6D/6E/5M/5K/2N/5C/6V
(`host/moltenvk/repro/sentry-msl.sh` compiles each reconstructed shader). wmtr (DXMT) refuses only
garbage or freed object handles, ends encoders a game left open when its context goes away, and refuses
a render pipeline without a vertex function (STEAMAC-6S/6Z); `wmtr/tests/wmtr_dxmt_test.m` drives
DXMT's own command lists through the shipped executor on the GPU.

**Allow DXMT (D3D11 on the Mac's Metal), experimental.** Settings → Advanced → **Allow DXMT (D3D11 on
the Mac's Metal)** is off by default and applies on next start (`--wmtr-lib PATH` or `--no-wmtr` for one launch).
The app bundles `libwmtr_host.dylib`; the guest layer carries DXMT's
`{d3d11,dxgi,d3d10core,winemetal}.dll` and `winemetal.so`. With the toggle on, the launcher offers
libkrun's wmtr virtio-gpu context (patch 0018, capset 30) and boots with `steamac.gfx=dxmt`.
The guest's `steam-gfx-path` swaps DXMT into every installed x86-64 Proton and watches for Proton
installs and updates; turning the toggle off boots with `steamac.gfx=vulkan` and restores Proton's
original files. DXMT's dlls are x86-64 with a Wine unixlib, which only an x86-64 Proton loads (in Proton
11.0 (ARM64)'s ARM64EC Wine, winemetal's unix side does not come up and games report "could not load
d3d11.dll"), while this Steam client maps every Windows title to Proton 11.0 (ARM64) (`proton-stable`,
priority 85). Before Steam starts, `steam-compat-default` gives **only games whose profile selects DXMT**
a per-game `CompatToolMapping` entry for **Proton 10.0** at priority 250, and only on a DXMT-enabled
boot. Eligible titles are installed Windows games (`libraryfolders.vdf` and `appcache/appinfo.vdf`:
type game/demo/application, oslist with windows but not linux) without a user-owned tool choice.
Every other game keeps Steam's default tool, **Proton 11.0 ARM64**, with DXVK/vkd3d-proton on Venus.
The old blanket mappings are removed on the first Steam start; subsequent profile changes or
newly installed games are applied at the next Steam start. User edits in Steam remain untouched.
This is not just a DLL choice: some games crash under x86-64 Proton 10.0 + FEX regardless of the
DLLs used. Stellar Blade Demo (3564860), for example, ends SEH recursion with
`virtual_setup_exception stack overflow` there, but runs on Proton 11.0 ARM64.
Steam downloads Proton 10.0 (x86-64, under FEX in Steam Linux Runtime 3.0) on the first DXMT game
launch, and `steam-gfx-path --watch` puts DXMT into it. Proton Experimental and x86-64 Proton 11.0
need Steam Linux Runtime 4.0, which the arm64 client refuses to install ("Invalid platform").
The wildcard entry ("0") is never used: Steam applies a high-priority wildcard to its own tools
as well and emptied the FEX tool that way. Turning Allow DXMT off removes the script's mappings.
D3D12 on Proton 10.0 runs Wine under FEX instead of ARM64EC, costing CPU time.
Measured end to end on an M1 Max: Death's Door (Unity, D3D11)
starts on Proton 10.0, reports the renderer as "Apple M1 Max" and draws through the executor's layer.
Measured on that M1 Max (macOS 27, VM with 6 vCPUs / 8 GB at 120 Hz, KosmicKrisp): Death's Door's title
screen ~41 FPS on the default Proton 11.0 (ARM64) + DXVK path, ~117 on DXMT (held by the 120 Hz display);
d3d11bench with 100 draws and 16 full-screen passes 44 / 50 (Proton 10.0 + DXVK) / 100 FPS, with 2000
draws and 4 passes 133 / 139 / 115 (DXMT stops at the display's 120 Hz, the Vulkan path goes past it).
Medians of three 20 s runs launched through Steam (DXMT: six over two boots); details on the site.
DXMT's winemetal calls cross virtio-gpu to an
executor **inside the VM process on the Mac**, which presents a `CAMetalLayer` in the VM view.
That layer would cover the guest compositor's cursor; the launcher draws the game's own cursor above
it instead, with gamescope-compatible sizing. Cursor tracking is host-gated and DXMT-only: it runs
only when game focus permits the overlay and an actual wmtr layer is attached and visible, not
merely because Allow DXMT is on (see “Cursor on the DXMT path” under “How it works”).
D3D12 (vkd3d-proton) and native Vulkan remain on Venus; this path is for **D3D11/D3D10 only**, not D3D9.
DXMT still has compatibility gaps, including **no stream output**; expect first-launch shader
compilation and stutter rather than assuming Steam's Vulkan shader cache covers Metal.

**Security: while this is on, guest programs can run code on the Mac with your user's rights.**
The in-process executor and DXMT's raw-host-address protocol are not a VM security boundary.
That is why this path is off by default and experimental; an executor fault can also stop the VM.
Enable it only for guest software you trust.

Before starting the VM, the launcher sets `STEAMAC_WMTR=1`, `STEAMAC_WMTR_LIB` (the **Executor library**
override, otherwise `@rpath/libwmtr_host.dylib` from the app's Frameworks or `work/out/host/lib`) and
`WMTR_PRESENT=overlay`; all three are cleared when off. A missing/unloadable library or missing
`wmtr_host_abi_version` disables the path for that boot (logged and red in Settings; explicit
`--wmtr-lib` exits with the reason); libkrun rejects another ABI version. Problem/crash reports
carry `wmtr on|off`.

**Build DXMT.** On Apple silicon macOS, install Xcode's Metal toolchain
(`xcodebuild -downloadComponent MetalToolchain`), Rosetta (`softwareupdate --install-rosetta` for the
x86-64 Wine build tool), Homebrew and Docker/OrbStack with `linux/amd64` support for the client step.
The script installs `meson ninja cmake pkgconf zstd`. If Metal tools are unavailable,
`DXMT_PREBUILT_METAL` can name a directory containing `dxmt_command.metallib` and
`air_{msad,samplepos,tessellation}.air` built from the same pinned sources.

```sh
host/dxmt/build.sh fetch llvm native host pe client payload  # default steps, in order
scripts/build-image.sh layer                               # include the guest payload
host/launcher/build.sh                                     # bundle executor + refreshed layer
```

`fetch` verifies the pinned DXMT/DirectX-header tarballs and applies `host/dxmt/patches`;
`llvm` builds static LLVM 15.0.7 (about 10 minutes on an M2, reused while the pin matches).
`native` builds airconv in `work/build/dxmt-native`; `host` builds/tests/installs the executor to
`work/out/host/lib/libwmtr_host.dylib`. `pe` cross-builds the four DLLs into `work/out/dxmt-pe`;
`client` builds/tests `wmtr/build/x86_64-linux/winemetal.so` in Docker; `payload` stages
`work/out/dxmt/{x86_64-windows,x86_64-unix}` for `/usr/lib/steamac/dxmt/` in the guest layer.
Steps can be rerun individually; `host/dxmt/build.sh` with no arguments runs them all.

Credits: [StonkGuy](https://github.com/StonkGuy/steamac) for the wmtr remoting path and integration;
[DXMT by 3Shain (Feifan He)](https://github.com/3Shain/dxmt) for D3D11/D3D10 on Metal.
The cherry-picked fixes in `host/dxmt/patches` retain their authors: David Grinberg, osy,
Holly Hamilton, SteveTanSaMa, hanzpo, Mybono, NerRobDog, 时雨 and Jakey-Jakey
(details in the patched DXMT `STEAMAC-README.md`).

Every host-visible guest allocation is a POSIX shm whose file descriptors stay open in the VM
process (about four per mapped allocation), and a Finder launch starts with a soft limit of 256
descriptors: past a few dozen such allocations games lost their GPU context (STEAMAC-G, Left 4 Dead
2). The VM process raises its soft limit to `kern.maxfilesperproc` at start (logged as “file
descriptors: soft limit 256 → N”); virglrenderer logs the errno of a failed shm or descriptor
operation (“… failed: Too many open files (RLIMIT_NOFILE 256)”).

Checks without a VM: `host/kosmickrisp/build.sh` runs `host/moltenvk/probe` and the repros
(`REPRO_DRIVER=kosmickrisp host/moltenvk/repro/run.sh <dylib>`, through the Khronos loader, test
only) on the staged driver; `host/virglrenderer/build.sh` runs `venus_check` with each installed driver.
