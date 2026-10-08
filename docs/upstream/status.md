# Status

Verified:

- SteamOS boots to `graphical.target`, autologin, gamescope session; networking (DHCP via gvproxy,
  downloading a 583 MB Steam client update), SSH;
- Venus in the guest: `Virtio-GPU Venus (Apple M4 Max)`, Vulkan 1.4; render test (compute + clear/copy)
  and display output through KMS match the reference pixel for pixel;
- all required DXVK features from Proton 11 / DXVK 3.x are visible in the guest (geometryShader,
  shaderCullDistance, depthClipEnable, robustness2 + nullDescriptor, maintenance5/6, …);
- keyboard, tablet, mouse, and the virtual pad are visible in SteamOS; as a DualSense, Steam's SDL
  maps it as `PS5 Controller` (type PS5) and shows PlayStation glyphs; the pad appears, disappears
  and changes kind while the VM runs; rumble from SDL and from Steam's virtual pad reaches the
  launcher (`rumble 49152 16384` for 1.5 s, then 0); playing it on a physical controller is not
  verified yet;
- DualSense passthrough, guest side: a uhid DualSense with the real USB report descriptor, fed by a
  stand-in for the launcher, binds `hid-playstation` (gamepad, touchpad, motion sensors, headset
  jack, RGB and player LEDs); touch position and touchpad click reach the touchpad device, the mute
  button toggles the mute LED through an output report back to the Mac side, and Steam opens
  `/dev/hidraw*` with its HIDAPI driver (`Controller using HIDAPI driver, vid=0x054c, pid=0x0ce6`).
  The Mac side (IOHIDManager, a physical DualSense over USB or Bluetooth) is not verified yet;
- Desktop Mode: Switch to Desktop, the Plasma desktop with mouse input (also letterboxed), Return to
  Gaming Mode, booting straight into the desktop; Flatpak sandboxes start;
- A→B update via official OTA and rollback;
- GL via zink (glamor in Xwayland, glxgears ~60 FPS), Steam UI (gamepad UI, CEF with GPU)
  renders in the VM window;
- Steam sign-in, installation of Proton 11.0-2 (ARM64) and FEX, running a DX11 game (Death's Door)
  through DXVK → Venus → MoltenVK;
- guest resolution follows the window size at constant DPI; fast shutdown (2–4 s);
- Heroes of Might and Magic: Olden Era (Unity, DX11) — 7 minutes without errors (offline test).

Stutters on the first pass are Metal compilation (~50–100 ms per new pipeline); subsequent passes take
~1 ms. On reboot after an abrupt shutdown, initramfs checks and repairs FAT on esp/efi.

