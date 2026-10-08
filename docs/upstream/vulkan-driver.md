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
| **KosmicKrisp** — Mesa on Metal 4 · macOS 26+ (`kosmickrisp`, default where available) | `host/kosmickrisp/`: Mesa main + open MRs (geometry shaders !44786, transform feedback !44928, tiled images in host-pointer memory !44929, device-local memory type !44221, linear render targets !44782/!44222) + steamac's patches (explicit LINEAR row pitch, LINEAR input attachments, `fillModeNonSolid`, which DXVK requires, 8-sample requests as 4, single texel alignment for texel buffers and timestamp pools over several Metal counter heaps, which vkd3d-proton requires; occlusion queries past one 32768-entry visibility buffer and timestamp pools on counter heaps shared by every guest device of the process (Metal allows 32 per process), so query pools of Dota 2 / Counter-Strike 2 no longer fail to create; sparse binding/residency on Metal 4 placement sparse resources and sampler min/max reduction emulated in shaders before Apple10, which give vkd3d-proton Tiled Resources Tier 2 and so D3D12 feature level 12_0) | faster: Stellar Blade Demo ~29 FPS against ~18 on MoltenVK on an M1 Max (split-screen video: `../media/stellar-blade-moltenvk-vs-kosmickrisp.mp4`); Steam UI, DXVK games and Stellar Blade Demo (D3D12, vkd3d-proton) run, its first run spends ~28 min compiling shaders on an M1 Max. macOS 26+ only; built only when the build host runs macOS 26+, otherwise MoltenVK is used. Known gaps (host repros): transform feedback with strip geometry shaders and its overflow counter (draft MR); within one render pass, depth writes to unmapped tiles stay in tile memory and later draws of the pass test against them (Tiled Resources Tier 2 allows this cache; vkd3d-proton's `test_sparse_depth_stencil_rendering` expects them dropped); no sparse 3D textures (Metal's 3D tiles are not Vulkan's standard 3D blocks), hence no Tiled Resources Tier 3 |
| **MoltenVK** — Metal 3 · macOS 15+ (`moltenvk`) | `host/moltenvk/`: the UTM fork + steamac's patches | every supported Mac; the default on macOS 15 and in builds without KosmicKrisp (source builds on macOS 15; the release DMG includes KosmicKrisp since 1.7) |

Switching changes the Venus driver identity (pipeline cache UUID), so Steam and games rebuild their
shader caches. A pipeline the host driver cannot build is a placeholder in virglrenderer: its draws
and dispatches are dropped, the driver never sees `VK_NULL_HANDLE`.

Every host-visible guest allocation is a POSIX shm whose file descriptors stay open in the VM
process (about four per mapped allocation), and a Finder launch starts with a soft limit of 256
descriptors: past a few dozen such allocations games lost their GPU context (STEAMAC-G, Left 4 Dead
2). The VM process raises its soft limit to `kern.maxfilesperproc` at start (logged as “file
descriptors: soft limit 256 → N”); virglrenderer logs the errno of a failed shm or descriptor
operation (“… failed: Too many open files (RLIMIT_NOFILE 256)”).

Checks without a VM: `host/kosmickrisp/build.sh` runs `host/moltenvk/probe` and the repros
(`REPRO_DRIVER=kosmickrisp host/moltenvk/repro/run.sh <dylib>`, through the Khronos loader, test
only) on the staged driver; `host/virglrenderer/build.sh` runs `venus_check` with each installed driver.

