# Limitations

- DirectX 12 (vkd3d-proton): feature level 12_0 on KosmicKrisp (tiled resources tier 2: sparse binding and
  residency on Metal 4 placement sparse resources), 11_0 on MoltenVK; SM 6.0 on both (no SM 6.2+ on Apple GPUs).
  Within one render pass, depth written to unbound tiles of a sparse depth attachment stays in Apple's tile memory.
  Stellar Blade Demo (UE4) runs at 1280×800, 60 FPS on an M4 Max; the first run spends minutes compiling shaders. The
  x86 emulator in Proton ARM64 (FEX) stopped it once after 20 minutes (DEP check in its protected .exe).
- Audio: virtio-snd → CoreAudio (default device or one selected in settings), latency ≈65 ms on built-in
  speakers; the microphone is advertised but has not been tested.
- Anti-cheat systems that block VMs will not work.
- `logicOp` is unavailable (the private Metal API in the MoltenVK fork does not build); zink emits a warning.

