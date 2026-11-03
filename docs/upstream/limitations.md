# Limitations

- DirectX 12 (vkd3d-proton): feature level 12_0 on KosmicKrisp (tiled resources tier 2: sparse binding and
  residency on Metal 4 placement sparse resources), 11_0 on MoltenVK; SM 6.0 on both (no SM 6.2+ on Apple GPUs).
  Within one render pass, depth written to unbound tiles of a sparse depth attachment stays in Apple's tile memory.
  Stellar Blade Demo (UE4) runs at 1280×800, 60 FPS on an M4 Max; the first run spends minutes compiling shaders. The
  x86 emulator in Proton ARM64 (FEX) stopped it once after 20 minutes (DEP check in its protected .exe).
- Audio: virtio-snd → CoreAudio (default device or one selected in settings), latency ≈65 ms on built-in
  speakers. At boot, playback exposes up to 12 PCM channels from the output device's configured speaker
  layout, including height speakers (e.g. 5.1.2 or 7.1.4) when CoreAudio identifies them explicitly. An
  unclassified/discrete layout, or a device with more than 32 channels, gets mono/stereo; 8 channels alone
  do not imply 5.1.2, and there is no Dolby Atmos encoding. For a multichannel layout, `audio-layout`
  reads the VirtIO ALSA playback map before WirePlumber starts and replaces SteamOS's stereo policy with
  the matching Pro Audio output (timer scheduling, separate playback/capture groups: the host devices
  have independent clocks); a stereo host keeps the stock SteamOS profile. Playback follows a new
  default or unplugged output at once and rechecks the layout once a second, routing to the physical
  speakers and downmixing missing ones with headroom. Guest channel maps stay fixed for a boot
  (virtio-snd/ALSA cache them): restart the VM to expose more channels or a new layout to games.
  `STEAMAC_SND_DUMP` records guest PCM before speaker mapping. 5.1.2 was tested on hardware, 7.1.4
  only by automated mapping tests; the microphone stays mono/stereo and has not been tested.
- Anti-cheat systems that block VMs will not work.
- `logicOp` is unavailable (the private Metal API in the MoltenVK fork does not build); zink emits a warning.
