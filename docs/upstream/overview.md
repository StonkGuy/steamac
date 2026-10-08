# steamac — Valve's official ARM64 SteamOS (the Steam Frame image) in a VM on Apple Silicon

**English** · [Русский](README.ru.md)

On macOS 15 (Sequoia), Valve's actual SteamOS for Steam Frame runs in a lightweight VM on
Hypervisor.framework (libkrun) with GPU acceleration via Venus.

```
game (DX9/10/11) ─ DXVK (Proton 11, x86 via FEX) ─ Vulkan
   └─ guest Mesa Venus ─ virtio-gpu (blob, 16K alignment)
        └─ libkrun ─ virglrenderer (Venus) ─ MoltenVK | KosmicKrisp ─ Metal
```

Vulkan on Metal — MoltenVK from the UTM fork (geometry shaders, robustness2) +
`VK_EXT_depth_clip_enable` (PR #2712) + our fixes, by default. KosmicKrisp (Mesa's Vulkan driver on
Metal 4) is an experimental alternative on macOS 26+ (see “Vulkan driver”).

